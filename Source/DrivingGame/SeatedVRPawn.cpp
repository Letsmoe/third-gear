#include "SeatedVRPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"

ASeatedVRPawn::ASeatedVRPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	AutoPossessPlayer = EAutoReceiveInput::Player0;

	EyeOrigin = CreateDefaultSubobject<USceneComponent>(TEXT("EyeOrigin"));
	SetRootComponent(EyeOrigin);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(EyeOrigin);
	Camera->bLockToHmd = true;
	// Desktop fallback: let the controller rotation drive the camera.
	Camera->bUsePawnControlRotation = false;

	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = true;
}

void ASeatedVRPawn::BeginPlay()
{
	Super::BeginPlay();

	if (IsHMDActive())
	{
		// Seated experience: origin at the head's starting pose, not the floor.
		UHeadMountedDisplayFunctionLibrary::SetTrackingOrigin(EHMDTrackingOrigin::Local);
		Recenter();
		// Head tracking supplies the view rotation; don't let the controller add to it.
		bUseControllerRotationYaw = false;
		bUseControllerRotationPitch = false;
	}
}

void ASeatedVRPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Temporary free movement until the car exists: WASD along the view direction, Q/E down/up, Shift = 50 km/h.
	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return;
	}
	auto Axis = [PC](const FKey& Positive, const FKey& Negative)
	{
		return (PC->IsInputKeyDown(Positive) ? 1.f : 0.f) - (PC->IsInputKeyDown(Negative) ? 1.f : 0.f);
	};
	const FVector Input(Axis(EKeys::W, EKeys::S), Axis(EKeys::D, EKeys::A), Axis(EKeys::E, EKeys::Q));
	if (Input.IsNearlyZero())
	{
		return;
	}
	const FRotator View = Camera->GetComponentRotation();
	const FVector Forward = FRotator(0.f, View.Yaw, 0.f).Vector();
	const FVector Right = FRotator(0.f, View.Yaw + 90.f, 0.f).Vector();
	const float Speed = PC->IsInputKeyDown(EKeys::LeftShift) ? 1389.f : 140.f; // cm/s: 50 km/h or walking
	AddActorWorldOffset((Forward * Input.X + Right * Input.Y + FVector::UpVector * Input.Z) * Speed * DeltaSeconds);
}

void ASeatedVRPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	PlayerInputComponent->BindKey(EKeys::R, IE_Pressed, this, &ASeatedVRPawn::Recenter);
	PlayerInputComponent->BindAxisKey(EKeys::MouseX, this, &ASeatedVRPawn::LookYaw);
	PlayerInputComponent->BindAxisKey(EKeys::MouseY, this, &ASeatedVRPawn::LookPitch);
}

void ASeatedVRPawn::Recenter()
{
	if (IsHMDActive())
	{
		UHeadMountedDisplayFunctionLibrary::ResetOrientationAndPosition();
	}
}

void ASeatedVRPawn::LookYaw(float Value)
{
	if (!IsHMDActive() && Value != 0.f)
	{
		AddControllerYawInput(Value);
	}
}

void ASeatedVRPawn::LookPitch(float Value)
{
	if (!IsHMDActive() && Value != 0.f)
	{
		AddControllerPitchInput(-Value);
	}
}

bool ASeatedVRPawn::IsHMDActive() const
{
	return UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled();
}
