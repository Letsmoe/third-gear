#pragma once

#include "CoreMinimal.h"
#include "WheelInputTypes.generated.h"

/** Snapshot of the wheel, pedals and H-shifter, normalised. */
USTRUCT(BlueprintType)
struct WHEELINPUT_API FWheelInputState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Wheel")
	bool bConnected = false;

	/** -1 = full left lock, +1 = full right lock (of the configured wheel range). */
	UPROPERTY(BlueprintReadOnly, Category = "Wheel")
	float Steering = 0.f;

	/** Physical wheel angle in degrees, + = clockwise (right). */
	UPROPERTY(BlueprintReadOnly, Category = "Wheel")
	float SteeringDegrees = 0.f;

	/** Filtered angular velocity of the wheel in degrees per second, + = clockwise. */
	UPROPERTY(BlueprintReadOnly, Category = "Wheel")
	float SteeringVelocityDegPerSec = 0.f;

	/** Pedals 0 = released, 1 = fully pressed. */
	UPROPERTY(BlueprintReadOnly, Category = "Wheel")
	float Throttle = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Wheel")
	float Brake = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Wheel")
	float Clutch = 0.f;

	/** H-shifter position: -1 = reverse, 0 = neutral, 1..N = gear. */
	UPROPERTY(BlueprintReadOnly, Category = "Wheel")
	int32 ShifterGear = 0;

	/** Bit i set = button with evdev index i is held (index = order of supported key codes, as printed by wheeltest). */
	UPROPERTY(BlueprintReadOnly, Category = "Wheel")
	int64 Buttons = 0;

	/** D-pad (hat axes ABS_HAT0X / ABS_HAT0Y): X = -1 left, +1 right; Y = -1 up, +1 down; 0 = centred. */
	UPROPERTY(BlueprintReadOnly, Category = "Wheel")
	int32 DPadX = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Wheel")
	int32 DPadY = 0;

	/** Every axis of the device as 0..1 of its range, indexed by the Linux ABS_* code; -1 where the device has no such axis.
	 *  Raw readings for calibration, independent of the configured axis mapping. Empty while disconnected. */
	UPROPERTY(BlueprintReadOnly, Category = "Wheel")
	TArray<float> AxisValues;

	bool IsButtonDown(int32 Index) const { return Index >= 0 && Index < 64 && (Buttons >> Index) & 1; }
};
