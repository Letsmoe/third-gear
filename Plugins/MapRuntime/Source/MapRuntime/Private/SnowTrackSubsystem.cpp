#include "SnowTrackSubsystem.h"

#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "WorldSnow.h"

namespace SnowTrackConfig
{
	/** The window follows the viewer in steps of this many texels (8 m), so strips are cleared rarely. */
	constexpr int32 ScrollStepTexels = 160;
	/** Below this snow cover wheels leave nothing. */
	constexpr float MinimumCover = 0.1f;
	/** Wheels further than this from the window centre are not stamped (the material fades tracks out before). */
	constexpr float StampRangeCm = 4500.f;
	/** A jump longer than this between two frames is a teleport, not driving. */
	constexpr float MaxSegmentCm = 1000.f;
	/** Fraction of the tracks that disappears per unit of new snow cover. */
	constexpr float FadePerCover = 4.f;
	const TCHAR* const TrackMapPath = TEXT("/Game/World/SnowTracks/RT_SnowTracks");
	/** Texel byte layout of PF_B8G8R8A8. */
	constexpr int32 ChannelDirectionCos = 0;
	constexpr int32 ChannelRidge = 1;
	constexpr int32 ChannelCompaction = 2;
	constexpr int32 ChannelDirectionSin = 3;
}

namespace SnowTrackMath
{
	int64 WrapIndex(int64 WorldIndex, int32 Size)
	{
		const int64 Remainder = WorldIndex % Size;
		return Remainder < 0 ? Remainder + Size : Remainder;
	}

	/** Distance from a point to the segment A-B. */
	float DistanceToSegment(const FVector2D& Point, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D Along = B - A;
		const double LengthSquared = Along.SizeSquared();
		const double T = LengthSquared > UE_SMALL_NUMBER ? FMath::Clamp(FVector2D::DotProduct(Point - A, Along) / LengthSquared, 0.0, 1.0) : 0.0;
		return static_cast<float>(FVector2D::Distance(Point, A + Along * T));
	}
}

bool USnowTrackSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

TStatId USnowTrackSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USnowTrackSubsystem, STATGROUP_Tickables);
}

void USnowTrackSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	TrackMap = LoadObject<UTextureRenderTarget2D>(nullptr, SnowTrackConfig::TrackMapPath);
	if (!TrackMap)
	{
		UE_LOG(LogTemp, Warning, TEXT("SnowTracks: %s missing (run Scripts/create_snow_track_map.py), no tyre tracks"), SnowTrackConfig::TrackMapPath);
		return;
	}
	UploadTexture = UTexture2D::CreateTransient(TextureSize, TextureSize, PF_B8G8R8A8);
	UploadTexture->SRGB = false;
	UploadTexture->Filter = TF_Nearest;
	UploadTexture->MipGenSettings = TMGS_NoMipmaps;
	UploadTexture->NeverStream = true;
	UploadTexture->UpdateResource();
	Pixels.SetNumUninitialized(TextureSize * TextureSize * 4);
	ClearAll();
}

void USnowTrackSubsystem::Deinitialize()
{
	// The render thread may still read Pixels for a queued upload; flush before freeing it.
	FlushRenderingCommands();
	Super::Deinitialize();
}

uint8* USnowTrackSubsystem::TexelAt(int64 WorldX, int64 WorldY)
{
	const int64 X = SnowTrackMath::WrapIndex(WorldX, TextureSize);
	const int64 Y = SnowTrackMath::WrapIndex(WorldY, TextureSize);
	return &Pixels[(Y * TextureSize + X) * 4];
}

void USnowTrackSubsystem::ClearAll()
{
	for (int32 Index = 0; Index < Pixels.Num(); Index += 4)
	{
		Pixels[Index + SnowTrackConfig::ChannelDirectionCos] = 128;
		Pixels[Index + SnowTrackConfig::ChannelRidge] = 0;
		Pixels[Index + SnowTrackConfig::ChannelCompaction] = 0;
		Pixels[Index + SnowTrackConfig::ChannelDirectionSin] = 128;
	}
	DirtyRects.Reset();
	DirtyRects.Add(FIntRect(0, 0, TextureSize, TextureSize));
	bCopyNeeded = true;
}

void USnowTrackSubsystem::ClearColumns(int64 FirstWorldColumn, int64 LastWorldColumn)
{
	for (int64 Column = FirstWorldColumn; Column <= LastWorldColumn; ++Column)
	{
		for (int64 Row = 0; Row < TextureSize; ++Row)
		{
			uint8* Texel = TexelAt(Column, Row);
			Texel[SnowTrackConfig::ChannelDirectionCos] = 128;
			Texel[SnowTrackConfig::ChannelRidge] = 0;
			Texel[SnowTrackConfig::ChannelCompaction] = 0;
			Texel[SnowTrackConfig::ChannelDirectionSin] = 128;
		}
	}
	MarkDirty(FirstWorldColumn, 0, LastWorldColumn, TextureSize - 1);
}

void USnowTrackSubsystem::ClearRows(int64 FirstWorldRow, int64 LastWorldRow)
{
	for (int64 Row = FirstWorldRow; Row <= LastWorldRow; ++Row)
	{
		for (int64 Column = 0; Column < TextureSize; ++Column)
		{
			uint8* Texel = TexelAt(Column, Row);
			Texel[SnowTrackConfig::ChannelDirectionCos] = 128;
			Texel[SnowTrackConfig::ChannelRidge] = 0;
			Texel[SnowTrackConfig::ChannelCompaction] = 0;
			Texel[SnowTrackConfig::ChannelDirectionSin] = 128;
		}
	}
	MarkDirty(0, FirstWorldRow, TextureSize - 1, LastWorldRow);
}

void USnowTrackSubsystem::MarkDirty(int64 MinX, int64 MinY, int64 MaxX, int64 MaxY)
{
	bCopyNeeded = true;
	if (MaxX - MinX + 1 >= TextureSize || MaxY - MinY + 1 >= TextureSize)
	{
		// Wider than the grid: the clear spans a whole axis, which at most wraps once.
		MinX = FMath::Max<int64>(MinX, MaxX - TextureSize + 1);
		MinY = FMath::Max<int64>(MinY, MaxY - TextureSize + 1);
	}
	const int32 StartX = static_cast<int32>(SnowTrackMath::WrapIndex(MinX, TextureSize));
	const int32 StartY = static_cast<int32>(SnowTrackMath::WrapIndex(MinY, TextureSize));
	const int32 WidthX = static_cast<int32>(MaxX - MinX + 1);
	const int32 WidthY = static_cast<int32>(MaxY - MinY + 1);
	const int32 FirstWidthX = FMath::Min(WidthX, TextureSize - StartX);
	const int32 FirstWidthY = FMath::Min(WidthY, TextureSize - StartY);
	DirtyRects.Add(FIntRect(StartX, StartY, StartX + FirstWidthX, StartY + FirstWidthY));
	if (FirstWidthX < WidthX)
	{
		DirtyRects.Add(FIntRect(0, StartY, WidthX - FirstWidthX, StartY + FirstWidthY));
	}
	if (FirstWidthY < WidthY)
	{
		DirtyRects.Add(FIntRect(StartX, 0, StartX + FirstWidthX, WidthY - FirstWidthY));
		if (FirstWidthX < WidthX)
		{
			DirtyRects.Add(FIntRect(0, 0, WidthX - FirstWidthX, WidthY - FirstWidthY));
		}
	}
}

void USnowTrackSubsystem::FollowViewer()
{
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (!Controller || !Controller->PlayerCameraManager)
	{
		return;
	}
	const FVector Viewer = Controller->PlayerCameraManager->GetCameraLocation();
	const int32 Step = SnowTrackConfig::ScrollStepTexels;
	const FIntPoint Wanted(
		FMath::RoundToInt(Viewer.X / TexelSizeCm / Step) * Step,
		FMath::RoundToInt(Viewer.Y / TexelSizeCm / Step) * Step);
	if (Wanted == CenterTexel)
	{
		return;
	}
	const int32 Half = TextureSize / 2;
	const bool bFirst = CenterTexel.X == MAX_int32;
	const int64 DeltaX = bFirst ? TextureSize : static_cast<int64>(Wanted.X) - CenterTexel.X;
	const int64 DeltaY = bFirst ? TextureSize : static_cast<int64>(Wanted.Y) - CenterTexel.Y;
	if (FMath::Abs(DeltaX) >= TextureSize || FMath::Abs(DeltaY) >= TextureSize)
	{
		ClearAll();
	}
	else
	{
		if (DeltaX > 0)
		{
			ClearColumns(CenterTexel.X + Half, Wanted.X + Half - 1);
		}
		else if (DeltaX < 0)
		{
			ClearColumns(Wanted.X - Half, CenterTexel.X - Half - 1);
		}
		if (DeltaY > 0)
		{
			ClearRows(CenterTexel.Y + Half, Wanted.Y + Half - 1);
		}
		else if (DeltaY < 0)
		{
			ClearRows(Wanted.Y - Half, CenterTexel.Y - Half - 1);
		}
	}
	CenterTexel = Wanted;
}

void USnowTrackSubsystem::FadeForNewSnow(float Cover)
{
	if (Cover < ReferenceCover)
	{
		ReferenceCover = Cover;
		return;
	}
	if (Cover - ReferenceCover < 0.02f)
	{
		return;
	}
	const float Keep = FMath::Clamp(1.f - (Cover - ReferenceCover) * SnowTrackConfig::FadePerCover, 0.f, 1.f);
	ReferenceCover = Cover;
	ParallelFor(TextureSize, [this, Keep](int32 Row)
	{
		uint8* RowStart = &Pixels[static_cast<int64>(Row) * TextureSize * 4];
		for (int32 Column = 0; Column < TextureSize; ++Column)
		{
			uint8* Texel = RowStart + Column * 4;
			Texel[SnowTrackConfig::ChannelRidge] = static_cast<uint8>(Texel[SnowTrackConfig::ChannelRidge] * Keep);
			Texel[SnowTrackConfig::ChannelCompaction] = static_cast<uint8>(Texel[SnowTrackConfig::ChannelCompaction] * Keep);
		}
	});
	DirtyRects.Reset();
	DirtyRects.Add(FIntRect(0, 0, TextureSize, TextureSize));
	bCopyNeeded = true;
}

void USnowTrackSubsystem::StampWheel(const void* Key, const FVector& WheelLocationCm, float TyreWidthCm)
{
	if (!TrackMap || CenterTexel.X == MAX_int32)
	{
		return;
	}
	const FVector2D Location(WheelLocationCm.X, WheelLocationCm.Y);
	FWheelHistory& History = Wheels.FindOrAdd(Key);
	const double Now = GetWorld()->GetTimeSeconds();
	const bool bHasHistory = Now - History.LastStampTime < 0.25;
	const FVector2D From = bHasHistory ? History.LastLocationCm : Location;
	History.LastLocationCm = Location;
	History.LastStampTime = Now;

	const FVector2D CenterCm(CenterTexel.X * TexelSizeCm, CenterTexel.Y * TexelSizeCm);
	const bool bSnowing = SnowCover > SnowTrackConfig::MinimumCover;
	const bool bInRange = FVector2D::Distance(Location, CenterCm) < SnowTrackConfig::StampRangeCm;
	const bool bTeleported = FVector2D::Distance(From, Location) > SnowTrackConfig::MaxSegmentCm;
	const bool bMoved = (From - Location).SizeSquared() > 1.f;
	if (!bSnowing || !bInRange || bTeleported || !bMoved)
	{
		return;
	}
	PressSegment(From, Location, TyreWidthCm * 0.5f);
}

void USnowTrackSubsystem::PressSegment(const FVector2D& FromCm, const FVector2D& ToCm, float HalfWidthCm)
{
	// Ridges of pushed aside snow sit just outside the tyre and reach about 10 cm further.
	const float RidgeCentreCm = HalfWidthCm + 6.f;
	const float ReachCm = RidgeCentreCm + 8.f;
	const FVector2D Direction = (ToCm - FromCm).GetSafeNormal();
	const uint8 DirectionCos = static_cast<uint8>(FMath::Clamp(Direction.X * 127.f + 128.f, 0.f, 255.f));
	const uint8 DirectionSin = static_cast<uint8>(FMath::Clamp(Direction.Y * 127.f + 128.f, 0.f, 255.f));

	const int64 MinX = FMath::FloorToInt64((FMath::Min(FromCm.X, ToCm.X) - ReachCm) / TexelSizeCm);
	const int64 MaxX = FMath::FloorToInt64((FMath::Max(FromCm.X, ToCm.X) + ReachCm) / TexelSizeCm);
	const int64 MinY = FMath::FloorToInt64((FMath::Min(FromCm.Y, ToCm.Y) - ReachCm) / TexelSizeCm);
	const int64 MaxY = FMath::FloorToInt64((FMath::Max(FromCm.Y, ToCm.Y) + ReachCm) / TexelSizeCm);
	for (int64 TexelY = MinY; TexelY <= MaxY; ++TexelY)
	{
		for (int64 TexelX = MinX; TexelX <= MaxX; ++TexelX)
		{
			const FVector2D Centre((TexelX + 0.5) * TexelSizeCm, (TexelY + 0.5) * TexelSizeCm);
			const float Distance = SnowTrackMath::DistanceToSegment(Centre, FromCm, ToCm);
			const float Compaction = 1.f - FMath::SmoothStep(HalfWidthCm - 2.5f, HalfWidthCm + 2.5f, Distance);
			const float RidgeOffset = (Distance - RidgeCentreCm) / 4.5f;
			const float Ridge = FMath::Exp(-RidgeOffset * RidgeOffset);
			uint8* Texel = TexelAt(TexelX, TexelY);
			const uint8 NewCompaction = FMath::Max<uint8>(Texel[SnowTrackConfig::ChannelCompaction], static_cast<uint8>(Compaction * 255.f));
			// A later pass over a ridge flattens it again; ridges only stand where nothing is pressed.
			const float RidgeHeight = FMath::Max(Texel[SnowTrackConfig::ChannelRidge] / 255.f, Ridge) * (1.f - NewCompaction / 255.f);
			Texel[SnowTrackConfig::ChannelCompaction] = NewCompaction;
			Texel[SnowTrackConfig::ChannelRidge] = static_cast<uint8>(RidgeHeight * 255.f);
			if (Compaction > 0.5f)
			{
				Texel[SnowTrackConfig::ChannelDirectionCos] = DirectionCos;
				Texel[SnowTrackConfig::ChannelDirectionSin] = DirectionSin;
			}
		}
	}
	MarkDirty(MinX, MinY, MaxX, MaxY);
}

void USnowTrackSubsystem::UploadAndCopy()
{
	if (DirtyRects.IsEmpty() && !bCopyNeeded)
	{
		return;
	}
	if (!DirtyRects.IsEmpty())
	{
		// The render thread reads from Pixels (stable storage); the regions array is freed when the upload is done.
		TArray<FUpdateTextureRegion2D>* Regions = new TArray<FUpdateTextureRegion2D>();
		for (const FIntRect& Rect : DirtyRects)
		{
			Regions->Add(FUpdateTextureRegion2D(Rect.Min.X, Rect.Min.Y, Rect.Min.X, Rect.Min.Y, Rect.Width(), Rect.Height()));
		}
		UploadTexture->UpdateTextureRegions(0, Regions->Num(), Regions->GetData(), TextureSize * 4, 4, Pixels.GetData(),
			[Regions](uint8*, const FUpdateTextureRegion2D*) { delete Regions; });
		DirtyRects.Reset();
	}
	UCanvas* Canvas = nullptr;
	FVector2D Size;
	FDrawToRenderTargetContext Context;
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(this, TrackMap, Canvas, Size, Context);
	if (Canvas)
	{
		Canvas->K2_DrawTexture(UploadTexture, FVector2D::ZeroVector, Size, FVector2D::ZeroVector, FVector2D::UnitVector, FLinearColor::White, BLEND_Opaque);
	}
	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(this, Context);
	bCopyNeeded = false;
}

void USnowTrackSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!TrackMap)
	{
		return;
	}
	SnowCover = ReadWorldSnowCover(GetWorld());
	FadeCheckCountdown -= DeltaTime;
	if (FadeCheckCountdown <= 0.f)
	{
		FadeCheckCountdown = 1.f;
		FadeForNewSnow(SnowCover);
	}
	FollowViewer();
	UploadAndCopy();
	if (Wheels.Num() > 64)
	{
		const double Now = GetWorld()->GetTimeSeconds();
		for (auto It = Wheels.CreateIterator(); It; ++It)
		{
			if (Now - It.Value().LastStampTime > 2.0)
			{
				It.RemoveCurrent();
			}
		}
	}
}
