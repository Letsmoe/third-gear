#pragma once

#include "CoreMinimal.h"
#include "ChaosWheeledVehicleMovementComponent.h"
#include "CarSimTypes.h"
#include "CarSurfaceGrip.h"
#include "CarMovementComponent.generated.h"

/**
 * Chaos wheeled vehicle with our own physics on top: Chaos provides the rigid body, suspension raycasts/springs and
 * the wheel animation data; engine, clutch, gearbox, differential, brakes, tyres and steering geometry are replaced by
 * FCarDrivetrain, run on the physics thread at every physics step (see FCarVehicleSimulation in the .cpp).
 * All values come from UCarSettings. Communication with the game thread goes through FCarSharedState.
 */
UCLASS()
class DRIVINGGAME_API UCarMovementComponent : public UChaosWheeledVehicleMovementComponent
{
	GENERATED_BODY()

public:
	UCarMovementComponent(const FObjectInitializer& ObjectInitializer);

	/** Driver controls for the next physics steps (thread-safe). */
	void SetDriverInput(const FCarDriverInput& Input);

	/** Latest physics-thread output (thread-safe copy). */
	FCarTelemetry GetTelemetry() const;

	/** Resets engine/gearbox/wheel spin state on the next physics step (use after teleporting the car). */
	void ResetDrivetrain(bool bEngineRunning);

	/** Replaces the weather and surface material under the wheels with fixed conditions (drive test). */
	void SetSurfaceConditionsOverride(const FCarSurfaceConditions& Conditions);

	virtual TUniquePtr<Chaos::FSimpleWheeledVehicle> CreatePhysicsVehicle() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void OnCreatePhysicsState() override;

private:
	/** Looks at the ground under each wheel a few times per second and hands the grip to the physics thread. */
	void UpdateWheelSurfaces(float DeltaTime);

	/** Weather as the tyres feel it: the override if set, else the weather visuals' wetness, snow and temperature. */
	FCarSurfaceConditions CurrentSurfaceConditions() const;

	TSharedRef<FCarSharedState, ESPMode::ThreadSafe> Shared;

	float SurfaceUpdateCountdown = 0.f;
	bool bSurfaceOverride = false;
	FCarSurfaceConditions SurfaceOverride;
};
