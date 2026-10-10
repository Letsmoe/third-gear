#include "Map3DSubsystem.h"

#include "Async/Async.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "ImageUtils.h"
#include "Map3DMeshBuilder.h"
#include "Map3DMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MapPalette.h"
#include "MinimapSubsystem.h"
#include "SMinimapWidgets.h"
#include "PreviewScene.h"
#include "Rendering/DrawElements.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/SlateBrush.h"
#include "WorldStreamer.h"
#include "WorldTileData.h"

DEFINE_LOG_CATEGORY_STATIC(LogMap3D, Log, All);

namespace
{
TAutoConsoleVariable<float> CVarSupersample(TEXT("tg.Map3D.Supersample"), 1.5f,
	TEXT("Size of the 3D map picture relative to the pixels it is shown in; the extra pixels smooth the edges."), ECVF_Default);
TAutoConsoleVariable<float> CVarMinimapTilt(TEXT("tg.Map3D.MinimapTiltDegrees"), 45.f,
	TEXT("Tilt of the 3D minimap from straight down, degrees."), ECVF_Default);
TAutoConsoleVariable<float> CVarMinimapHz(TEXT("tg.Map3D.MinimapHz"), 30.f,
	TEXT("How often the 3D minimap picture is rendered, per second."), ECVF_Default);
TAutoConsoleVariable<float> CVarNearRadius(TEXT("tg.Map3D.NearRadius"), 700.f,
	TEXT("Tiles closer than this to the view's centre get roofs, markings, trees and traffic lights, metres."), ECVF_Default);
TAutoConsoleVariable<float> CVarLoadRadius(TEXT("tg.Map3D.LoadRadius"), 3200.f,
	TEXT("Tiles farther than this from the view's centre are never loaded, metres."), ECVF_Default);

constexpr float TileSizeMeters = 250.f;
constexpr float CentimetresPerMetre = 100.f;
constexpr float HorizontalFovDegrees = 40.f;

/** Where the car sits in the minimap picture, as a fraction of the half height below the centre. */
constexpr float MinimapCarScreenOffset = 0.5f;

constexpr float IdleCaptureSeconds = 0.5f;
constexpr int32 MaxTilesInFlight = 6;
constexpr int32 MaxResidentTiles = 450;
constexpr double KeepTileSeconds = 4.0;

/** A tile's CPU copy of its mesh is dropped this long after the upload, when the render buffers certainly exist. */
constexpr double DiscardMeshAfterSeconds = 2.0;
constexpr double MaxUploadSecondsPerFrame = 0.003;

/** The full map tilts between flat and this, radians; a tilt below the first is flat (2D). */
constexpr float FullMapMinTilt = 0.05f;
constexpr float FullMapMaxTilt = 1.08f;
constexpr float DefaultFullMapTilt = 0.87f;
constexpr float OrbitRadiansPerPixel = 0.005f;
constexpr float NorthUpYaw = -HALF_PI;

/** The top of the picture fades to the land colour over this share of its height. */
constexpr float DistanceFadeShare = 0.16f;

/** Tiles are loaded this far beyond the picture's edge, and every tile this close to the view's centre is loaded regardless. */
constexpr double FootprintMarginMeters = 140.0;
constexpr double AlwaysLoadRadiusMeters = 300.0;

constexpr float GroundHalfSizeMeters = 90000.f;
constexpr float RouteHeightMeters = 0.35f;

/** A palette colour given as sRGB hex. */
FColor Hex(uint32 Rgb)
{
	return FColor((Rgb >> 16) & 0xFF, (Rgb >> 8) & 0xFF, Rgb & 0xFF, 255);
}

FColor Mix(const FColor& From, const FColor& To, float Amount)
{
	return FMath::Lerp(FLinearColor::FromSRGBColor(From), FLinearColor::FromSRGBColor(To), Amount).ToFColorSRGB();
}

/** The colour with every channel scaled in linear space. */
FColor Brighten(const FColor& Color, float Factor)
{
	FLinearColor Linear = FLinearColor::FromSRGBColor(Color) * Factor;
	Linear.A = 1.f;
	return Linear.ToFColorSRGB();
}

/**
 * The 3D map's colours from the 2D palette, so both agree: land, water, roads, buildings, route and waypoint come from it;
 * markings, trees, traffic lights and the car are the 3D map's own. Dark walls are lighter than the roofs, light walls get
 * their darker look from the per-face shading.
 */
void FillPalette(const FMapPalette& Source, FColor (&OutColors)[static_cast<int32>(EMap3DColor::Count)])
{
	const float WallBoost = Source.bDark ? 1.3f : 1.f;
	const auto Set = [&OutColors](EMap3DColor Slot, const FColor& Color) { OutColors[static_cast<int32>(Slot)] = Color; };
	Set(EMap3DColor::Ground, Source.Land);
	Set(EMap3DColor::Green, Source.Forest);
	Set(EMap3DColor::Water, Source.Water);
	Set(EMap3DColor::Footway, Mix(Source.Land, Source.Road[2], 0.5f));
	Set(EMap3DColor::Road, Source.Road[2]);
	Set(EMap3DColor::Marking, Source.bDark ? Hex(0xE6ECF8) : Hex(0xFFFFFF));
	Set(EMap3DColor::RoofResidential, Source.Residential);
	Set(EMap3DColor::RoofCommercial, Source.Commercial);
	Set(EMap3DColor::RoofIndustrial, Source.Industrial);
	Set(EMap3DColor::WallResidential, Brighten(Source.Residential, WallBoost));
	Set(EMap3DColor::WallCommercial, Brighten(Source.Commercial, WallBoost));
	Set(EMap3DColor::WallIndustrial, Brighten(Source.Industrial, WallBoost));
	Set(EMap3DColor::Trunk, Source.bDark ? Hex(0x1E3A4A) : Hex(0xA39B8E));
	Set(EMap3DColor::Crown, Source.bDark ? Hex(0x0E5A63) : Hex(0xB9DDA9));
	Set(EMap3DColor::Route, Source.Route);
	Set(EMap3DColor::SignalHousing, Source.bDark ? Hex(0x20263A) : Hex(0x3C4043));
	Set(EMap3DColor::CarBody, Hex(0xFFFFFF));
	Set(EMap3DColor::CarGlass, Source.bDark ? Hex(0x1B2540) : Hex(0x5F6B7A));
	Set(EMap3DColor::PinRed, Source.Waypoint);
	Set(EMap3DColor::PinWhite, Hex(0xFFFFFF));
	Set(EMap3DColor::SignalRed, Hex(0xEA4335));
	Set(EMap3DColor::SignalYellow, Hex(0xFBBC04));
	Set(EMap3DColor::SignalGreen, Hex(0x34A853));
}

/** The ground a picture shows: its four corners in order around it, their bounds and which way round they run. */
struct FFootprint
{
	FVector2d Corners[4];
	FBox2D Bounds = FBox2D(ForceInit);
	double Orientation = 1.0;

	/** Whether a ground point lies inside the quadrilateral, or within Margin metres outside it. */
	bool Contains(const FVector2d& Point, double Margin) const
	{
		for (int32 Edge = 0; Edge < 4; ++Edge)
		{
			const FVector2d From = Corners[Edge];
			const FVector2d Along = Corners[(Edge + 1) % 4] - From;
			const double Distance = Orientation * (Along.X * (Point.Y - From.Y) - Along.Y * (Point.X - From.X)) / FMath::Max(Along.Size(), 1e-3);
			if (Distance < -Margin)
			{
				return false;
			}
		}
		return true;
	}
};

/** The footprint of a camera's picture on the ground, pulled in to LoadRadius around the target where it runs to the horizon. */
FFootprint MakeFootprint(const FMap3DCamera& Camera, float LoadRadius)
{
	const FVector2f Ndc[4] = {FVector2f(-1.f, -1.f), FVector2f(1.f, -1.f), FVector2f(1.f, 1.f), FVector2f(-1.f, 1.f)};
	FFootprint Footprint;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FVector2d Ground;
		if (!Camera.GroundFromNdc(Ndc[Index], Ground))
		{
			const FVector Forward = Camera.GetForward();
			Ground = Camera.TargetMeters + FVector2d(Forward.X, Forward.Y).GetSafeNormal() * LoadRadius;
		}
		const FVector2d Offset = Ground - Camera.TargetMeters;
		Footprint.Corners[Index] = Offset.Size() > LoadRadius ? Camera.TargetMeters + Offset.GetSafeNormal() * LoadRadius : Ground;
		Footprint.Bounds += Footprint.Corners[Index];
	}
	double Area = 0.0;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FVector2d& From = Footprint.Corners[Index];
		const FVector2d& To = Footprint.Corners[(Index + 1) % 4];
		Area += From.X * To.Y - To.X * From.Y;
	}
	Footprint.Orientation = Area >= 0.0 ? 1.0 : -1.0;
	return Footprint;
}

/** The screen's field-of-view slope for the aspect ratio of the picture: tan of half the vertical angle. */
double HalfHeightSlope(const FMap3DCamera& Camera)
{
	return Camera.HalfWidthSlope() * Camera.ImageSize.Y / FMath::Max(1, Camera.ImageSize.X);
}

uint32 HashCamera(const FMap3DCamera& Camera)
{
	uint32 Hash = GetTypeHash(FMath::RoundToInt(Camera.TargetMeters.X * 20.0));
	Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Camera.TargetMeters.Y * 20.0)));
	Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Camera.YawRadians * 2000.f)));
	Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Camera.TiltRadians * 2000.f)));
	Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Camera.DistanceMeters * 20.f)));
	return HashCombine(Hash, GetTypeHash(Camera.ImageSize));
}
}

// ---------------------------------------------------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------------------------------------------------

double FMap3DCamera::HalfWidthSlope() const
{
	return FMath::Tan(FMath::DegreesToRadians(HorizontalFovDegrees) * 0.5);
}

FVector FMap3DCamera::GetForward() const
{
	const double Depression = HALF_PI - TiltRadians;
	return FVector(FMath::Cos(Depression) * FMath::Cos(YawRadians), FMath::Cos(Depression) * FMath::Sin(YawRadians), -FMath::Sin(Depression));
}

FVector FMap3DCamera::GetRight() const
{
	return FVector(-FMath::Sin(YawRadians), FMath::Cos(YawRadians), 0.0);
}

FVector FMap3DCamera::GetUp() const
{
	return FVector::CrossProduct(GetForward(), GetRight());
}

FVector FMap3DCamera::GetLocationMeters() const
{
	const FVector Target(TargetMeters.X, TargetMeters.Y, 0.0);
	return Target - GetForward() * DistanceMeters;
}

bool FMap3DCamera::GroundFromNdc(const FVector2f& Ndc, FVector2d& OutMeters) const
{
	const double Aspect = static_cast<double>(ImageSize.X) / FMath::Max(1, ImageSize.Y);
	const double Slope = HalfWidthSlope();
	const FVector Direction = GetForward() + GetRight() * (Ndc.X * Slope) + GetUp() * (Ndc.Y * Slope / Aspect);
	if (Direction.Z > -1e-4)
	{
		return false;
	}
	const FVector Location = GetLocationMeters();
	const double Along = -Location.Z / Direction.Z;
	OutMeters = FVector2d(Location.X + Direction.X * Along, Location.Y + Direction.Y * Along);
	return true;
}

bool FMap3DCamera::NdcFromWorld(const FVector& PointMeters, FVector2f& OutNdc) const
{
	const FVector Relative = PointMeters - GetLocationMeters();
	const double Depth = FVector::DotProduct(Relative, GetForward());
	if (Depth < 0.1)
	{
		return false;
	}
	const double Aspect = static_cast<double>(ImageSize.X) / FMath::Max(1, ImageSize.Y);
	const double Slope = HalfWidthSlope();
	OutNdc = FVector2f(FVector::DotProduct(Relative, GetRight()) / (Depth * Slope), FVector::DotProduct(Relative, GetUp()) * Aspect / (Depth * Slope));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------------

bool UMap3DSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	const bool bIsPlayableWorld = World && World->IsGameWorld() && World->GetNetMode() != NM_DedicatedServer;
	return bIsPlayableWorld && !IsRunningCommandlet() && !FParse::Param(FCommandLine::Get(), TEXT("NoMinimap")) && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMap3DSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMap3DSubsystem, STATGROUP_Tickables);
}

void UMap3DSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	bDisabled = UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled();
	bMinimapPreferred3D = FParse::Param(FCommandLine::Get(), TEXT("Map3D"));
	bStats = FParse::Param(FCommandLine::Get(), TEXT("Map3DStats"));
	FullMapYawRadians = NorthUpYaw;
	float TiltDegrees = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("MapTilt="), TiltDegrees))
	{
		FullMapTiltRadians = FMath::Clamp(FMath::DegreesToRadians(TiltDegrees), 0.f, FullMapMaxTilt);
	}
	float RotationDegrees = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("MapRotation="), RotationDegrees))
	{
		FullMapYawRadians = NorthUpYaw + FMath::DegreesToRadians(RotationDegrees);
	}
}

void UMap3DSubsystem::Deinitialize()
{
	for (TPair<FIntPoint, FPendingTile>& Entry : Pending)
	{
		Entry.Value.Result.Wait();
	}
	Pending.Reset();
	if (IndexLoad.IsValid())
	{
		IndexLoad.Wait();
	}
	Tiles.Reset();
	Scene.Reset();
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------------------------------------------------

void UMap3DSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (bDisabled || bSceneFailed)
	{
		return;
	}
	if (!Minimap.IsValid())
	{
		Minimap = GetWorld()->GetSubsystem<UMinimapSubsystem>();
	}
	if (!Minimap.IsValid())
	{
		return;
	}
	const double TickStart = FPlatformTime::Seconds();
	ApplyTestSwitches();
	UpdateModes();
	if (!bMinimapActive && !bFullMapActive)
	{
		return;
	}
	if (!EnsureScene())
	{
		return;
	}
	double PhaseStart = FPlatformTime::Seconds();
	StartIndexLoad();
	PollIndexLoad();
	UpdatePalette();
	RecordPhase(0, PhaseStart);

	const FMap3DCamera Camera = bFullMapActive ? MakeFullMapCamera() : MakeMinimapCamera();
	UpdateTiles(Camera);
	RecordPhase(1, PhaseStart);
	UpdateRoute();
	UpdateMarkers(Camera);
	RecordPhase(2, PhaseStart);

	SecondsSinceCapture += DeltaTime;
	const uint32 CameraHash = HashCamera(Camera);
	const bool bChanged = CameraHash != LastCameraHash || bCaptureDirty;
	const float Interval = bMinimapActive ? 1.f / FMath::Max(CVarMinimapHz.GetValueOnGameThread(), 1.f) : (bChanged ? 0.f : IdleCaptureSeconds);
	if (SecondsSinceCapture >= Interval && (bChanged || bMinimapActive || SecondsSinceCapture >= IdleCaptureSeconds))
	{
		SecondsSinceCapture = 0.f;
		LastCameraHash = CameraHash;
		bCaptureDirty = false;
		CaptureView(Camera);
		RecordPhase(3, PhaseStart);
	}
	const double TickSeconds = FPlatformTime::Seconds() - TickStart;
	StatsTickSeconds += TickSeconds;
	StatsWorstTickSeconds = FMath::Max(StatsWorstTickSeconds, TickSeconds);
	++StatsFrames;
	LogStats(DeltaTime);
}

void UMap3DSubsystem::RecordPhase(int32 Phase, double& PhaseStart)
{
	const double Now = FPlatformTime::Seconds();
	StatsPhaseWorstSeconds[Phase] = FMath::Max(StatsPhaseWorstSeconds[Phase], Now - PhaseStart);
	PhaseStart = Now;
}

void UMap3DSubsystem::UpdateModes()
{
	const bool bMinimapShown = Minimap->IsMinimapShown();
	const bool bFullMapOpen = Minimap->IsFullMapOpen();
	bMinimapActive = bMinimapPreferred3D && bMinimapShown && !bFullMapOpen;
	bFullMapActive = bFullMapOpen && FullMapTiltRadians > FullMapMinTilt;
	bDarkPalette = Minimap->GetPalette().bDark;
}

void UMap3DSubsystem::ApplyTestSwitches()
{
	// -MapSpin turns the full map all the time, so every frame needs a new picture (a worst case for measuring).
	static const bool bSpin = FParse::Param(FCommandLine::Get(), TEXT("MapSpin"));
	if (bSpin && Minimap->IsFullMapOpen() && FullMapTiltRadians > FullMapMinTilt)
	{
		FullMapYawRadians += GetWorld()->GetDeltaSeconds() * 0.5f;
	}
	float MetersPerPixel = 0.f;
	if (!bTestMetersDone && Minimap->IsFullMapOpen() && FParse::Value(FCommandLine::Get(), TEXT("MapMetersPerPixel="), MetersPerPixel) && MetersPerPixel > 0.f)
	{
		bTestMetersDone = true;
		// The full map zooms by a factor of 0.8 per wheel notch.
		const float Notches = FMath::Loge(MetersPerPixel / Minimap->GetFullMapMetersPerPixel()) / FMath::Loge(0.8f);
		Minimap->ZoomFullMap(Notches, FVector2f::ZeroVector);
	}
}

void UMap3DSubsystem::ToggleMode(bool bFullMap)
{
	if (!bFullMap)
	{
		bMinimapPreferred3D = !bMinimapPreferred3D;
		return;
	}
	FullMapTiltRadians = FullMapTiltRadians > FullMapMinTilt ? 0.f : DefaultFullMapTilt;
	if (FullMapTiltRadians == 0.f)
	{
		FullMapYawRadians = NorthUpYaw;
	}
}

void UMap3DSubsystem::OrbitFullMap(const FVector2f& CursorDeltaPixels)
{
	FullMapYawRadians += CursorDeltaPixels.X * OrbitRadiansPerPixel;
	const float Tilt = FullMapTiltRadians - CursorDeltaPixels.Y * OrbitRadiansPerPixel;
	FullMapTiltRadians = FMath::Clamp(Tilt, 0.f, FullMapMaxTilt);
	if (FullMapTiltRadians < FullMapMinTilt)
	{
		// Back to the flat map, which is north-up.
		FullMapTiltRadians = 0.f;
		FullMapYawRadians = NorthUpYaw;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Cameras
// ---------------------------------------------------------------------------------------------------------------------

FMap3DCamera UMap3DSubsystem::MakeMinimapCamera() const
{
	FMap3DCamera Camera;
	Camera.HorizontalFovDegrees = HorizontalFovDegrees;
	Camera.TiltRadians = FMath::DegreesToRadians(CVarMinimapTilt.GetValueOnGameThread());
	Camera.YawRadians = Minimap->GetMinimapHeadingRadians();
	const double Slope = Camera.HalfWidthSlope();
	Camera.DistanceMeters = Minimap->GetMinimapRadiusMeters() / Slope;
	// The car sits below the picture's centre, so the target lies ahead of it by the distance that puts it there.
	const double Depression = HALF_PI - Camera.TiltRadians;
	const double Ahead = MinimapCarScreenOffset * Slope * Camera.DistanceMeters / (FMath::Sin(Depression) + MinimapCarScreenOffset * Slope * FMath::Cos(Depression));
	const FVector2f Car = Minimap->GetCarPositionMeters();
	Camera.TargetMeters = FVector2d(Car.X + FMath::Cos(Camera.YawRadians) * Ahead, Car.Y + FMath::Sin(Camera.YawRadians) * Ahead);
	const int32 Side = FMath::RoundToInt(MinimapContentPx * CVarSupersample.GetValueOnGameThread());
	Camera.ImageSize = FIntPoint(Side, Side);
	return Camera;
}

FMap3DCamera UMap3DSubsystem::MakeFullMapCamera() const
{
	FMap3DCamera Camera;
	Camera.HorizontalFovDegrees = HorizontalFovDegrees;
	Camera.TiltRadians = FullMapTiltRadians;
	Camera.YawRadians = FullMapYawRadians;
	const FVector2f Centre = Minimap->GetFullMapCentreMeters();
	Camera.TargetMeters = FVector2d(Centre.X, Centre.Y);
	Camera.DistanceMeters = 0.5 * FullMapWidgetSize.X * Minimap->GetFullMapMetersPerPixel() / Camera.HalfWidthSlope();
	const float Supersample = CVarSupersample.GetValueOnGameThread();
	const float Scale = FMath::Min(Supersample, 4096.f / FMath::Max(FullMapWidgetSize.X, 1.f));
	Camera.ImageSize = FIntPoint(FMath::Max(64, FMath::RoundToInt(FullMapWidgetSize.X * Scale)), FMath::Max(64, FMath::RoundToInt(FullMapWidgetSize.Y * Scale)));
	return Camera;
}

FVector2f UMap3DSubsystem::FullMapScreenToWorld(const FVector2f& OffsetFromCentrePixels) const
{
	const FMap3DCamera Camera = MakeFullMapCamera();
	const FVector2f Ndc(OffsetFromCentrePixels.X / (0.5f * FullMapWidgetSize.X), -OffsetFromCentrePixels.Y / (0.5f * FullMapWidgetSize.Y));
	FVector2d Ground = Camera.TargetMeters;
	Camera.GroundFromNdc(Ndc, Ground);
	return FVector2f(Ground);
}

float UMap3DSubsystem::GetFullMapNorthAngleRadians() const
{
	return FMath::Atan2(-FMath::Cos(FullMapYawRadians), -FMath::Sin(FullMapYawRadians));
}

FVector2f UMap3DSubsystem::FullMapDragToCentreShift(const FVector2f& CursorDeltaPixels) const
{
	return FullMapScreenToWorld(FVector2f::ZeroVector) - FullMapScreenToWorld(CursorDeltaPixels);
}

// ---------------------------------------------------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------------------------------------------------

bool UMap3DSubsystem::EnsureScene()
{
	if (Scene.IsValid())
	{
		return true;
	}
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/UI/M_Map3D.M_Map3D"), nullptr, LOAD_NoWarn);
	UMaterialInterface* OverlayBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/UI/M_Map3DOverlay.M_Map3DOverlay"), nullptr, LOAD_NoWarn);
	if (!Base || !OverlayBase)
	{
		UE_LOG(LogMap3D, Warning, TEXT("/Game/UI/M_Map3D or M_Map3DOverlay is missing; run Scripts/create_map3d_material.py. The 3D map is off."));
		bSceneFailed = true;
		return false;
	}
	FPreviewScene::ConstructionValues Values;
	Values.SetCreateDefaultLighting(false).SetCreatePhysicsScene(false).SetTransactional(false).ShouldSimulatePhysics(false).SetEditor(false);
	Scene = MakeUnique<FPreviewScene>(Values);

	Material = UMaterialInstanceDynamic::Create(Base, this);
	OverlayMaterial = UMaterialInstanceDynamic::Create(OverlayBase, this);
	PaletteTexture = UTexture2D::CreateTransient(Map3DPaletteSize, 1, PF_B8G8R8A8);
	PaletteTexture->Filter = TF_Nearest;
	PaletteTexture->AddressX = TA_Clamp;
	PaletteTexture->AddressY = TA_Clamp;
	PaletteTexture->SRGB = true;
	PaletteTexture->MipGenSettings = TMGS_NoMipmaps;
	PaletteTexture->NeverStream = true;
	Material->SetTextureParameterValue(TEXT("Palette"), PaletteTexture);
	OverlayMaterial->SetTextureParameterValue(TEXT("Palette"), PaletteTexture);
	WritePalette(Minimap->GetPalette());

	Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage());
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->CaptureSource = SCS_SceneColorHDR;
	Capture->bUseRayTracingIfEnabled = false;
	Capture->FOVAngle = HorizontalFovDegrees;
	FEngineShowFlags& Flags = Capture->ShowFlags;
	Flags.SetAntiAliasing(false);
	Flags.SetAmbientOcclusion(false);
	Flags.SetBloom(false);
	Flags.SetMotionBlur(false);
	Flags.SetDepthOfField(false);
	Flags.SetEyeAdaptation(false);
	Flags.SetScreenSpaceReflections(false);
	Flags.SetVolumetricFog(false);
	Flags.SetLumenGlobalIllumination(false);
	Flags.SetLumenReflections(false);
	Flags.SetLensFlares(false);
	Flags.SetGrain(false);
	Flags.SetVignette(false);
	Flags.SetDecals(false);
	Flags.SetParticles(false);
	Flags.SetFog(false);
	Flags.SetAtmosphere(false);
	Flags.SetDynamicShadows(false);
	Flags.SetSkyLighting(false);
	Scene->AddComponent(Capture, FTransform::Identity);

	BuildGround();
	BuildCarMarker();
	BuildPinMarker();
	return true;
}

UMap3DMeshComponent* UMap3DSubsystem::AddMeshComponent(const TSharedPtr<FMap3DMeshData>& Mesh, const FTransform& Transform, bool bOverlay)
{
	UMap3DMeshComponent* Component = NewObject<UMap3DMeshComponent>(GetTransientPackage());
	Component->SetMaterial(0, bOverlay ? OverlayMaterial.Get() : Material.Get());
	Component->SetMeshData(Mesh);
	Scene->AddComponent(Component, Transform);
	return Component;
}

void UMap3DSubsystem::BuildGround()
{
	FMap3DMeshBuilder Builder;
	const FVector2f Min(-GroundHalfSizeMeters, -GroundHalfSizeMeters);
	const FVector2f Max(GroundHalfSizeMeters, GroundHalfSizeMeters);
	Builder.AddQuad(FVector3f(Min.X, Min.Y, -0.02f), FVector3f(Max.X, Min.Y, -0.02f), FVector3f(Max.X, Max.Y, -0.02f), FVector3f(Min.X, Max.Y, -0.02f),
		EMap3DColor::Ground, 1.f, FVector3f::UpVector);
	GroundComponent = AddMeshComponent(Builder.Finish(), FTransform::Identity, /*bOverlay=*/false);
}

void UMap3DSubsystem::BuildCarMarker()
{
	// A small white hatchback, pointing along +x, wider and longer than life so it reads at map scale. The overlay material
	// has no depth test, so the parts are listed from the bottom up and drawn in that order.
	FMap3DMeshBuilder Builder;
	for (const float SignX : {-1.3f, 1.4f})
	{
		for (const float SignY : {-1.f, 1.f})
		{
			Builder.AddBox(FVector3f(SignX, SignY * 0.95f, 0.f), FVector3f(0.4f, 0.12f, 0.35f), 0.f, EMap3DColor::CarGlass, 1.f);
		}
	}
	Builder.AddBox(FVector3f(0.f, 0.f, 0.35f), FVector3f(2.2f, 0.95f, 0.4f), 0.f, EMap3DColor::CarBody, 1.f);
	Builder.AddBox(FVector3f(-0.2f, 0.f, 1.1f), FVector3f(1.2f, 0.8f, 0.35f), 0.f, EMap3DColor::CarGlass, 1.f);
	Builder.AddBox(FVector3f(-0.2f, 0.f, 1.78f), FVector3f(1.1f, 0.75f, 0.02f), 0.f, EMap3DColor::CarBody, 1.f);
	CarComponent = AddMeshComponent(Builder.Finish(), FTransform::Identity, /*bOverlay=*/true);
	CarComponent->SetVisibility(false);
}

void UMap3DSubsystem::BuildPinMarker()
{
	// A red ball on a thin stem, the stem's foot at the waypoint.
	FMap3DMeshBuilder Builder;
	Builder.AddBox(FVector3f(0.f, 0.f, 0.f), FVector3f(0.4f, 0.4f, 8.f), 0.f, EMap3DColor::PinRed, 1.f);
	Builder.AddBlob(FVector3f(0.f, 0.f, 20.f), FVector3f(6.f, 6.f, 6.f), EMap3DColor::PinRed, 1.f);
	Builder.AddBlob(FVector3f(0.f, 0.f, 20.f), FVector3f(2.4f, 2.4f, 2.4f), EMap3DColor::PinWhite, 1.05f);
	PinComponent = AddMeshComponent(Builder.Finish(), FTransform::Identity, /*bOverlay=*/true);
	PinComponent->SetVisibility(false);
}

void UMap3DSubsystem::UpdatePalette()
{
	if (!bPaletteWritten || bDarkPalette != bPaletteIsDark)
	{
		WritePalette(Minimap->GetPalette());
		bCaptureDirty = true;
	}
}

void UMap3DSubsystem::WritePalette(const FMapPalette& Source)
{
	FColor Colors[static_cast<int32>(EMap3DColor::Count)];
	FillPalette(Source, Colors);
	TArray<FColor> Pixels;
	Pixels.Init(FColor::Black, Map3DPaletteSize);
	for (int32 Index = 0; Index < static_cast<int32>(EMap3DColor::Count); ++Index)
	{
		Pixels[Index] = Colors[Index]; // FColor is laid out as BGRA in memory, like the texture
	}
	void* Destination = PaletteTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Destination, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
	PaletteTexture->GetPlatformData()->Mips[0].BulkData.Unlock();
	PaletteTexture->UpdateResource();
	bPaletteWritten = true;
	bPaletteIsDark = Source.bDark;
}

UTextureRenderTarget2D* UMap3DSubsystem::EnsureRenderTarget(TObjectPtr<UTextureRenderTarget2D>& Target, const FIntPoint& Size)
{
	if (Target && Target->SizeX == Size.X && Target->SizeY == Size.Y)
	{
		return Target;
	}
	Target = NewObject<UTextureRenderTarget2D>(this);
	Target->ClearColor = FLinearColor::Black;
	Target->InitCustomFormat(Size.X, Size.Y, PF_B8G8R8A8, false);
	Target->UpdateResourceImmediate(true);
	bHasPicture = false;
	return Target;
}

// ---------------------------------------------------------------------------------------------------------------------
// Tiles
// ---------------------------------------------------------------------------------------------------------------------

void UMap3DSubsystem::StartIndexLoad()
{
	if (bIndexLoadStarted)
	{
		return;
	}
	const AWorldStreamer* Streamer = TActorIterator<AWorldStreamer>(GetWorld()) ? *TActorIterator<AWorldStreamer>(GetWorld()) : nullptr;
	if (!Streamer || Streamer->GetWorldDir().IsEmpty())
	{
		return;
	}
	bIndexLoadStarted = true;
	const FString WorldDir = Streamer->GetWorldDir();
	IndexLoad = Async(EAsyncExecution::ThreadPool, [WorldDir]()
	{
		TMap<FIntPoint, FString> Paths;
		FString Json;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Json, *FPaths::Combine(WorldDir, TEXT("world.json")))
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			return Paths;
		}
		for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("tiles")))
		{
			const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
			const TArray<TSharedPtr<FJsonValue>>& Bounds = Entry->GetArrayField(TEXT("bounds_m"));
			const FIntPoint Key(FMath::RoundToInt(Bounds[0]->AsNumber() / TileSizeMeters), FMath::RoundToInt(Bounds[1]->AsNumber() / TileSizeMeters));
			Paths.Add(Key, FPaths::Combine(WorldDir, Entry->GetStringField(TEXT("file"))));
		}
		return Paths;
	});
}

void UMap3DSubsystem::PollIndexLoad()
{
	if (IndexLoad.IsValid() && IndexLoad.IsReady())
	{
		TilePaths = IndexLoad.Get();
		IndexLoad = TFuture<TMap<FIntPoint, FString>>();
		UE_LOG(LogMap3D, Log, TEXT("3D map tile index: %d tiles"), TilePaths.Num());
	}
}

void UMap3DSubsystem::CollectWantedTiles(const FMap3DCamera& Camera, TArray<TPair<float, FIntPoint>>& OutWanted) const
{
	const float LoadRadius = CVarLoadRadius.GetValueOnGameThread();
	const FFootprint Footprint = MakeFootprint(Camera, LoadRadius);
	const FIntPoint MinKey(FMath::FloorToInt((Footprint.Bounds.Min.X - FootprintMarginMeters) / TileSizeMeters), FMath::FloorToInt((Footprint.Bounds.Min.Y - FootprintMarginMeters) / TileSizeMeters));
	const FIntPoint MaxKey(FMath::FloorToInt((Footprint.Bounds.Max.X + FootprintMarginMeters) / TileSizeMeters), FMath::FloorToInt((Footprint.Bounds.Max.Y + FootprintMarginMeters) / TileSizeMeters));
	for (int32 KeyY = MinKey.Y; KeyY <= MaxKey.Y; ++KeyY)
	{
		for (int32 KeyX = MinKey.X; KeyX <= MaxKey.X; ++KeyX)
		{
			const FVector2d Centre((KeyX + 0.5) * TileSizeMeters, (KeyY + 0.5) * TileSizeMeters);
			const double FromTarget = (Centre - Camera.TargetMeters).Size();
			const bool bInView = Footprint.Contains(Centre, FootprintMarginMeters) && FromTarget < LoadRadius + TileSizeMeters;
			if (bInView || FromTarget < AlwaysLoadRadiusMeters)
			{
				OutWanted.Emplace(static_cast<float>(FromTarget), FIntPoint(KeyX, KeyY));
			}
		}
	}
}

void UMap3DSubsystem::UpdateTiles(const FMap3DCamera& Camera)
{
	CollectFinishedTiles();
	if (TilePaths.IsEmpty())
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	TArray<TPair<float, FIntPoint>> Wanted;
	CollectWantedTiles(Camera, Wanted);
	Wanted.Sort([](const TPair<float, FIntPoint>& A, const TPair<float, FIntPoint>& B) { return A.Key < B.Key; });
	const float NearRadius = CVarNearRadius.GetValueOnGameThread();
	for (const TPair<float, FIntPoint>& Candidate : Wanted)
	{
		const FIntPoint& Key = Candidate.Value;
		if (!TilePaths.Contains(Key))
		{
			continue;
		}
		const EMap3DDetail Detail = Candidate.Key < NearRadius ? EMap3DDetail::Near : EMap3DDetail::Far;
		FTile* Existing = Tiles.Find(Key);
		if (Existing)
		{
			Existing->LastUsedSeconds = Now;
		}
		const bool bNeedsLoad = !Existing || (Existing->Detail == EMap3DDetail::Far && Detail == EMap3DDetail::Near);
		if (bNeedsLoad && !Pending.Contains(Key) && Pending.Num() < MaxTilesInFlight)
		{
			StartTileLoad(Key, Detail);
		}
	}
	DiscardUploadedMeshes();
	EvictTiles();
}

void UMap3DSubsystem::DiscardUploadedMeshes()
{
	const double Now = FPlatformTime::Seconds();
	for (TPair<FIntPoint, FTile>& Entry : Tiles)
	{
		FTile& Tile = Entry.Value;
		if (Tile.Component && !Tile.bMeshDiscarded && Now - Tile.UploadedSeconds > DiscardMeshAfterSeconds)
		{
			Tile.Component->DiscardMeshData();
			Tile.bMeshDiscarded = true;
		}
	}
}

void UMap3DSubsystem::StartTileLoad(const FIntPoint& Key, EMap3DDetail Detail)
{
	const FString Path = TilePaths[Key];
	FPendingTile& Entry = Pending.Add(Key);
	Entry.Detail = Detail;
	Entry.Result = Async(EAsyncExecution::ThreadPool, [Path, Detail]()
	{
		const double BuildStart = FPlatformTime::Seconds();
		TSharedPtr<FBuiltTile> Built = MakeShared<FBuiltTile>();
		Built->Detail = Detail;
		EWorldTileSections Sections = EWorldTileSections::Names | EWorldTileSections::Grid | EWorldTileSections::Surfaces
			| EWorldTileSections::Buildings | EWorldTileSections::BuildingTypes | EWorldTileSections::Roofs;
		if (Detail == EMap3DDetail::Near)
		{
			Sections |= EWorldTileSections::Markings | EWorldTileSections::Plants | EWorldTileSections::Pois;
		}
		FWorldTileData Data;
		FString Error;
		if (!FWorldTileData::Load(Path, Data, Error, Sections))
		{
			UE_LOG(LogMap3D, Warning, TEXT("%s"), *Error);
			return Built;
		}
		FMap3DMeshBuilder Builder;
		Map3D::BuildTile(Data, Detail, Builder);
		if (!Builder.IsEmpty())
		{
			Built->TriangleCount = Builder.NumTriangles();
			Built->Mesh = Builder.Finish();
		}
		Built->BuildMilliseconds = static_cast<float>(1000.0 * (FPlatformTime::Seconds() - BuildStart));
		return Built;
	});
}

void UMap3DSubsystem::CollectFinishedTiles()
{
	const double Start = FPlatformTime::Seconds();
	TArray<FIntPoint> Finished;
	for (TPair<FIntPoint, FPendingTile>& Entry : Pending)
	{
		if (Entry.Value.Result.IsReady())
		{
			Finished.Add(Entry.Key);
		}
	}
	for (const FIntPoint& Key : Finished)
	{
		if (FPlatformTime::Seconds() - Start > MaxUploadSecondsPerFrame)
		{
			break;
		}
		const double UploadStart = FPlatformTime::Seconds();
		const TSharedPtr<FBuiltTile> Built = Pending[Key].Result.Get();
		Pending.Remove(Key);
		FTile& Tile = Tiles.FindOrAdd(Key);
		Tile.Detail = Built->Detail;
		Tile.LastUsedSeconds = FPlatformTime::Seconds();
		Tile.UploadedSeconds = Tile.LastUsedSeconds;
		if (!Built->Mesh.IsValid())
		{
			continue;
		}
		const FTransform Transform(FVector(Key.X * TileSizeMeters * CentimetresPerMetre, Key.Y * TileSizeMeters * CentimetresPerMetre, 0.0));
		if (Tile.Component)
		{
			Tile.Component->SetMeshData(Built->Mesh);
			Tile.bMeshDiscarded = false;
		}
		else
		{
			Tile.Component = AddMeshComponent(Built->Mesh, Transform, /*bOverlay=*/false);
		}
		LoadedTriangleCount += Built->TriangleCount;
		++StatsTilesBuilt;
		if (bStats)
		{
			UE_LOG(LogMap3D, Log, TEXT("Tile %d,%d %s: %d triangles, built in %.1f ms on a worker, uploaded in %.1f ms"), Key.X, Key.Y,
				Built->Detail == EMap3DDetail::Near ? TEXT("near") : TEXT("far"), Built->TriangleCount, Built->BuildMilliseconds, 1000.0 * (FPlatformTime::Seconds() - UploadStart));
		}
		bCaptureDirty = true;
	}
}

void UMap3DSubsystem::EvictTiles()
{
	if (Tiles.Num() <= MaxResidentTiles)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	TArray<TPair<double, FIntPoint>> ByAge;
	for (const TPair<FIntPoint, FTile>& Entry : Tiles)
	{
		if (Now - Entry.Value.LastUsedSeconds > KeepTileSeconds)
		{
			ByAge.Emplace(Entry.Value.LastUsedSeconds, Entry.Key);
		}
	}
	ByAge.Sort([](const TPair<double, FIntPoint>& A, const TPair<double, FIntPoint>& B) { return A.Key < B.Key; });
	const int32 Excess = Tiles.Num() - MaxResidentTiles;
	for (int32 Index = 0; Index < FMath::Min(Excess, ByAge.Num()); ++Index)
	{
		FTile Tile;
		Tiles.RemoveAndCopyValue(ByAge[Index].Value, Tile);
		if (Tile.Component)
		{
			Scene->RemoveComponent(Tile.Component);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Car, waypoint and route
// ---------------------------------------------------------------------------------------------------------------------

void UMap3DSubsystem::UpdateMarkers(const FMap3DCamera& Camera)
{
	// The markers grow with the view's distance so they stay readable, like on a navigation map.
	const float CarScale = FMath::Clamp(static_cast<float>(Camera.DistanceMeters) / 100.f, 1.5f, 20.f);
	const float PinScale = FMath::Clamp(static_cast<float>(Camera.DistanceMeters) / 800.f, 0.7f, 8.f);
	const FVector2f Car = Minimap->GetCarPositionMeters();
	const float YawDegrees = FMath::RadiansToDegrees(Minimap->GetMinimapHeadingRadians());
	CarComponent->SetVisibility(true);
	CarComponent->SetWorldTransform(FTransform(FRotator(0.f, YawDegrees, 0.f), FVector(Car.X, Car.Y, 0.f) * CentimetresPerMetre, FVector(CarScale)));
	FVector2f Waypoint;
	const bool bHasWaypoint = Minimap->GetWaypointMeters(Waypoint);
	PinComponent->SetVisibility(bHasWaypoint);
	if (bHasWaypoint)
	{
		PinComponent->SetWorldTransform(FTransform(FRotator::ZeroRotator, FVector(Waypoint.X, Waypoint.Y, 0.f) * CentimetresPerMetre, FVector(PinScale)));
	}
}

void UMap3DSubsystem::UpdateRoute()
{
	const int32 Version = Minimap->GetRouteVersion();
	if (Version == RouteVersionDrawn)
	{
		return;
	}
	RouteVersionDrawn = Version;
	bCaptureDirty = true;
	const FMinimapRoute& Route = Minimap->GetRoute();
	if (!Route.IsValid())
	{
		if (RouteComponent)
		{
			RouteComponent->SetVisibility(false);
		}
		return;
	}
	const FVector2f Origin = Route.Points[0];
	TArray<FVector2f> Relative;
	Relative.Reserve(Route.Points.Num());
	for (const FVector2f& Point : Route.Points)
	{
		Relative.Add(Point - Origin);
	}
	FMap3DMeshBuilder Builder;
	Builder.AddRibbon(Relative, RouteWidthMeters, RouteHeightMeters, EMap3DColor::Route);
	const FTransform Transform(FVector(Origin.X, Origin.Y, 0.f) * CentimetresPerMetre);
	if (RouteComponent)
	{
		RouteComponent->SetMeshData(Builder.Finish());
		RouteComponent->SetWorldTransform(Transform);
	}
	else
	{
		RouteComponent = AddMeshComponent(Builder.Finish(), Transform, /*bOverlay=*/false);
	}
	RouteComponent->SetVisibility(true);
}

// ---------------------------------------------------------------------------------------------------------------------
// Capture and painting
// ---------------------------------------------------------------------------------------------------------------------

void UMap3DSubsystem::CaptureView(const FMap3DCamera& Camera)
{
	UTextureRenderTarget2D* Image = bFullMapActive ? EnsureRenderTarget(FullMapTarget, Camera.ImageSize) : EnsureRenderTarget(MinimapTarget, Camera.ImageSize);
	const double Start = FPlatformTime::Seconds();
	Capture->TextureTarget = Image;
	Capture->FOVAngle = Camera.HorizontalFovDegrees;
	const FRotator Rotation = Camera.GetForward().Rotation();
	Scene->GetWorld()->SendAllEndOfFrameUpdates();
	Capture->SetWorldLocationAndRotation(Camera.GetLocationMeters() * CentimetresPerMetre, Rotation);
	Scene->GetWorld()->SendAllEndOfFrameUpdates();
	Capture->CaptureScene();
	bHasPicture = true;
	DumpPicture(Image);
	StatsCaptureSeconds += FPlatformTime::Seconds() - Start;
	++StatsCaptures;
}

bool UMap3DSubsystem::PaintPicture(UTextureRenderTarget2D* Image, TSharedPtr<FSlateBrush>& Brush, FSlateWindowElementList& Elements, const FGeometry& Geometry, int32& LayerId)
{
	if (!Image || !bHasPicture)
	{
		return false;
	}
	if (!Brush.IsValid() || Brush->GetResourceObject() != Image)
	{
		Brush = MakeShared<FSlateBrush>();
		Brush->DrawAs = ESlateBrushDrawType::Image;
		Brush->ImageSize = FVector2f(Image->SizeX, Image->SizeY);
		Brush->SetResourceObject(Image);
	}
	FSlateDrawElement::MakeBox(Elements, ++LayerId, Geometry.ToPaintGeometry(), Brush.Get(), ESlateDrawEffect::IgnoreTextureAlpha, FLinearColor::White);
	PaintDistanceFade(Elements, Geometry, LayerId);
	return true;
}

void UMap3DSubsystem::PaintDistanceFade(FSlateWindowElementList& Elements, const FGeometry& Geometry, int32& LayerId) const
{
	// The far edge dissolves into the land colour, as on a navigation map, which hides tiles that have not loaded yet.
	const FLinearColor Land = FMapPalette::Linear(Minimap->GetPalette().Land);
	const float Height = Geometry.GetLocalSize().Y;
	TArray<FSlateGradientStop> Stops;
	Stops.Emplace(FVector2f(0.f, 0.f), Land);
	Stops.Emplace(FVector2f(0.f, Height * DistanceFadeShare), FLinearColor(Land.R, Land.G, Land.B, 0.f));
	FSlateDrawElement::MakeGradient(Elements, ++LayerId, Geometry.ToPaintGeometry(), MoveTemp(Stops), Orient_Vertical, ESlateDrawEffect::None);
}

bool UMap3DSubsystem::PaintMinimap(FSlateWindowElementList& Elements, const FGeometry& Geometry, int32& LayerId)
{
	return bMinimapActive && PaintPicture(MinimapTarget, MinimapBrush, Elements, Geometry, LayerId);
}

bool UMap3DSubsystem::PaintFullMap(FSlateWindowElementList& Elements, const FGeometry& Geometry, int32& LayerId)
{
	FullMapWidgetSize = FVector2f(Geometry.GetLocalSize());
	return bFullMapActive && PaintPicture(FullMapTarget, FullMapBrush, Elements, Geometry, LayerId);
}

void UMap3DSubsystem::DumpPicture(UTextureRenderTarget2D* Image)
{
	static const bool bDump = FParse::Param(FCommandLine::Get(), TEXT("Map3DDump"));
	if (!bDump)
	{
		return;
	}
	// Once the scene has settled: the tiles are built a second or two after the first capture.
	if (++CapturesSinceDump != 150 && CapturesSinceDump != 450)
	{
		return;
	}
	const FString Path = FPaths::ProjectSavedDir() / FString::Printf(TEXT("Screenshots/map3d_%s_%d.png"), bFullMapActive ? TEXT("full") : TEXT("minimap"), CapturesSinceDump);
	FImage Pixels;
	if (FImageUtils::GetRenderTargetImage(Image, Pixels))
	{
		FImageUtils::SaveImageByExtension(*Path, Pixels);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------------------------------------------------

void UMap3DSubsystem::LogStats(float DeltaTime)
{
	StatsWindowSeconds += DeltaTime;
	if (!bStats || StatsWindowSeconds < 5.f || StatsFrames == 0)
	{
		return;
	}
	UE_LOG(LogMap3D, Log, TEXT("Map3D %s: tick avg %.2f ms (worst %.2f), capture call avg %.3f ms x %d, tiles %d resident, %d pending, %d built, %d triangles"),
		bFullMapActive ? TEXT("full map") : TEXT("minimap"), 1000.0 * StatsTickSeconds / StatsFrames, 1000.0 * StatsWorstTickSeconds,
		StatsCaptures > 0 ? 1000.0 * StatsCaptureSeconds / StatsCaptures : 0.0, StatsCaptures, Tiles.Num(), Pending.Num(), StatsTilesBuilt, LoadedTriangleCount);
	UE_LOG(LogMap3D, Log, TEXT("Map3D process memory %.0f MB"), FPlatformMemory::GetStats().UsedPhysical / (1024.0 * 1024.0));
	UE_LOG(LogMap3D, Log, TEXT("Map3D worst phase: setup %.2f ms, tiles %.2f ms, route and markers %.2f ms, capture %.2f ms"),
		1000.0 * StatsPhaseWorstSeconds[0], 1000.0 * StatsPhaseWorstSeconds[1], 1000.0 * StatsPhaseWorstSeconds[2], 1000.0 * StatsPhaseWorstSeconds[3]);
	FMemory::Memzero(StatsPhaseWorstSeconds);
	StatsTickSeconds = 0.0;
	StatsCaptureSeconds = 0.0;
	StatsWorstTickSeconds = 0.0;
	StatsFrames = 0;
	StatsCaptures = 0;
	StatsWindowSeconds = 0.f;
}
