#include "CarLights.h"

#include "CarAudioComponent.h"
#include "CarPawn.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "WeatherVisuals.h"
#include "Engine/TextureLightProfile.h"

namespace
{
TAutoConsoleVariable<int32> CVarHeadlightShadows(TEXT("tg.Headlights.Shadows"), 0,
	TEXT("Virtual shadow maps for the headlights: 1 = on, 0 = off (a moving light invalidates cached shadow pages every frame)."), ECVF_Default);

TAutoConsoleVariable<int32> CVarHeadlightMegaLights(TEXT("tg.Headlights.MegaLights"), 1,
	TEXT("Whether the headlight spot lights may use MegaLights (read when the car spawns): 1 = yes, 0 = classic deferred lights."), ECVF_Default);

TAutoConsoleVariable<float> CVarHeadlightTestWall(TEXT("tg.Headlights.Wall"), 0.f,
	TEXT("Test wall in metres ahead of the headlamps to photograph the beam pattern on; 0 = none."), ECVF_Default);

TAutoConsoleVariable<int32> CVarHeadlightMode(TEXT("tg.Headlights.Mode"), -1,
	TEXT("Test override of the light switch: -1 = driver's switch, 0 = off, 1 = low beam, 2 = high beam."), ECVF_Default);
TAutoConsoleVariable<int32> CVarBrakeLightTest(TEXT("tg.Headlights.Brake"), -1,
	TEXT("Test override of the brake lights: -1 = pedal, 0 = off, 1 = on."), ECVF_Default);
TAutoConsoleVariable<float> CVarBeamScale(TEXT("tg.Headlights.BeamScale"), 1.f, TEXT("Multiplier on the beam intensities, for tuning."), ECVF_Default);
TAutoConsoleVariable<float> CVarRearScale(TEXT("tg.Headlights.RearScale"), 1.f, TEXT("Multiplier on the tail, brake and reverse lamp intensities, for tuning."), ECVF_Default);

/** Name of the body material slot whose texture mask lights up the lamps (City Sample "LE" parameters). */
const FName LightMaterialSlotName(TEXT("veh_light"));

/** LED headlamps, about 5500 K: slightly cool white (halogen would be about 3200 K, 1.0, 0.8, 0.55). */
const FLinearColor WarmWhite(0.92f, 0.96f, 1.f);
const FLinearColor TailRed(1.f, 0.03f, 0.01f);

/** Stops of exposure the eye closes for the low beam and for the high beam on top of it, and the seconds it takes to adapt. */
constexpr float LowBeamAdaptationStops = 1.5f;
constexpr float HighBeamAdaptationStops = 1.0f;
constexpr float AdaptationSeconds = 1.5f;

/** Fraction per second at which a bulb's glow follows the switch (filaments warm and cool in about 0.1 s). */
constexpr float GlowRiseRate = 14.f;
constexpr float GlowFallRate = 9.f;

float ChaseGlow(float Glow, float Target, float DeltaTime)
{
	const float Rate = Target > Glow ? GlowRiseRate : GlowFallRate;
	return FMath::FInterpConstantTo(Glow, Target, DeltaTime, Rate);
}
}

UCarLightsComponent::UCarLightsComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

ACarPawn* UCarLightsComponent::GetCar() const
{
	return Cast<ACarPawn>(GetOwner());
}

void UCarLightsComponent::BeginPlay()
{
	Super::BeginPlay();
	if (!GetCar())
	{
		return;
	}

	// The body mesh is the attached static mesh that has the light material slot.
	TArray<UStaticMeshComponent*> MeshComponents;
	GetOwner()->GetComponents<UStaticMeshComponent>(MeshComponents);
	for (UStaticMeshComponent* Mesh : MeshComponents)
	{
		const int32 SlotIndex = Mesh->GetMaterialIndex(LightMaterialSlotName);
		if (SlotIndex == INDEX_NONE)
		{
			continue;
		}
		LightMaterial = Mesh->CreateDynamicMaterialInstance(SlotIndex);
		break;
	}
	CreateLights();

	// Test hooks: start with the lights on, e.g. -LowBeam, -HighBeam, -Indicator=left (or right, hazard).
	const TCHAR* CommandLine = FCommandLine::Get();
	bHighBeam = FParse::Param(CommandLine, TEXT("HighBeam"));
	bLowBeam = bHighBeam || FParse::Param(CommandLine, TEXT("LowBeam"));
	bForceBrakeLights = FParse::Param(CommandLine, TEXT("BrakeLights"));
	bForceReverseLight = FParse::Param(CommandLine, TEXT("ReverseLight"));
	FString IndicatorName;
	if (FParse::Value(CommandLine, TEXT("Indicator="), IndicatorName))
	{
		Indicator = IndicatorName == TEXT("left") ? ECarIndicator::Left : IndicatorName == TEXT("right") ? ECarIndicator::Right : ECarIndicator::Hazard;
	}
}

USpotLightComponent* UCarLightsComponent::AddLamp(const TCHAR* Name, const FVector& Location, float YawDegrees, const FLinearColor& Colour, float RadiusCm)
{
	// A wide spot light that shines away from the car, so the lamp does not light the cabin through the body.
	USpotLightComponent* Light = NewObject<USpotLightComponent>(GetOwner(), Name);
	Light->SetMobility(EComponentMobility::Movable); // a light is static by default and then ignores every runtime change
	Light->SetupAttachment(GetCar()->GetMesh());
	Light->SetRelativeLocationAndRotation(Location, FRotator(0.f, YawDegrees, 0.f));
	Light->SetInnerConeAngle(30.f);
	Light->SetOuterConeAngle(75.f);
	Light->SetLightColor(Colour);
	Light->IntensityUnits = ELightUnits::Candelas;
	Light->SetIntensity(0.f);
	Light->SetAttenuationRadius(RadiusCm);
	Light->SetSourceRadius(2.f);
	Light->SetCastShadows(false);
	Light->SetVisibility(false);
	Light->RegisterComponent();
	return Light;
}

USpotLightComponent* UCarLightsComponent::AddHeadlamp(const TCHAR* Name, const FVector& Location, float AimDownDegrees, float RangeCm, float ConeDegrees, UTextureLightProfile* Profile)
{
	USpotLightComponent* Light = NewObject<USpotLightComponent>(GetOwner(), Name);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetupAttachment(GetCar()->GetMesh());
	Light->SetRelativeLocationAndRotation(Location, FRotator(-AimDownDegrees, 0.f, GetDefault<UCarLightSettings>()->ProfileRollDegrees));
	Light->SetLightColor(WarmWhite);
	Light->IntensityUnits = ELightUnits::Candelas;
	Light->SetIntensity(0.f);
	Light->SetAttenuationRadius(RangeCm);
	Light->SetSourceRadius(4.f);
	// The profile shapes the beam; the cone only has to contain it. Inner angle 0 keeps the cone from adding its own falloff.
	Light->SetInnerConeAngle(0.f);
	Light->SetOuterConeAngle(ConeDegrees);
	Light->SetCastShadows(CVarHeadlightShadows.GetValueOnGameThread() != 0);
	Light->SetVisibility(false);
	if (Profile)
	{
		Light->SetIESTexture(Profile);
		Light->SetUseIESBrightness(false); // the profile is normalised; the intensity below is the peak candela
	}
	Light->bAllowMegaLights = CVarHeadlightMegaLights.GetValueOnGameThread() != 0;
	Light->RegisterComponent();
	return Light;
}

void UCarLightsComponent::CreateLights()
{
	const UCarLightSettings* Settings = GetDefault<UCarLightSettings>();
	UTextureLightProfile* LowProfile = Settings->LowBeamProfile.LoadSynchronous();
	UTextureLightProfile* HighProfile = Settings->HighBeamProfile.LoadSynchronous();

	for (const float Side : {-1.f, 1.f})
	{
		const TCHAR* SideName = Side < 0.f ? TEXT("Left") : TEXT("Right");
		const FVector Headlamp(Settings->HeadlampLocation.X, Side * Settings->HeadlampLocation.Y, Settings->HeadlampLocation.Z);
		LowBeamLights.Add(AddHeadlamp(*FString::Printf(TEXT("LowBeam%s"), SideName), Headlamp, Settings->LowBeamAimDownDegrees,
			Settings->LowBeamRangeCm, Settings->LowBeamConeDegrees, LowProfile));
		HighBeamLights.Add(AddHeadlamp(*FString::Printf(TEXT("HighBeam%s"), SideName), Headlamp, 0.f,
			Settings->HighBeamRangeCm, Settings->HighBeamConeDegrees, HighProfile));

		const FVector Tail(Settings->TailLampLocation.X, Side * Settings->TailLampLocation.Y, Settings->TailLampLocation.Z);
		TailLights.Add(AddLamp(*FString::Printf(TEXT("Tail%s"), SideName), Tail, 180.f, TailRed, 250.f));
	}
}

void UCarLightsComponent::ToggleLowBeam()
{
	if (bHighBeam)
	{
		bHighBeam = false; // from high beam the switch goes back to low beam, not off
		return;
	}
	bLowBeam = !bLowBeam;
}

void UCarLightsComponent::ToggleHighBeam()
{
	bHighBeam = !bHighBeam;
	if (bHighBeam)
	{
		bLowBeam = true;
	}
}

void UCarLightsComponent::ToggleIndicator(ECarIndicator Direction)
{
	const bool bSame = Indicator == Direction;
	Indicator = bSame ? ECarIndicator::Off : Direction;
	SteeringPeakDegrees = 0.f;
	BlinkClockSeconds = 0.f; // a new stalk movement starts with the lamps on
}

void UCarLightsComponent::UpdateFromCar(const FCarDriverInput& Input, const FCarTelemetry& Telemetry)
{
	LastInput = Input;
	LastTelemetry = Telemetry;
}

void UCarLightsComponent::SetLampPhase(bool bOn)
{
	if (bOn == bLastLampPhase)
	{
		return;
	}
	bLastLampPhase = bOn;
	OnIndicatorLamp.Broadcast(bOn);
	if (ACarPawn* Car = GetCar())
	{
		if (UCarAudioComponent* Audio = Car->GetCarAudio())
		{
			Audio->SetIndicatorActive(bOn);
		}
	}
}

void UCarLightsComponent::UpdateIndicator(float DeltaTime, float SteeringWheelDeg)
{
	if (Indicator == ECarIndicator::Off)
	{
		bLeftLampOn = false;
		bRightLampOn = false;
		SetLampPhase(false);
		return;
	}

	// Self-cancelling: once the wheel has been turned into the corner and comes back, the turn is over.
	const float Direction = Indicator == ECarIndicator::Left ? -1.f : Indicator == ECarIndicator::Right ? 1.f : 0.f;
	const UCarLightSettings* Settings = GetDefault<UCarLightSettings>();
	if (Direction != 0.f)
	{
		const float TurnedDegrees = SteeringWheelDeg * Direction;
		SteeringPeakDegrees = FMath::Max(SteeringPeakDegrees, TurnedDegrees);
		if (SteeringPeakDegrees > Settings->CancelTurnDegrees && TurnedDegrees < Settings->CancelReturnDegrees)
		{
			Indicator = ECarIndicator::Off;
			return;
		}
	}

	BlinkClockSeconds += DeltaTime;
	const float Period = 1.f / FMath::Max(0.1f, Settings->IndicatorHz);
	const bool bPhaseOn = FMath::Fmod(BlinkClockSeconds, Period) < 0.5f * Period;
	bLeftLampOn = bPhaseOn && (Indicator == ECarIndicator::Left || Indicator == ECarIndicator::Hazard);
	bRightLampOn = bPhaseOn && (Indicator == ECarIndicator::Right || Indicator == ECarIndicator::Hazard);
	SetLampPhase(bPhaseOn);
}

void UCarLightsComponent::ApplyState(float DeltaTime, const FCarDriverInput& Input, const FCarTelemetry& Telemetry)
{
	const UCarLightSettings* Settings = GetDefault<UCarLightSettings>();
	const bool bPowered = Telemetry.bEngineRunning || Input.bIgnitionOn;
	const int32 SwitchOverride = CVarHeadlightMode.GetValueOnGameThread();
	const int32 BrakeOverride = CVarBrakeLightTest.GetValueOnGameThread();
	const bool bBrakeLit = BrakeOverride >= 0 ? BrakeOverride != 0 : (bForceBrakeLights || (bPowered && Input.Brake > 0.03f));
	const bool bLowOn = SwitchOverride >= 0 ? SwitchOverride >= 1 : (bLowBeam || bHighBeam);
	const bool bHighOn = SwitchOverride >= 0 ? SwitchOverride == 2 : bHighBeam;
	const float BeamScale = CVarBeamScale.GetValueOnGameThread();
	const float RearScale = CVarRearScale.GetValueOnGameThread();
	const bool bReverseLit = bForceReverseLight || (bPowered && Telemetry.EngagedGear == -1);

	BeamGlow = ChaseGlow(BeamGlow, bLowOn ? 1.f : 0.f, DeltaTime);
	HighBeamGlow = ChaseGlow(HighBeamGlow, bHighOn ? 1.f : 0.f, DeltaTime);
	BrakeGlow = ChaseGlow(BrakeGlow, bBrakeLit ? 1.f : 0.f, DeltaTime);
	ReverseGlow = ChaseGlow(ReverseGlow, bReverseLit ? 1.f : 0.f, DeltaTime);
	LeftGlow = ChaseGlow(LeftGlow, bLeftLampOn ? 1.f : 0.f, DeltaTime);
	RightGlow = ChaseGlow(RightGlow, bRightLampOn ? 1.f : 0.f, DeltaTime);

	if (LightMaterial)
	{
		LightMaterial->SetScalarParameterValue(TEXT("Headlight Amt LE"), BeamGlow);
		LightMaterial->SetScalarParameterValue(TEXT("Run Amt LE"), BeamGlow);
		LightMaterial->SetScalarParameterValue(TEXT("Brake Amt LE"), BrakeGlow);
		LightMaterial->SetScalarParameterValue(TEXT("Reverse Amt LE"), ReverseGlow);
		LightMaterial->SetScalarParameterValue(TEXT("Turn Amt Left LE"), LeftGlow);
		LightMaterial->SetScalarParameterValue(TEXT("Turn Amt Right LE"), RightGlow);
	}

	for (USpotLightComponent* Light : LowBeamLights)
	{
		const float Glow = BeamGlow * (1.f - 0.5f * HighBeamGlow);
		Light->SetVisibility(BeamGlow > 0.01f);
		Light->SetIntensity(Settings->LowBeamCandela * BeamScale * Glow);
	}
	for (USpotLightComponent* Light : HighBeamLights)
	{
		Light->SetVisibility(HighBeamGlow > 0.01f);
		Light->SetIntensity(Settings->HighBeamCandela * BeamScale * HighBeamGlow);
	}
	for (USpotLightComponent* Light : TailLights)
	{
		const float Candela = RearScale * (Settings->TailCandela * BeamGlow + Settings->BrakeCandela * BrakeGlow);
		Light->SetVisibility(Candela > 0.05f);
		Light->SetIntensity(Candela);
	}
}

void UCarLightsComponent::UpdateTestWall()
{
	const float DistanceMeters = CVarHeadlightTestWall.GetValueOnGameThread();
	if (DistanceMeters <= 0.f)
	{
		if (TestWall)
		{
			TestWall->SetVisibility(false);
		}
		return;
	}
	if (!TestWall)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		TestWall = NewObject<UStaticMeshComponent>(GetOwner(), TEXT("HeadlightTestWall"));
		TestWall->SetStaticMesh(Cube);
		TestWall->SetupAttachment(GetCar()->GetMesh());
		TestWall->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		TestWall->SetCastShadow(false);
		TestWall->RegisterComponent();
	}
	// A 14 m wide, 5 m high light grey wall whose face is the given distance from the headlamps.
	const UCarLightSettings* Settings = GetDefault<UCarLightSettings>();
	TestWall->SetRelativeLocation(FVector(Settings->HeadlampLocation.X + DistanceMeters * 100.f + 5.f, 0.f, 200.f));
	TestWall->SetRelativeScale3D(FVector(0.1f, 14.f, 5.f));
	TestWall->SetVisibility(true);
}

void UCarLightsComponent::UpdateEyeAdaptation(float DeltaTime)
{
	const float TargetStops = LowBeamAdaptationStops * BeamGlow + HighBeamAdaptationStops * HighBeamGlow;
	AdaptationStops = FMath::FInterpTo(AdaptationStops, TargetStops, DeltaTime, 1.f / AdaptationSeconds);
	if (UWeatherVisualsSubsystem* Visuals = GetWorld() ? GetWorld()->GetSubsystem<UWeatherVisualsSubsystem>() : nullptr)
	{
		Visuals->SetHeadlightAdaptationStops(AdaptationStops);
	}
}

void UCarLightsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UpdateIndicator(DeltaTime, LastInput.SteeringWheelDeg);
	ApplyState(DeltaTime, LastInput, LastTelemetry);
	UpdateEyeAdaptation(DeltaTime);
	UpdateTestWall();
}
