#include "MinimapSubsystem.h"

#include "AITrafficSubsystem.h"
#include "Async/Async.h"
#include "CarPawn.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "GameMenuSubsystem.h"
#include "Map3DSubsystem.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "EngineUtils.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "SMinimapWidgets.h"
#include "Slate/WidgetRenderer.h"
#include "WeatherVisuals.h"
#include "WorldStreamer.h"
#include "Widgets/Layout/SBox.h"
#include "WheelInputSettings.h"
#include "WheelInputSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogMinimap, Log, All);

namespace
{
/** Visible radius of the minimap at each zoom step, metres; the default is the second. */
const float ZoomRadiiMeters[] = {100.f, 200.f, 400.f, 800.f};
constexpr int32 DefaultZoomStep = 1;

/** Speed of the zoom animation (larger is faster), per second. */
constexpr float ZoomAnimationRate = 9.f;

constexpr int32 MinimapZOrder = 50;
constexpr int32 FullMapZOrder = 90;

/** The minimap is a square, so its corners are this much farther from the centre than its sides. */
constexpr float SquareCornerFactor = 1.42f;

constexpr float MinimapRedrawSeconds = 1.f / 30.f;

/** Largest distance of a car from a lane for the car to count as being on it, metres. */
constexpr float CarLaneSearchMeters = 60.f;

/** Route checks run this often, seconds. */
constexpr float ProgressCheckSeconds = 0.25f;

/** A car farther than this from the route for OffRouteSecondsLimit gets a new route, metres and seconds. */
constexpr float OffRouteDistanceMeters = 30.f;
constexpr float OffRouteSecondsLimit = 2.f;

/** The route ends when this close to its end, metres. */
constexpr float ArrivalMeters = 20.f;

constexpr float FullMapMinMetersPerPixel = 0.3f;
constexpr float FullMapMaxMetersPerPixel = 5.f;

/** Buildings are drawn on the full map up to this many metres per pixel, and the map loads tiles up to this radius, metres. */
constexpr float FullMapBuildingsMaxMetersPerPixel = 1.6f;
constexpr float FullMapMaxLoadRadiusMeters = 5500.f;

/** Which roads the full map shows: the classes MinTierForRadius gives for this many pixels' worth of metres (400 px of the view). */
constexpr float FullMapRoadDetailMeters = 400.f;
constexpr float FullMapZoomPerNotch = 0.8f;

/** Seconds before a failed route search is tried again. */
constexpr float RetrySeconds = 5.f;

bool IsPlusKey(const FKey& Key, uint32 Character)
{
	return Key == EKeys::Add || Key == EKeys::Equals || Character == TEXT('+');
}

bool IsMinusKey(const FKey& Key, uint32 Character)
{
	return Key == EKeys::Subtract || Key == EKeys::Hyphen || Character == TEXT('-');
}
}

/** Gives the subsystem the key presses before the viewport and the game see them. */
class FMinimapInputProcessor : public IInputProcessor
{
public:
	explicit FMinimapInputProcessor(UMinimapSubsystem* InOwner)
		: Owner(InOwner)
	{
	}

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
	{
		if (InKeyEvent.IsRepeat())
		{
			return false;
		}
		return Owner.IsValid() && Owner->HandleKeyDown(InKeyEvent.GetKey(), InKeyEvent.GetCharacter());
	}

	virtual const TCHAR* GetDebugName() const override { return TEXT("Minimap"); }

private:
	TWeakObjectPtr<UMinimapSubsystem> Owner;
};

// ---------------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------------

bool UMinimapSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	const bool bIsPlayableWorld = World && World->IsGameWorld() && World->GetNetMode() != NM_DedicatedServer;
	return bIsPlayableWorld && !IsRunningCommandlet() && !FParse::Param(FCommandLine::Get(), TEXT("NoMinimap")) && Super::ShouldCreateSubsystem(Outer);
}

TStatId UMinimapSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMinimapSubsystem, STATGROUP_Tickables);
}

void UMinimapSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	// The widgets live in the flat game viewport; in VR they would not be seen.
	bDisabled = UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled() || !FSlateApplication::IsInitialized();
	if (bDisabled)
	{
		return;
	}
	ZoomStep = DefaultZoomStep;
	float TestRadius = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("MinimapRadius="), TestRadius) && TestRadius > 10.f)
	{
		TargetRadiusMeters = TestRadius;
	}
	else
	{
		TargetRadiusMeters = ZoomRadiiMeters[ZoomStep];
	}
	DisplayRadiusMeters = TargetRadiusMeters;
	InputProcessor = MakeShared<FMinimapInputProcessor>(this);
	FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
}

void UMinimapSubsystem::Deinitialize()
{
	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
	if (IndexBuild.IsValid())
	{
		IndexBuild.Wait();
	}
	if (RouteSearch.IsValid())
	{
		RouteSearch.Wait();
	}
	if (GEngine && GEngine->GameViewport)
	{
		if (MinimapContainer.IsValid())
		{
			GEngine->GameViewport->RemoveViewportWidgetContent(MinimapContainer.ToSharedRef());
		}
		if (FullMapWidget.IsValid())
		{
			GEngine->GameViewport->RemoveViewportWidgetContent(FullMapWidget.ToSharedRef());
		}
	}
	MinimapContainer.Reset();
	MinimapWidget.Reset();
	MinimapContent.Reset();
	MinimapRenderer.Reset();
	FullMapWidget.Reset();
	Super::Deinitialize();
}

void UMinimapSubsystem::EnsureMinimapWidget()
{
	if (MinimapContainer.IsValid() || !GEngine || !GEngine->GameViewport)
	{
		return;
	}
	MinimapImage = NewObject<UTextureRenderTarget2D>(this);
	MinimapImage->ClearColor = FLinearColor::Transparent;
	MinimapImage->InitCustomFormat(static_cast<int32>(MinimapContentPx), static_cast<int32>(MinimapContentPx), PF_B8G8R8A8, false);
	MinimapImage->UpdateResourceImmediate(true);
	MinimapRenderer = MakeShared<FWidgetRenderer>(false, true);
	MinimapContent = SNew(SMinimapContent, this);
	MinimapWidget = SNew(SMinimapWidget, MinimapImage.Get());
	MinimapContainer = SNew(SBox)
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Bottom)
		.Padding(10.f)
		[
			MinimapWidget.ToSharedRef()
		];
	MinimapContainer->SetVisibility(EVisibility::Collapsed);
	GEngine->GameViewport->AddViewportWidgetContent(MinimapContainer.ToSharedRef(), MinimapZOrder);
}

void UMinimapSubsystem::EnsureTileCache()
{
	if (bTileCacheStarted)
	{
		return;
	}
	const AWorldStreamer* Streamer = TActorIterator<AWorldStreamer>(GetWorld()) ? *TActorIterator<AWorldStreamer>(GetWorld()) : nullptr;
	if (Streamer && !Streamer->GetWorldDir().IsEmpty())
	{
		bTileCacheStarted = true;
		MapTiles.Initialize(Streamer->GetWorldDir());
	}
}

void UMinimapSubsystem::RedrawMinimapImage()
{
	if (!MinimapRenderer.IsValid() || !MinimapContent.IsValid() || !MinimapImage)
	{
		return;
	}
	MinimapRenderer->DrawWidget(MinimapImage, MinimapContent.ToSharedRef(), FVector2D(MinimapContentPx, MinimapContentPx), 0.f, false);
}

ACarPawn* UMinimapSubsystem::FindCar() const
{
	const APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	return PlayerController ? Cast<ACarPawn>(PlayerController->GetPawn()) : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------------------------------------------------

void UMinimapSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (bDisabled)
	{
		return;
	}
	FrameSecondsSum += DeltaTime;
	++FrameCount;
	EnsureMinimapWidget();
	EnsureTileCache();
	StartIndexBuild();
	PollIndexBuild();
	UpdatePalette();

	const UGameMenuSubsystem* Menu = GetWorld()->GetSubsystem<UGameMenuSubsystem>();
	const bool bMenuOpen = Menu && Menu->IsMenuOpen();
	if (bMenuOpen && bFullMapOpen)
	{
		CloseFullMap(/*bRestoreInput=*/false); // the menu has taken over the input mode and the pause
	}

	ACarPawn* CarPawn = FindCar();
	Car = CarPawn;
	const bool bHasMap = Index.IsValid() && !Index->IsEmpty();
	bCarVisible = CarPawn && bHasMap;
	if (CarPawn)
	{
		ReadCarPose(*CarPawn);
	}
	if (MinimapContainer.IsValid())
	{
		const bool bShow = bCarVisible && !bMenuOpen && !bFullMapOpen;
		MinimapContainer->SetVisibility(bShow ? EVisibility::HitTestInvisible : EVisibility::Collapsed);
	}
	if (!bCarVisible)
	{
		return;
	}
	ApplyTestSwitches();
	PollWheelButtons();
	AnimateZoom(DeltaTime);
	UpdateRoute(DeltaTime);
	MeasureFullMapPaint();

	SecondsSinceFrame += DeltaTime;
	if (SecondsSinceFrame >= MinimapRedrawSeconds)
	{
		SecondsSinceFrame = 0.f;
		UpdateMinimapFrame();
	}
}

void UMinimapSubsystem::ReadCarPose(const ACarPawn& CarPawn)
{
	const FVector Location = CarPawn.GetActorLocation() / 100.0;
	const FVector Forward = CarPawn.GetActorForwardVector();
	CarPositionM = FVector2f(static_cast<float>(Location.X), static_cast<float>(Location.Y));
	CarDirection = FVector2f(static_cast<float>(Forward.X), static_cast<float>(Forward.Y)).GetSafeNormal();
	CarHeadingRadians = FMath::Atan2(CarDirection.Y, CarDirection.X);
}

void UMinimapSubsystem::MeasureFullMapPaint()
{
	static const bool bStats = FParse::Param(FCommandLine::Get(), TEXT("MinimapStats"));
	if (!bStats || !bFullMapOpen || !FullMapWidget.IsValid() || !MinimapRenderer.IsValid())
	{
		return;
	}
	if (!StatsImage)
	{
		StatsImage = NewObject<UTextureRenderTarget2D>(this);
		StatsImage->InitCustomFormat(1600, 900, PF_B8G8R8A8, false);
		StatsImage->UpdateResourceImmediate(true);
	}
	const double StartSeconds = FPlatformTime::Seconds();
	MinimapRenderer->DrawWidget(StatsImage, FullMapWidget.ToSharedRef(), FVector2D(1600, 900), 0.f, false);
	RecordDrawCost(/*bFullMap=*/true, FPlatformTime::Seconds() - StartSeconds);
}

void UMinimapSubsystem::RecordDrawCost(bool bFullMap, double Seconds)
{
	static const bool bStats = FParse::Param(FCommandLine::Get(), TEXT("MinimapStats"));
	constexpr int32 SamplesPerReport = 150;
	if (!bStats)
	{
		return;
	}
	double& Total = bFullMap ? FullMapCostSeconds : MinimapCostSeconds;
	int32& Samples = bFullMap ? FullMapCostSamples : MinimapCostSamples;
	Total += Seconds;
	if (++Samples < SamplesPerReport)
	{
		return;
	}
	UE_LOG(LogMinimap, Log, TEXT("%s draw: %.3f ms average over %d redraws (%d tiles loaded), frame time %.2f ms"), bFullMap ? TEXT("Full map") : TEXT("Minimap"),
		1000.0 * Total / Samples, Samples, MapTiles.GetLoadedTileCount(), 1000.0 * FrameSecondsSum / FMath::Max(FrameCount, 1));
	FrameSecondsSum = 0.0;
	FrameCount = 0;
	Total = 0.0;
	Samples = 0;
}

void UMinimapSubsystem::UpdateMinimapFrame()
{
	const double StartSeconds = FPlatformTime::Seconds();
	const float CullRadius = DisplayRadiusMeters * SquareCornerFactor;
	Index->Query(CarPositionM, CullRadius, CarHeadingRadians, MinimapGeometry::MinTierForRadius(DisplayRadiusMeters), MinimapFrame);
	if (Route.IsValid())
	{
		Route.AppendToFrame(CarPositionM, CullRadius, CarHeadingRadians, MinimapFrame);
	}
	MapTiles.Update(MakeTileRequest(CarPositionM, CullRadius, /*bBuildings=*/true));
	MapTiles.GetTilesInView(CarPositionM, CullRadius, MinimapTiles);
	if (!bFullMapOpen)
	{
		RedrawMinimapImage();
	}
	RecordDrawCost(/*bFullMap=*/false, FPlatformTime::Seconds() - StartSeconds);
}

bool UMinimapSubsystem::GetMinimapWaypoint(FVector2f& OutView) const
{
	if (!bHasWaypoint)
	{
		return false;
	}
	OutView = MinimapGeometry::ToViewFrame(WaypointM - CarPositionM, CarHeadingRadians);
	return true;
}

bool UMinimapSubsystem::GetWaypointMeters(FVector2f& OutMeters) const
{
	if (!bHasWaypoint)
	{
		return false;
	}
	OutMeters = WaypointM;
	return true;
}

bool UMinimapSubsystem::GetRemainingRouteMeters(float& OutMeters) const
{
	if (!Route.IsValid())
	{
		return false;
	}
	OutMeters = RemainingMeters;
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Road data
// ---------------------------------------------------------------------------------------------------------------------

void UMinimapSubsystem::StartIndexBuild()
{
	if (bIndexBuildStarted)
	{
		return;
	}
	const UAITrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UAITrafficSubsystem>();
	if (!Traffic || !Traffic->IsRunning())
	{
		return;
	}
	bIndexBuildStarted = true;
	IndexBuildStartSeconds = FPlatformTime::Seconds();
	const FLaneNetwork* Network = &Traffic->GetLaneNetwork();
	IndexBuild = Async(EAsyncExecution::ThreadPool, [Network]()
	{
		TSharedPtr<FMinimapIndex> Built = MakeShared<FMinimapIndex>();
		Built->Build(*Network);
		return Built;
	});
}

void UMinimapSubsystem::PollIndexBuild()
{
	if (!IndexBuild.IsValid() || !IndexBuild.IsReady())
	{
		return;
	}
	Index = IndexBuild.Get();
	IndexBuild = TFuture<TSharedPtr<FMinimapIndex>>();
	UE_LOG(LogMinimap, Log, TEXT("Minimap road index built in %.0f ms"), (FPlatformTime::Seconds() - IndexBuildStartSeconds) * 1000.0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Input and zoom
// ---------------------------------------------------------------------------------------------------------------------

bool UMinimapSubsystem::HandleKeyDown(const FKey& Key, uint32 Character)
{
	const UGameMenuSubsystem* Menu = GetWorld() ? GetWorld()->GetSubsystem<UGameMenuSubsystem>() : nullptr;
	if (bDisabled || !bCarVisible || (Menu && Menu->IsMenuOpen()))
	{
		return false;
	}
	if (Key == EKeys::M)
	{
		if (bFullMapOpen)
		{
			CloseFullMap(/*bRestoreInput=*/true);
		}
		else
		{
			OpenFullMap(/*bPauseGame=*/true);
		}
		return true;
	}
	if (Key == EKeys::T)
	{
		if (UMap3DSubsystem* Map3D = GetWorld()->GetSubsystem<UMap3DSubsystem>())
		{
			Map3D->ToggleMode(bFullMapOpen);
		}
		return true;
	}
	if (bFullMapOpen)
	{
		return false;
	}
	if (IsPlusKey(Key, Character))
	{
		StepZoom(-1);
		return true;
	}
	if (IsMinusKey(Key, Character))
	{
		StepZoom(+1);
		return true;
	}
	return false;
}

void UMinimapSubsystem::StepZoom(int32 Direction)
{
	// A test radius that is no step snaps to the nearest one first.
	int32 Nearest = 0;
	for (int32 Step = 1; Step < UE_ARRAY_COUNT(ZoomRadiiMeters); ++Step)
	{
		if (FMath::Abs(FMath::Loge(ZoomRadiiMeters[Step] / TargetRadiusMeters)) < FMath::Abs(FMath::Loge(ZoomRadiiMeters[Nearest] / TargetRadiusMeters)))
		{
			Nearest = Step;
		}
	}
	ZoomStep = FMath::Clamp(Nearest + Direction, 0, static_cast<int32>(UE_ARRAY_COUNT(ZoomRadiiMeters)) - 1);
	TargetRadiusMeters = ZoomRadiiMeters[ZoomStep];
}

void UMinimapSubsystem::UpdatePalette()
{
	constexpr float NightBelowSunDegrees = -3.f;
	constexpr float DayAboveSunDegrees = 0.f;
	if (FParse::Param(FCommandLine::Get(), TEXT("MapDark")))
	{
		bDarkMap = true;
		return;
	}
	const UWeatherVisualsSubsystem* Visuals = GetWorld()->GetSubsystem<UWeatherVisualsSubsystem>();
	if (!Visuals)
	{
		return;
	}
	const float SunDegrees = Visuals->GetSunAltitudeDegrees();
	if (SunDegrees < NightBelowSunDegrees)
	{
		bDarkMap = true;
	}
	else if (SunDegrees > DayAboveSunDegrees)
	{
		bDarkMap = false;
	}
}

FMapTileRequest UMinimapSubsystem::MakeTileRequest(const FVector2f& CentreM, float RadiusM, bool bBuildings) const
{
	FMapTileRequest Request;
	Request.CentreM = CentreM;
	Request.RadiusM = RadiusM;
	Request.bBuildings = bBuildings;
	Request.bBuildingRecords = bBuildings && bBuildingRecordsWanted;
	Request.Palette = &GetPalette();
	return Request;
}

void UMinimapSubsystem::AnimateZoom(float DeltaSeconds)
{
	// Interpolating the logarithm makes each step take the same time, however large it is.
	const float LogRadius = FMath::FInterpTo(FMath::Loge(DisplayRadiusMeters), FMath::Loge(TargetRadiusMeters), DeltaSeconds, ZoomAnimationRate);
	DisplayRadiusMeters = FMath::Exp(LogRadius);
	if (FMath::Abs(DisplayRadiusMeters - TargetRadiusMeters) < 0.5f)
	{
		DisplayRadiusMeters = TargetRadiusMeters;
	}
}

void UMinimapSubsystem::PollWheelButtons()
{
	const UWheelInputSubsystem* Wheel = GEngine ? GEngine->GetEngineSubsystem<UWheelInputSubsystem>() : nullptr;
	if (!Wheel)
	{
		return;
	}
	const FWheelInputState State = Wheel->GetState();
	if (!State.bConnected)
	{
		return;
	}
	const UWheelInputSettings* Settings = GetDefault<UWheelInputSettings>();
	const auto WasPressed = [&](int32 ButtonIndex)
	{
		return ButtonIndex >= 0 && ButtonIndex < 64 && State.IsButtonDown(ButtonIndex) && !((PreviousWheelButtons >> ButtonIndex) & 1);
	};
	if (!bFullMapOpen && WasPressed(Settings->MinimapZoomInButtonIndex))
	{
		StepZoom(-1);
	}
	if (!bFullMapOpen && WasPressed(Settings->MinimapZoomOutButtonIndex))
	{
		StepZoom(+1);
	}
	PreviousWheelButtons = State.Buttons;
}

void UMinimapSubsystem::ApplyTestSwitches()
{
	FString Values;
	if (!bTestWaypointDone && FParse::Value(FCommandLine::Get(), TEXT("MapWaypoint="), Values, /*bShouldStopOnSeparator=*/false))
	{
		bTestWaypointDone = true;
		TArray<FString> Parts;
		Values.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 2)
		{
			SetWaypointNear(FVector2f(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1])), 2000.f);
		}
	}
	if (!bTestMapDone && FParse::Param(FCommandLine::Get(), TEXT("OpenMap")))
	{
		bTestMapDone = true;
		OpenFullMap(/*bPauseGame=*/false);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Full map
// ---------------------------------------------------------------------------------------------------------------------

void UMinimapSubsystem::ApplyFullMapTestView()
{
	FString Values;
	if (!FParse::Value(FCommandLine::Get(), TEXT("MapView="), Values, /*bShouldStopOnSeparator=*/false))
	{
		return;
	}
	TArray<FString> Parts;
	Values.ParseIntoArray(Parts, TEXT(","));
	if (Parts.Num() != 3)
	{
		return;
	}
	FullMapCentreM = FVector2f(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]));
	FullMapMetersPerPixel = FMath::Clamp(FCString::Atof(*Parts[2]), FullMapMinMetersPerPixel, FullMapMaxMetersPerPixel);
}

void UMinimapSubsystem::OpenFullMap(bool bPauseGame)
{
	APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	if (bFullMapOpen || !PlayerController || !GEngine || !GEngine->GameViewport)
	{
		return;
	}
	bFullMapOpen = true;
	FullMapCentreM = CarPositionM;
	ApplyFullMapTestView();
	FullMapFrameCentre = FVector2f(MAX_flt, MAX_flt);
	FullMapWidget = SNew(SFullMapWidget, this);
	GEngine->GameViewport->AddViewportWidgetContent(FullMapWidget.ToSharedRef(), FullMapZOrder);
	PlayerController->SetShowMouseCursor(true);
	FInputModeUIOnly InputMode;
	InputMode.SetWidgetToFocus(FullMapWidget);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PlayerController->SetInputMode(InputMode);
	if (bPauseGame && !UGameplayStatics::IsGamePaused(GetWorld()))
	{
		UGameplayStatics::SetGamePaused(GetWorld(), true);
		bPausedByMap = true;
	}
}

void UMinimapSubsystem::CloseFullMap(bool bRestoreInput)
{
	if (!bFullMapOpen)
	{
		return;
	}
	bFullMapOpen = false;
	if (GEngine && GEngine->GameViewport && FullMapWidget.IsValid())
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(FullMapWidget.ToSharedRef());
	}
	FullMapWidget.Reset();
	APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	if (bRestoreInput && PlayerController)
	{
		PlayerController->SetShowMouseCursor(false);
		PlayerController->SetInputMode(FInputModeGameOnly());
	}
	if (bRestoreInput && bPausedByMap)
	{
		UGameplayStatics::SetGamePaused(GetWorld(), false);
	}
	bPausedByMap = false;
}

bool UMinimapSubsystem::AreFullMapBuildingsShown() const
{
	return FullMapMetersPerPixel <= FullMapBuildingsMaxMetersPerPixel;
}

const FMinimapFrame& UMinimapSubsystem::PrepareFullMapFrame(const FVector2f& SizePixels)
{
	const float Radius = 0.5f * SizePixels.Size() * FullMapMetersPerPixel;
	MapTiles.Update(MakeTileRequest(FullMapCentreM, FMath::Min(Radius, FullMapMaxLoadRadiusMeters), AreFullMapBuildingsShown()));
	MapTiles.GetTilesInView(FullMapCentreM, FMath::Min(Radius, FullMapMaxLoadRadiusMeters), FullMapTiles);
	const bool bUnchanged = FullMapFrameCentre == FullMapCentreM && FullMapFrameScale == FullMapMetersPerPixel
		&& FullMapFrameSize == SizePixels && FullMapFrameRouteVersion == RouteVersion;
	if (bUnchanged || !Index.IsValid())
	{
		return FullMapFrame;
	}
	FullMapFrameCentre = FullMapCentreM;
	FullMapFrameScale = FullMapMetersPerPixel;
	FullMapFrameSize = SizePixels;
	FullMapFrameRouteVersion = RouteVersion;
	const float NorthUp = -HALF_PI;
	Index->Query(FullMapCentreM, Radius, NorthUp, MinimapGeometry::MinTierForRadius(FullMapRoadDetailMeters * FullMapMetersPerPixel), FullMapFrame);
	if (Route.IsValid())
	{
		Route.AppendToFrame(FullMapCentreM, Radius, NorthUp, FullMapFrame);
	}
	return FullMapFrame;
}

float UMinimapSubsystem::GetFullMapScaleBarMeters() const
{
	static const float Candidates[] = {10.f, 20.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f};
	float Best = Candidates[0];
	for (const float Candidate : Candidates)
	{
		if (Candidate / FullMapMetersPerPixel <= 200.f)
		{
			Best = Candidate;
		}
	}
	return Best;
}

bool UMinimapSubsystem::GetFullMapWaypoint(FVector2f& OutView) const
{
	if (!bHasWaypoint)
	{
		return false;
	}
	OutView = MinimapGeometry::ToViewFrame(WaypointM - FullMapCentreM, -HALF_PI);
	return true;
}

bool UMinimapSubsystem::GetFullMapCar(FVector2f& OutView, float& OutAngleRadians) const
{
	OutView = MinimapGeometry::ToViewFrame(CarPositionM - FullMapCentreM, -HALF_PI);
	OutAngleRadians = FMath::Atan2(CarDirection.X, -CarDirection.Y);
	return bCarVisible;
}

void UMinimapSubsystem::PanFullMap(const FVector2f& CursorDeltaPixels)
{
	const UMap3DSubsystem* Map3D = GetWorld()->GetSubsystem<UMap3DSubsystem>();
	if (Map3D && Map3D->IsFullMapActive())
	{
		FullMapCentreM += Map3D->FullMapDragToCentreShift(CursorDeltaPixels);
		return;
	}
	FullMapCentreM -= CursorDeltaPixels * FullMapMetersPerPixel;
}

void UMinimapSubsystem::ZoomFullMap(float WheelDelta, const FVector2f& CursorOffsetPixels)
{
	const UMap3DSubsystem* Map3D = GetWorld()->GetSubsystem<UMap3DSubsystem>();
	const bool b3D = Map3D && Map3D->IsFullMapActive();
	const FVector2f WorldUnderCursor = b3D ? Map3D->FullMapScreenToWorld(CursorOffsetPixels) : FullMapCentreM + CursorOffsetPixels * FullMapMetersPerPixel;
	FullMapMetersPerPixel = FMath::Clamp(FullMapMetersPerPixel * FMath::Pow(FullMapZoomPerNotch, WheelDelta), FullMapMinMetersPerPixel, FullMapMaxMetersPerPixel);
	if (b3D)
	{
		// The picture's scale follows FullMapMetersPerPixel, so the ground under the cursor has moved; shift the centre back.
		FullMapCentreM += WorldUnderCursor - Map3D->FullMapScreenToWorld(CursorOffsetPixels);
		return;
	}
	FullMapCentreM = WorldUnderCursor - CursorOffsetPixels * FullMapMetersPerPixel;
}

void UMinimapSubsystem::SetWaypointFromFullMap(const FVector2f& CursorOffsetPixels)
{
	const UMap3DSubsystem* Map3D = GetWorld()->GetSubsystem<UMap3DSubsystem>();
	const bool b3D = Map3D && Map3D->IsFullMapActive();
	const FVector2f World = b3D ? Map3D->FullMapScreenToWorld(CursorOffsetPixels) : FullMapCentreM + CursorOffsetPixels * FullMapMetersPerPixel;
	SetWaypointNear(World, FMath::Max(60.f, 15.f * FullMapMetersPerPixel));
}

// ---------------------------------------------------------------------------------------------------------------------
// Waypoint and route
// ---------------------------------------------------------------------------------------------------------------------

bool UMinimapSubsystem::SetWaypointNear(const FVector2f& WorldM, float MaxSnapMeters)
{
	FMinimapLaneHit Hit;
	if (!Index.IsValid() || !Index->FindNearestLane(WorldM, MaxSnapMeters, nullptr, Hit))
	{
		return false;
	}
	bHasWaypoint = true;
	WaypointM = Hit.Point;
	WaypointLane = Hit.LaneId;
	++WaypointGeneration;
	Route = FMinimapRoute();
	++RouteVersion;
	OffRouteSeconds = 0.f;
	SecondsUntilRetry = 0.f;
	return true;
}

void UMinimapSubsystem::ClearWaypoint()
{
	bHasWaypoint = false;
	WaypointLane = INDEX_NONE;
	++WaypointGeneration;
	Route = FMinimapRoute();
	++RouteVersion;
}

void UMinimapSubsystem::UpdateRoute(float DeltaSeconds)
{
	PollRouteSearch();
	if (!bHasWaypoint)
	{
		return;
	}
	SecondsUntilRetry = FMath::Max(0.f, SecondsUntilRetry - DeltaSeconds);
	if (!Route.IsValid())
	{
		StartRouteSearch();
		return;
	}
	SecondsSinceProgressCheck += DeltaSeconds;
	if (SecondsSinceProgressCheck < ProgressCheckSeconds)
	{
		return;
	}
	CheckProgressOnRoute();
	SecondsSinceProgressCheck = 0.f;
}

void UMinimapSubsystem::CheckProgressOnRoute()
{
	float DistanceFromRoute = 0.f;
	if (!Route.Locate(CarPositionM, DistanceFromRoute, RemainingMeters))
	{
		return;
	}
	if (RemainingMeters < ArrivalMeters || FVector2f::Distance(CarPositionM, WaypointM) < ArrivalMeters)
	{
		UE_LOG(LogMinimap, Log, TEXT("Arrived at the waypoint"));
		ClearWaypoint();
		return;
	}
	OffRouteSeconds = DistanceFromRoute > OffRouteDistanceMeters ? OffRouteSeconds + ProgressCheckSeconds : 0.f;
	if (OffRouteSeconds < OffRouteSecondsLimit)
	{
		return;
	}
	UE_LOG(LogMinimap, Log, TEXT("Left the route by %.0f m, planning again"), DistanceFromRoute);
	OffRouteSeconds = 0.f;
	Route = FMinimapRoute();
	++RouteVersion;
	StartRouteSearch();
}

void UMinimapSubsystem::StartRouteSearch()
{
	const UAITrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UAITrafficSubsystem>();
	if (RouteSearch.IsValid() || SecondsUntilRetry > 0.f || !Traffic || !Index.IsValid())
	{
		return;
	}
	FMinimapLaneHit CarLane;
	if (!Index->FindNearestLane(CarPositionM, CarLaneSearchMeters, &CarDirection, CarLane))
	{
		SecondsUntilRetry = 1.f;
		return;
	}
	RouteSearchGeneration = WaypointGeneration;
	const FLaneNetwork* Network = &Traffic->GetLaneNetwork();
	const int32 StartLane = CarLane.LaneId;
	const int32 GoalLane = WaypointLane;
	RouteSearch = Async(EAsyncExecution::ThreadPool, [Network, StartLane, GoalLane]()
	{
		TSharedPtr<FMinimapRoute> Found = MakeShared<FMinimapRoute>();
		if (!MinimapRouting::FindRoute(*Network, StartLane, GoalLane, *Found))
		{
			Found.Reset();
		}
		return Found;
	});
}

void UMinimapSubsystem::PollRouteSearch()
{
	if (!RouteSearch.IsValid() || !RouteSearch.IsReady())
	{
		return;
	}
	const TSharedPtr<FMinimapRoute> Found = RouteSearch.Get();
	RouteSearch = TFuture<TSharedPtr<FMinimapRoute>>();
	if (RouteSearchGeneration != WaypointGeneration || !bHasWaypoint)
	{
		return; // the waypoint moved or went away while searching
	}
	if (!Found.IsValid())
	{
		UE_LOG(LogMinimap, Warning, TEXT("No route to the waypoint"));
		SecondsUntilRetry = RetrySeconds;
		return;
	}
	Route = *Found;
	++RouteVersion;
	float DistanceFromRoute = 0.f;
	Route.Locate(CarPositionM, DistanceFromRoute, RemainingMeters);
	UE_LOG(LogMinimap, Log, TEXT("Route: %d lanes, %.0f m, searched in %.1f ms"), Route.LaneIds.Num(), Route.TotalMeters, Route.SearchMilliseconds);
}
