#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "WheelInputTypes.h"
#include "WheelInputSubsystem.generated.h"

class FEvdevWheel;

/**
 * Owns the wheel device for the lifetime of the engine (survives PIE sessions).
 * Input is read and force feedback written on a dedicated ~1 kHz thread. The game sets a target torque
 * (from the vehicle simulation) plus damping/friction coefficients; the thread adds damping and friction from
 * the wheel's own measured angular velocity at 1 kHz, which is far more stable than doing it at frame rate.
 * If the game stops updating the force (pause, hitch, crash), the force fades out after a short timeout.
 */
UCLASS()
class WHEELINPUT_API UWheelInputSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Thread-safe snapshot of the current input. */
	UFUNCTION(BlueprintPure, Category = "Wheel")
	FWheelInputState GetState() const;

	/**
	 * Applies the current values of UWheelInputSettings (axes, inversion, deadzones, buttons, force sign and gain, range)
	 * to the running device without a restart. Call on the game thread after changing the settings object.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wheel")
	void ApplySettings();

	/**
	 * Requests a steering torque, normalised to the wheel's maximum: +1 = full force turning the wheel clockwise (right).
	 * Thread-safe. Must be refreshed regularly (at least every 0.25 s) or it fades out.
	 */
	UFUNCTION(BlueprintCallable, Category = "Wheel")
	void SetSteeringForce(float Normalized);

	/**
	 * Resistance computed on the 1 kHz thread from the wheel's angular velocity w (rad/s of the physical wheel):
	 * force -= Damping * w + Friction * tanh(w / 0.3). Both in units of the normalised force (1 = wheel maximum).
	 */
	UFUNCTION(BlueprintCallable, Category = "Wheel")
	void SetSteeringResistance(float Damping, float Friction);

	/** Sets the rotation range (lock to lock) in degrees, e.g. to match the car's steering. */
	UFUNCTION(BlueprintCallable, Category = "Wheel")
	void SetWheelRange(int32 Degrees);

	UFUNCTION(BlueprintPure, Category = "Wheel")
	bool IsConnected() const;

private:
	TSharedPtr<FEvdevWheel> Wheel;
};
