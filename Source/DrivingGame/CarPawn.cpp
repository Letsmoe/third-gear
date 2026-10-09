#include "CarPawn.h"

#include "Animation/AnimInstance.h"
#include "Camera/CameraComponent.h"
#include "CarAudioComponent.h"
#include "CarMovementComponent.h"
#include "CarSettings.h"
#include "Components/InputComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/PlayerController.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "InputCoreTypes.h"
#include "WheelInputSettings.h"
#include "WheelInputSubsystem.h"

namespace
{
// Keyboard pedal ramps, per second.
constexpr float KeyThrottleRise = 2.5f, KeyThrottleFall = 5.f;
constexpr float KeyBrakeRise = 2.5f, KeyBrakeFall = 6.f;
constexpr float KeyClutchPress = 6.f, KeyClutchRelease = 0.9f; // slow release lets the clutch slip through the bite point
/** Push-button start: one press cranks until the engine runs or this time has passed. */
constexpr float StarterMaxSeconds = 1.5f;

UWheelInputSubsystem* GetWheel()
{
	return GEngine ? GEngine->GetEngineSubsystem<UWheelInputSubsystem>() : nullptr;
}

bool ButtonPressed(int64 Buttons, int64 Previous, int32 Index)
{
	return Index >= 0 && Index < 64 && ((Buttons >> Index) & 1) && !((Previous >> Index) & 1);
}
}

ACarPawn::ACarPawn(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UCarMovementComponent>(AWheeledVehiclePawn::VehicleMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	const UCarSettings* Settings = GetDefault<UCarSettings>();
	USkeletalMeshComponent* CarMesh = GetMesh();
	if (USkeletalMesh* SkeletalMesh = Settings->SkeletalMesh.LoadSynchronous())
	{
		CarMesh->SetSkeletalMeshAsset(SkeletalMesh);
	}
	if (UClass* AnimClass = Settings->AnimClass.LoadSynchronous())
	{
		CarMesh->SetAnimInstanceClass(AnimClass);
	}
	CarMesh->SetSimulatePhysics(true);
	CarMesh->SetCollisionProfileName(FName("Vehicle"));
	CarMesh->BodyInstance.bUseCCD = true;

	for (int32 i = 0; i < Settings->AttachedMeshes.Num(); ++i)
	{
		const FCarAttachedMesh& Entry = Settings->AttachedMeshes[i];
		UStaticMeshComponent* Component = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("AttachedMesh%d"), i));
		Component->SetupAttachment(CarMesh, Entry.Socket);
		Component->SetRelativeLocationAndRotation(Entry.Location, Entry.Rotation);
		Component->SetStaticMesh(Entry.Mesh.LoadSynchronous());
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision); // collision comes from the physics asset
		AttachedMeshes.Add(Component);
	}

	EyeOrigin = CreateDefaultSubobject<USceneComponent>(TEXT("EyeOrigin"));
	EyeOrigin->SetupAttachment(CarMesh);
	EyeOrigin->SetRelativeLocation(Settings->DriverEyeLocation);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(EyeOrigin);
	Camera->bLockToHmd = true;
	Camera->bUsePawnControlRotation = false;
	Camera->SetFieldOfView(90.f);

	Dashboard = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Dashboard"));
	Dashboard->SetupAttachment(CarMesh);
	Dashboard->SetRelativeLocation(Settings->DashboardLocation);
	Dashboard->SetRelativeRotation(FRotator(15.f, 180.f, 0.f)); // text faces the driver, tilted like an instrument cluster
	Dashboard->SetHorizontalAlignment(EHTA_Center);
	Dashboard->SetVerticalAlignment(EVRTA_TextCenter);
	Dashboard->SetWorldSize(2.4f);
	Dashboard->SetTextRenderColor(FColor(255, 220, 140));
	Dashboard->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Dashboard->SetCastShadow(false);
	if (UMaterialInterface* TextMaterial = Settings->DashboardTextMaterial.LoadSynchronous())
	{
		Dashboard->SetTextMaterial(TextMaterial);
	}

	CarAudio = CreateDefaultSubobject<UCarAudioComponent>(TEXT("CarAudio"));

	AutoPossessPlayer = EAutoReceiveInput::Disabled;
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;
}

UCarMovementComponent* ACarPawn::GetCarMovement() const
{
	return Cast<UCarMovementComponent>(GetVehicleMovement());
}

FCarTelemetry ACarPawn::GetTelemetry() const
{
	const UCarMovementComponent* Movement = GetCarMovement();
	return Movement ? Movement->GetTelemetry() : FCarTelemetry();
}

void ACarPawn::BeginPlay()
{
	Super::BeginPlay();

	if (IsHMDActive())
	{
		// Seated experience: origin at the head's starting pose, not the floor.
		UHeadMountedDisplayFunctionLibrary::SetTrackingOrigin(EHMDTrackingOrigin::Local);
		Recenter();
	}
	if (UWheelInputSubsystem* Wheel = GetWheel())
	{
		Wheel->SetWheelRange(GetDefault<UWheelInputSettings>()->WheelRangeDegrees);
	}
}

void ACarPawn::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWheelInputSubsystem* Wheel = GetWheel())
	{
		Wheel->SetSteeringForce(0.f);
		Wheel->SetSteeringResistance(0.f, 0.f);
	}
	Super::EndPlay(EndPlayReason);
}

void ACarPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	PlayerInputComponent->BindKey(EKeys::R, IE_Pressed, this, &ACarPawn::Recenter);
	PlayerInputComponent->BindKey(EKeys::E, IE_Pressed, this, &ACarPawn::ToggleEngine);
	PlayerInputComponent->BindKey(EKeys::BackSpace, IE_Pressed, this, &ACarPawn::ResetCarUpright);
	auto BindPressed = [PlayerInputComponent](const FKey& Key, TFunction<void()> Action)
	{
		FInputKeyBinding Binding(FInputChord(Key), IE_Pressed);
		Binding.KeyDelegate.GetDelegateForManualSet().BindLambda(MoveTemp(Action));
		PlayerInputComponent->KeyBindings.Add(MoveTemp(Binding));
	};
	BindPressed(EKeys::SpaceBar, [this]() { bParkingBrake = !bParkingBrake; });
	BindPressed(EKeys::B, [this]() { KeyGear = -1; });
	const FKey GearKeys[] = {EKeys::N, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six};
	for (int32 Gear = 0; Gear < UE_ARRAY_COUNT(GearKeys); ++Gear)
	{
		BindPressed(GearKeys[Gear], [this, Gear]() { KeyGear = Gear; });
	}
	PlayerInputComponent->BindAxisKey(EKeys::MouseX, this, &ACarPawn::LookYaw);
	PlayerInputComponent->BindAxisKey(EKeys::MouseY, this, &ACarPawn::LookPitch);
}

void ACarPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UCarMovementComponent* Movement = GetCarMovement();
	if (!Movement)
	{
		return;
	}
	Movement->SetDriverInput(GatherInput(DeltaSeconds));

	// Chaos puts resting bodies to sleep, which would freeze the engine simulation too.
	if (!GetMesh()->IsAnyRigidBodyAwake())
	{
		GetMesh()->WakeAllRigidBodies();
	}

	const FCarTelemetry Telemetry = Movement->GetTelemetry();
	UpdateForceFeedback(Telemetry);
	UpdateDashboard(Telemetry);
}

FCarDriverInput ACarPawn::GatherInput(float DeltaSeconds)
{
	if (bAutopilot)
	{
		return AutopilotInput;
	}
	if (!IsPlayerControlled())
	{
		// Parked car (e.g. -FreeCam -SpawnCar): engine idling, parking brake on, ignore wheel and keyboard.
		FCarDriverInput Parked;
		Parked.bHandbrake = true;
		return Parked;
	}

	const APlayerController* PC = Cast<APlayerController>(GetController());
	auto Key = [PC](const FKey& K) { return PC && PC->IsInputKeyDown(K); };
	auto Ramp = [DeltaSeconds](float Value, bool bDown, float Rise, float Fall)
	{
		return FMath::Clamp(Value + (bDown ? Rise : -Fall) * DeltaSeconds, 0.f, 1.f);
	};
	KeyThrottle = Ramp(KeyThrottle, Key(EKeys::W) || Key(EKeys::Up), KeyThrottleRise, KeyThrottleFall);
	KeyBrake = Ramp(KeyBrake, Key(EKeys::S) || Key(EKeys::Down), KeyBrakeRise, KeyBrakeFall);
	KeyClutch = Ramp(KeyClutch, Key(EKeys::LeftShift) || Key(EKeys::RightShift), KeyClutchPress, KeyClutchRelease);

	// Keyboard steering: a virtual steering wheel turned at a limited rate, with less lock at speed.
	const FCarTelemetry Telemetry = GetTelemetry();
	const float Speed = FMath::Abs(Telemetry.LocalVelocityMps.X);
	const float MaxWheelDeg = FMath::Clamp(450.f * FMath::Square(5.f / FMath::Max(5.f, Speed)), 25.f, 450.f);
	const float SteerRate = FMath::Lerp(360.f, 120.f, FMath::Clamp(Speed / 28.f, 0.f, 1.f));
	const float SteerInput = (Key(EKeys::D) || Key(EKeys::Right) ? 1.f : 0.f) - (Key(EKeys::A) || Key(EKeys::Left) ? 1.f : 0.f);
	const float TargetDeg = SteerInput * MaxWheelDeg;
	const float Rate = SteerInput != 0.f ? SteerRate : 2.f * SteerRate;
	KeySteeringDeg = FMath::FInterpConstantTo(KeySteeringDeg, TargetDeg, DeltaSeconds, Rate);

	FCarDriverInput Input;
	Input.SteeringWheelDeg = KeySteeringDeg;
	Input.Throttle = KeyThrottle;
	Input.Brake = KeyBrake;
	Input.Clutch = KeyClutch;
	Input.SelectedGear = KeyGear;

	if (UWheelInputSubsystem* Wheel = GetWheel())
	{
		const FWheelInputState State = Wheel->GetState();
		if (State.bConnected)
		{
			const UWheelInputSettings* WheelSettings = GetDefault<UWheelInputSettings>();
			Input.SteeringWheelDeg = SteerInput != 0.f ? KeySteeringDeg : State.SteeringDegrees;
			Input.Throttle = FMath::Max(Input.Throttle, State.Throttle);
			Input.Brake = FMath::Max(Input.Brake, State.Brake);
			Input.Clutch = FMath::Max(Input.Clutch, State.Clutch);
			Input.SelectedGear = State.ShifterGear;
			if (ButtonPressed(State.Buttons, PrevWheelButtons, WheelSettings->StartEngineButtonIndex))
			{
				ToggleEngine();
			}
			if (ButtonPressed(State.Buttons, PrevWheelButtons, WheelSettings->HandbrakeButtonIndex))
			{
				bParkingBrake = !bParkingBrake;
			}
			if (ButtonPressed(State.Buttons, PrevWheelButtons, WheelSettings->RecenterViewButtonIndex))
			{
				Recenter();
			}
			if (ButtonPressed(State.Buttons, PrevWheelButtons, WheelSettings->ResetCarButtonIndex))
			{
				ResetCarUpright();
			}
			PrevWheelButtons = State.Buttons;
		}
	}

	if (StarterTimeLeft > 0.f)
	{
		StarterTimeLeft = Telemetry.bEngineRunning && !Telemetry.bCranking ? 0.f : StarterTimeLeft - DeltaSeconds;
	}
	Input.bStarter = StarterTimeLeft > 0.f;
	Input.bIgnitionOn = bIgnitionOn;
	Input.bHandbrake = bParkingBrake;
	return Input;
}

void ACarPawn::UpdateForceFeedback(const FCarTelemetry& Telemetry)
{
	UWheelInputSubsystem* Wheel = GetWheel();
	if (!Wheel || bAutopilot || !IsPlayerControlled())
	{
		return;
	}
	const UCarSettings* Settings = GetDefault<UCarSettings>();
	const float Speed = FMath::Abs(Telemetry.LocalVelocityMps.X);

	// Electric power steering takes most of the rack torque (less at speed); none with the engine off.
	const float Assist = Telemetry.bEngineRunning
		? FMath::Lerp(Settings->PowerSteeringAssistLowSpeed, Settings->PowerSteeringAssistHighSpeed, FMath::Clamp(Speed / 27.8f, 0.f, 1.f))
		: 0.f;
	const float HandTorque = Telemetry.SteeringRackTorqueNm * (1.f - Assist);
	const float Force = FMath::Clamp(HandTorque / FMath::Max(0.1f, Settings->FfbFullScaleNm), -Settings->FfbMaxForce, Settings->FfbMaxForce);

	// Tyre scrub makes the steering heavy and slow when parked; the 1 kHz wheel thread applies this from wheel speed.
	const float Parked = FMath::Clamp(1.f - Speed / 3.f, 0.f, 1.f);
	const float NoAssistScale = Telemetry.bEngineRunning ? 1.f : 2.f;
	const float Damping = FMath::Lerp(Settings->FfbDampingMoving, Settings->FfbDampingParked, Parked) * NoAssistScale;
	const float Friction = FMath::Lerp(Settings->FfbFrictionMoving, Settings->FfbFrictionParked, Parked) * NoAssistScale;

	Wheel->SetSteeringForce(Force);
	Wheel->SetSteeringResistance(Damping, FMath::Min(Friction, Settings->FfbMaxForce));
}

void ACarPawn::UpdateDashboard(const FCarTelemetry& Telemetry)
{
	FString Gear = Telemetry.EngagedGear == -1 ? TEXT("R") : Telemetry.EngagedGear == 0 ? TEXT("N") : FString::FromInt(Telemetry.EngagedGear);
	FString Status;
	if (!bIgnitionOn)
	{
		Status = TEXT("ENGINE OFF - press E");
	}
	else if (!Telemetry.bEngineRunning)
	{
		Status = Telemetry.bCranking ? TEXT("starting...") : TEXT("STALLED - press E");
	}
	if (Telemetry.bGrinding)
	{
		Status += TEXT(" *GRIND* clutch!");
	}
	if (bParkingBrake)
	{
		Status += TEXT(" (P)");
	}
	if (Telemetry.bAbsActive)
	{
		Status += TEXT(" ABS");
	}
	Dashboard->SetText(FText::FromString(FString::Printf(TEXT("%.0f km/h   %s   %4.0f rpm\n%s"),
		FMath::Abs(Telemetry.SpeedKmh), *Gear, Telemetry.EngineRpm, *Status)));
}

void ACarPawn::ToggleEngine()
{
	const FCarTelemetry Telemetry = GetTelemetry();
	if (bIgnitionOn && Telemetry.bEngineRunning)
	{
		bIgnitionOn = false;
		StarterTimeLeft = 0.f;
	}
	else
	{
		bIgnitionOn = true;
		StarterTimeLeft = StarterMaxSeconds;
	}
}

void ACarPawn::PlaceCar(const FVector& GroundLocation, float Yaw, bool bEngineRunning)
{
	const FVector Location = GroundLocation + FVector(0.f, 0.f, GetDefault<UCarSettings>()->OriginHeightAboveGroundCm + 2.f);
	SetActorLocationAndRotation(Location, FRotator(0.f, Yaw, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
	GetMesh()->SetAllPhysicsLinearVelocity(FVector::ZeroVector);
	GetMesh()->SetAllPhysicsAngularVelocityInRadians(FVector::ZeroVector);
	bIgnitionOn = true;
	if (UCarMovementComponent* Movement = GetCarMovement())
	{
		Movement->ResetDrivetrain(bEngineRunning);
	}
}

void ACarPawn::ResetCarUpright()
{
	const FVector Location = GetActorLocation();
	const FCarTelemetry Telemetry = GetTelemetry();
	PlaceCar(Location + FVector(0.f, 0.f, 50.f), GetActorRotation().Yaw, Telemetry.bEngineRunning);
}

void ACarPawn::Recenter()
{
	if (IsHMDActive())
	{
		UHeadMountedDisplayFunctionLibrary::ResetOrientationAndPosition();
	}
	else
	{
		DesktopLook = FRotator::ZeroRotator;
		Camera->SetRelativeRotation(DesktopLook);
	}
}

void ACarPawn::LookYaw(float Value)
{
	if (!IsHMDActive() && Value != 0.f)
	{
		DesktopLook.Yaw = FMath::Clamp(DesktopLook.Yaw + Value, -150.f, 150.f);
		Camera->SetRelativeRotation(DesktopLook);
	}
}

void ACarPawn::LookPitch(float Value)
{
	if (!IsHMDActive() && Value != 0.f)
	{
		DesktopLook.Pitch = FMath::Clamp(DesktopLook.Pitch + Value, -60.f, 60.f);
		Camera->SetRelativeRotation(DesktopLook);
	}
}

bool ACarPawn::IsHMDActive() const
{
	return UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled();
}
