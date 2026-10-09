#include "InstrumentCluster.h"

#include "CarLights.h"
#include "CarPawn.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/IConsoleManager.h"
#include "IsobarTime.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "SInstrumentClusterWidget.h"
#include "Slate/WidgetRenderer.h"
#include "WeatherSubsystem.h"
#include "WeatherVisuals.h"

namespace
{
/** Length of the bulb check at ignition, and of the needle sweep inside it, seconds. */
constexpr float BulbCheckSeconds = 2.6f;
constexpr float SweepUpSeconds = 0.9f;
constexpr float SweepHoldSeconds = 0.15f;
constexpr float SweepDownSeconds = 0.9f;

/** Coolant temperature the thermostat holds once warm, and the time constant of the warm-up, game seconds. */
constexpr float OperatingCoolantCelsius = 90.f;
constexpr float CoolantWarmUpSeconds = 240.f;

/** Fuel use of the car, litres per 100 km, and the tank size, litres. */
constexpr float FuelLitresPer100Km = 7.f;
constexpr float TankLitres = 50.f;

float SweepFractionAt(float SecondsSinceIgnition)
{
	if (SecondsSinceIgnition < SweepUpSeconds)
	{
		return FMath::InterpEaseInOut(0.f, 1.f, SecondsSinceIgnition / SweepUpSeconds, 2.f);
	}
	const float AfterUp = SecondsSinceIgnition - SweepUpSeconds;
	if (AfterUp < SweepHoldSeconds)
	{
		return 1.f;
	}
	const float Down = (AfterUp - SweepHoldSeconds) / SweepDownSeconds;
	if (Down < 1.f)
	{
		return FMath::InterpEaseInOut(1.f, 0.f, Down, 2.f);
	}
	return -1.f;
}
}

UInstrumentClusterComponent::UInstrumentClusterComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

ACarPawn* UInstrumentClusterComponent::GetCar() const
{
	return Cast<ACarPawn>(GetOwner());
}

void UInstrumentClusterComponent::BeginPlay()
{
	Super::BeginPlay();
	OdometerKm = GetDefault<UInstrumentClusterSettings>()->StartOdometerKm;
	if (GetCar() && GetCar()->IsPlayerControlled() && FApp::CanEverRender() && FSlateApplication::IsInitialized())
	{
		CreateDisplay();
	}
}

void UInstrumentClusterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Face)
	{
		Face->DestroyComponent();
	}
	Widget.Reset();
	WidgetRenderer.Reset();
	Super::EndPlay(EndPlayReason);
}

void UInstrumentClusterComponent::CreateDisplay()
{
	const UInstrumentClusterSettings* Settings = GetDefault<UInstrumentClusterSettings>();
	UStaticMesh* QuadMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (!QuadMesh)
	{
		return;
	}
	const FVector Normal = Settings->FaceNormal.GetSafeNormal();
	const FVector Up = (FVector::UpVector - Normal * Normal.Z).GetSafeNormal();
	const FVector Right = FVector::CrossProduct(Up, -Normal);
	Face = NewObject<UStaticMeshComponent>(GetOwner(), TEXT("ClusterFace"));
	Face->SetStaticMesh(QuadMesh);
	Face->SetupAttachment(GetCar()->GetMesh());
	Face->SetRelativeLocationAndRotation(Settings->FaceCenter, FRotationMatrix::MakeFromXZ(Right, Normal).Rotator());
	Face->SetRelativeScale3D(FVector(Settings->FaceWidthCm / 100.f, Settings->FaceHeightCm / 100.f, 1.f));
	Face->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Face->SetCastShadow(false);
	Face->bAffectDistanceFieldLighting = false;
	Face->RegisterComponent();

	DrawSize = FVector2D(Settings->PixelWidth, Settings->PixelWidth * Settings->FaceHeightCm / Settings->FaceWidthCm);
	RenderTarget = NewObject<UTextureRenderTarget2D>(this);
	RenderTarget->ClearColor = FLinearColor::Black;
	RenderTarget->InitCustomFormat(FMath::RoundToInt(DrawSize.X), FMath::RoundToInt(DrawSize.Y), PF_B8G8R8A8, false);
	RenderTarget->bAutoGenerateMips = true;
	RenderTarget->UpdateResourceImmediate(true);

	UMaterialInterface* Material = Settings->ScreenMaterial.LoadSynchronous();
	if (Material)
	{
		ScreenMaterial = UMaterialInstanceDynamic::Create(Material, this);
		ScreenMaterial->SetTextureParameterValue(TEXT("ClusterImage"), RenderTarget);
		Face->SetMaterial(0, ScreenMaterial);
	}

	WidgetRenderer = MakeShared<FWidgetRenderer>(false, true);
	Widget = SNew(SInstrumentClusterWidget);
}

void UInstrumentClusterComponent::UpdateFromCar(const FCarDriverInput& Input, const FCarTelemetry& Telemetry)
{
	LastInput = Input;
	LastTelemetry = Telemetry;
}

void UInstrumentClusterComponent::AdvanceSimulation(float DeltaTime)
{
	const bool bPowered = LastInput.bIgnitionOn;
	if (bPowered && !bWasPowered)
	{
		StartupSeconds = 0.f; // ignition on: bulb check and needle sweep
	}
	bWasPowered = bPowered;
	StartupSeconds += DeltaTime;
	float FrozenStartup = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("ClusterFreezeStartup="), FrozenStartup))
	{
		StartupSeconds = FrozenStartup; // screenshot aid: hold the bulb check at this moment
	}

	const float TargetCoolant = LastTelemetry.bEngineRunning ? OperatingCoolantCelsius : 15.f;
	const float CoolingTime = LastTelemetry.bEngineRunning ? CoolantWarmUpSeconds : 20.f * CoolantWarmUpSeconds;
	CoolantCelsius = FMath::FInterpTo(CoolantCelsius, TargetCoolant, DeltaTime, 1.f / CoolingTime);

	const double DistanceKm = FMath::Abs(LastTelemetry.SpeedKmh) / 3600.0 * DeltaTime;
	OdometerKm += DistanceKm;
	TripKm += DistanceKm;
}

void UInstrumentClusterComponent::GatherState()
{
	FClusterState Next;
	Next.bPowered = LastInput.bIgnitionOn;
	Next.SpeedKmh = LastTelemetry.SpeedKmh;
	Next.Rpm = LastTelemetry.EngineRpm;
	Next.Gear = LastTelemetry.EngagedGear;
	Next.SweepFraction = StartupSeconds < BulbCheckSeconds ? SweepFractionAt(StartupSeconds) : -1.f;
	Next.CoolantFraction = FMath::GetMappedRangeValueClamped(FVector2D(30.0, OperatingCoolantCelsius), FVector2D(0.05, 0.5), CoolantCelsius);
	Next.FuelFraction = FMath::Max(0.f, 0.62f - static_cast<float>(TripKm) * FuelLitresPer100Km / 100.f / TankLitres);
	Next.OdometerKm = static_cast<float>(OdometerKm);
	Next.TripKm = static_cast<float>(TripKm);

	const bool bBulbCheck = StartupSeconds < BulbCheckSeconds;
	Next.bCheckEngine = bBulbCheck;
	Next.bAbs = bBulbCheck;
	Next.bAirbag = bBulbCheck;
	Next.bOilPressure = bBulbCheck || !LastTelemetry.bEngineRunning;
	Next.bBattery = bBulbCheck || !LastTelemetry.bEngineRunning;
	Next.bHandbrake = bBulbCheck || LastInput.bHandbrake;

	if (LastTelemetry.bGrinding)
	{
		Next.Message = TEXT("Gear grind: use the clutch");
	}
	else if (LastInput.bIgnitionOn && !bBulbCheck && !LastTelemetry.bEngineRunning && !LastTelemetry.bCranking)
	{
		Next.Message = TEXT("Engine off: press E");
	}
	if (const UCarLightsComponent* Lights = GetCar()->GetCarLights())
	{
		Next.bLowBeam = Lights->IsLowBeamOn();
		Next.bHighBeam = Lights->IsHighBeamOn();
		Next.bLeftIndicator = Lights->IsLeftLampOn();
		Next.bRightIndicator = Lights->IsRightLampOn();
	}

	if (const UWeatherVisualsSubsystem* Visuals = GetWorld()->GetSubsystem<UWeatherVisualsSubsystem>())
	{
		Next.OutsideTemperatureCelsius = Visuals->GetState().TemperatureCelsius;
	}
	if (const UWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UWeatherSubsystem>())
	{
		Next.TimeOfDayHours = static_cast<float>(IsobarCalendarAt(Weather->GetWeatherSeconds()).GetHourOfDay());
	}

	// At night the backlight dims; the exposure value says how dark the scene is.
	static const IConsoleVariable* ExposureVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("dg.MenuPanelExposureEv"));
	const float ExposureEv = ExposureVariable ? ExposureVariable->GetFloat() : 13.f;
	Next.Backlight = FMath::GetMappedRangeValueClamped(FVector2D(13.0, 5.0), FVector2D(1.0, 0.45), ExposureEv);
	ApplyTestSwitches(Next);
	State = Next;
}

void UInstrumentClusterComponent::ApplyTestSwitches(FClusterState& InOutState) const
{
	// Screenshot aids: -ClusterTest=<km/h>,<rpm>,<gear> shows those values, -ClusterCold shows ignition on with the engine off.
	FString Values;
	if (FParse::Value(FCommandLine::Get(), TEXT("ClusterTest="), Values, /*bShouldStopOnSeparator=*/false))
	{
		TArray<FString> Parts;
		Values.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 3)
		{
			InOutState.SpeedKmh = FCString::Atof(*Parts[0]);
			InOutState.Rpm = FCString::Atof(*Parts[1]);
			InOutState.Gear = FCString::Atoi(*Parts[2]);
			InOutState.CoolantFraction = 0.5f;
		}
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("ClusterCold")))
	{
		InOutState.Rpm = 0.f;
		InOutState.SpeedKmh = 0.f;
		InOutState.Gear = 0;
		InOutState.bOilPressure = true;
		InOutState.bBattery = true;
		InOutState.Message = TEXT("Engine off: press E");
	}
}

void UInstrumentClusterComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AdvanceSimulation(DeltaTime);
	if (!WidgetRenderer.IsValid() || !Widget.IsValid() || !RenderTarget)
	{
		return;
	}

	// The material undoes the scene's exposure so the display shows at its own brightness (see AGameMenuPanel).
	static const IConsoleVariable* ExposureVariable = IConsoleManager::Get().FindConsoleVariable(TEXT("dg.MenuPanelExposureEv"));
	if (ScreenMaterial && ExposureVariable)
	{
		ScreenMaterial->SetScalarParameterValue(TEXT("Gain"), 1.2f * FMath::Pow(2.f, ExposureVariable->GetFloat()));
	}

	SecondsSinceRedraw += DeltaTime;
	if (SecondsSinceRedraw < 1.f / FMath::Max(1.f, GetDefault<UInstrumentClusterSettings>()->RedrawHz))
	{
		return;
	}
	SecondsSinceRedraw = 0.f;
	GatherState();
	Widget->SetState(State);
	WidgetRenderer->DrawWidget(RenderTarget, Widget.ToSharedRef(), DrawSize, 0.f, false);

	if (FParse::Param(FCommandLine::Get(), TEXT("DumpCluster")) && GetWorld()->GetTimeSeconds() > 9.0 && !bDumped)
	{
		bDumped = true;
		UKismetRenderingLibrary::ExportRenderTarget(this, RenderTarget, FPaths::ProjectSavedDir() / TEXT("Screenshots"), TEXT("cluster.png"));
	}
}
