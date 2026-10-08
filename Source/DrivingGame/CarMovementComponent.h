#pragma once

#include "CoreMinimal.h"
#include "ChaosWheeledVehicleMovementComponent.h"
#include "CarSimTypes.h"
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

	virtual TUniquePtr<Chaos::FSimpleWheeledVehicle> CreatePhysicsVehicle() override;

protected:
	virtual void OnCreatePhysicsState() override;

private:
	TSharedRef<FCarSharedState, ESPMode::ThreadSafe> Shared;
};
