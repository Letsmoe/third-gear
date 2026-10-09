#include "CarLights.h"

#include "CarAudioComponent.h"
#include "CarPawn.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
TAutoConsoleVariable<int32> CVarHeadlightShadows(TEXT("tg.Headlights.Shadows"), 0,
	TEXT("Virtual shadow maps for the headlights: 1 = on, 0 = off (a moving light invalidates cached shadow pages every frame)."), ECVF_Default);

/** Name of the body material slot whose texture mask lights up the lamps (City Sample "LE" parameters). */
const FName LightMaterialSlotName(TEXT("veh_light"));

const FLinearColor WarmWhite(1.f, 0.93f, 0.8f);
const FLinearColor TailRed(1.f, 0.03f, 0.01f);
const FLinearColor ReverseWhite(1.f, 0.97f, 0.9f);
const FLinearColor IndicatorAmber(1.f, 0.38f, 0.02f);

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

USpotLightComponent* UCarLightsComponent::AddHeadlamp(const TCHAR* Name, const FVector& Location, float AimDownDegrees, float RangeCm,
	UMaterialInterface* Function, UMaterialInstanceDynamic*& OutFunctionInstance)
{
	USpotLightComponent* Light = NewObject<USpotLightComponent>(GetOwner(), Name);
	Light->SetupAttachment(GetCar()->GetMesh());
	Light->SetRelativeLocationAndRotation(Location, FRotator(-AimDownDegrees, 0.f, 0.f));
	Light->SetLightColor(WarmWhite);
	Light->IntensityUnits = ELightUnits::Candelas;
	Light->SetIntensity(0.f);
	Light->SetAttenuationRadius(RangeCm);
	Light->SetSourceRadius(4.f);
	Light->SetInnerConeAngle(40.f);
	Light->SetOuterConeAngle(45.f);
	Light->SetCastShadows(CVarHeadlightShadows.GetValueOnGameThread() != 0);
	Light->SetVisibility(false);
	if (Function)
	{
		OutFunctionInstance = UMaterialInstanceDynamic::Create(Function, this);
		Light->SetLightFunctionMaterial(OutFunctionInstance);
		Light->SetLightFunctionScale(FVector(1.f));
	}
	Light->RegisterComponent();
	return Light;
}

void UCarLightsComponent::CreateLights()
{
	const UCarLightSettings* Settings = GetDefault<UCarLightSettings>();
	UMaterialInterface* LowFunction = Settings->LowBeamFunction.LoadSynchronous();
	UMaterialInterface* HighFunction = Settings->HighBeamFunction.LoadSynchronous();

	for (const float Side : {-1.f, 1.f})
	{
		const TCHAR* SideName = Side < 0.f ? TEXT("Left") : TEXT("Right");
		const FVector Headlamp(Settings->HeadlampLocation.X, Side * Settings->HeadlampLocation.Y, Settings->HeadlampLocation.Z);
		UMaterialInstanceDynamic* Function = nullptr;
		LowBeamLights.Add(AddHeadlamp(*FString::Printf(TEXT("LowBeam%s"), SideName), Headlamp, Settings->LowBeamAimDownDegrees,
			Settings->LowBeamRangeCm, LowFunction, Function));
		BeamFunctionInstances.Add(Function);
		HighBeamLights.Add(AddHeadlamp(*FString::Printf(TEXT("HighBeam%s"), SideName), Headlamp, 0.f,
			Settings->HighBeamRangeCm, HighFunction, Function));
		BeamFunctionInstances.Add(Function);

		const FVector Tail(Settings->TailLampLocation.X, Side * Settings->TailLampLocation.Y, Settings->TailLampLocation.Z);
		TailLights.Add(AddLamp(*FString::Printf(TEXT("Tail%s"), SideName), Tail, 180.f, TailRed, 900.f));
		ReverseLights.Add(AddLamp(*FString::Printf(TEXT("Reverse%s"), SideName), Tail + FVector(-4.f, -Side * 22.f, -4.f), 180.f, ReverseWhite, 1200.f));

		const FVector Front(Settings->FrontIndicatorLocation.X, Side * Settings->FrontIndicatorLocation.Y, Settings->FrontIndicatorLocation.Z);
		IndicatorLights.Add({AddLamp(*FString::Printf(TEXT("IndicatorFront%s"), SideName), Front, Side * 35.f, IndicatorAmber, 700.f), Side < 0.f});
		const FVector Rear = Tail + FVector(0.f, Side * 2.f, 3.f);
		IndicatorLights.Add({AddLamp(*FString::Printf(TEXT("IndicatorRear%s"), SideName), Rear, 180.f - Side * 35.f, IndicatorAmber, 700.f), Side < 0.f});
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

void UCarLightsComponent::UpdateBeamFunctions()
{
	// The light function draws the beam on whatever it hits from the light's pose: position and the three axes.
	for (int32 Index = 0; Index < BeamFunctionInstances.Num(); ++Index)
	{
		UMaterialInstanceDynamic* Function = BeamFunctionInstances[Index];
		const USpotLightComponent* Light = (Index % 2 == 0)
			? LowBeamLights[Index / 2].Get() : HighBeamLights[Index / 2].Get();
		if (!Function || !Light || !Light->IsVisible())
		{
			continue;
		}
		const FTransform& Pose = Light->GetComponentTransform();
		Function->SetVectorParameterValue(TEXT("LightPosition"), FLinearColor(Pose.GetLocation().X, Pose.GetLocation().Y, Pose.GetLocation().Z, 0.f));
		const FVector Forward = Pose.GetUnitAxis(EAxis::X);
		const FVector Right = Pose.GetUnitAxis(EAxis::Y);
		const FVector Up = Pose.GetUnitAxis(EAxis::Z);
		Function->SetVectorParameterValue(TEXT("LightForward"), FLinearColor(Forward.X, Forward.Y, Forward.Z, 0.f));
		Function->SetVectorParameterValue(TEXT("LightRight"), FLinearColor(Right.X, Right.Y, Right.Z, 0.f));
		Function->SetVectorParameterValue(TEXT("LightUp"), FLinearColor(Up.X, Up.Y, Up.Z, 0.f));
	}
}

void UCarLightsComponent::ApplyState(float DeltaTime, const FCarDriverInput& Input, const FCarTelemetry& Telemetry)
{
	const UCarLightSettings* Settings = GetDefault<UCarLightSettings>();
	const bool bPowered = Telemetry.bEngineRunning || Input.bIgnitionOn;
	const bool bBrakeLit = bForceBrakeLights || (bPowered && Input.Brake > 0.03f);
	const bool bReverseLit = bForceReverseLight || (bPowered && Telemetry.EngagedGear == -1);

	BeamGlow = ChaseGlow(BeamGlow, (bLowBeam || bHighBeam) ? 1.f : 0.f, DeltaTime);
	HighBeamGlow = ChaseGlow(HighBeamGlow, bHighBeam ? 1.f : 0.f, DeltaTime);
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
		Light->SetIntensity(Settings->LowBeamCandela * Glow);
	}
	for (USpotLightComponent* Light : HighBeamLights)
	{
		Light->SetVisibility(HighBeamGlow > 0.01f);
		Light->SetIntensity(Settings->HighBeamCandela * HighBeamGlow);
	}
	for (USpotLightComponent* Light : TailLights)
	{
		const float Candela = Settings->TailCandela * BeamGlow + Settings->BrakeCandela * BrakeGlow;
		Light->SetVisibility(Candela > 0.05f);
		Light->SetIntensity(Candela);
	}
	for (USpotLightComponent* Light : ReverseLights)
	{
		Light->SetVisibility(ReverseGlow > 0.01f);
		Light->SetIntensity(Settings->ReverseCandela * ReverseGlow);
	}
	for (const FLampLight& Lamp : IndicatorLights)
	{
		const float Glow = Lamp.bLeft ? LeftGlow : RightGlow;
		Lamp.Light->SetVisibility(Glow > 0.01f);
		Lamp.Light->SetIntensity(Settings->IndicatorCandela * Glow);
	}
	UpdateBeamFunctions();
}

void UCarLightsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UpdateIndicator(DeltaTime, LastInput.SteeringWheelDeg);
	ApplyState(DeltaTime, LastInput, LastTelemetry);
}
