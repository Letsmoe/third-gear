#pragma once

#include "CoreMinimal.h"
#include "WheeledVehiclePawn.h"
#include "CarSimTypes.h"
#include "CarPawn.generated.h"

class UCameraComponent;
class UCarMovementComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

/**
 * The player car: skeletal mesh + attached static meshes from UCarSettings, UCarMovementComponent physics,
 * seated driver camera (HMD-locked, tracking origin "Local", R recentres) and a small speed/rpm/gear readout.
 *
 * Controls are read every frame from the Logitech wheel (UWheelInputSubsystem) and/or the keyboard:
 *   W/S throttle/brake, A/D steer, Left Shift clutch, 1-6 gears, N neutral, B reverse, E start/stop engine,
 *   Space parking brake, Backspace put car back on its wheels, R recentre view. Mouse looks around without HMD.
 * With the wheel connected the H-shifter selects the gear and the pedals/wheel take over (keyboard still adds).
 * Force feedback is computed from the simulated steering rack torque (see Tick).
 */
UCLASS(Config = Game)
class DRIVINGGAME_API ACarPawn : public AWheeledVehiclePawn
{
	GENERATED_BODY()

public:
	ACarPawn(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	UCarMovementComponent* GetCarMovement() const;
	FCarTelemetry GetTelemetry() const;

	/** Scripted driving (drive test): when enabled, the given input replaces wheel and keyboard. */
	void SetAutopilot(bool bEnable) { bAutopilot = bEnable; }
	void SetAutopilotInput(const FCarDriverInput& Input) { AutopilotInput = Input; }

	/** Teleports the car (origin on the ground at Location), stops it and resets the drivetrain. */
	void PlaceCar(const FVector& GroundLocation, float Yaw, bool bEngineRunning);

	/** Re-centres the seated tracking origin on the current head pose. */
	void Recenter();

	/** Puts the car back upright on the road where it stands. */
	void ResetCarUpright();

protected:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> EyeOrigin;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UTextRenderComponent> Dashboard;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TArray<TObjectPtr<UStaticMeshComponent>> AttachedMeshes;

private:
	FCarDriverInput GatherInput(float DeltaSeconds);
	void UpdateForceFeedback(const FCarTelemetry& Telemetry);
	void UpdateDashboard(const FCarTelemetry& Telemetry);
	void ToggleEngine();
	void LookYaw(float Value);
	void LookPitch(float Value);
	bool IsHMDActive() const;

	bool bAutopilot = false;
	FCarDriverInput AutopilotInput;

	// Keyboard state (ramped so digital keys give usable pedal/steering signals).
	float KeyThrottle = 0.f;
	float KeyBrake = 0.f;
	float KeyClutch = 0.f;
	float KeySteeringDeg = 0.f;
	int32 KeyGear = 0;
	bool bParkingBrake = false;
	bool bIgnitionOn = true;
	float StarterTimeLeft = 0.f;
	int64 PrevWheelButtons = 0;
	FRotator DesktopLook = FRotator::ZeroRotator;
};
