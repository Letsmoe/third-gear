#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "WheelInputSettings.generated.h"

/**
 * Device mapping for the wheel, section [/Script/WheelInput.WheelInputSettings] in Config/DefaultGame.ini.
 * Defaults target a Logitech G923 with the Driving Force Shifter under the new-lg4ff driver but are UNVERIFIED:
 * check them with Tools/wheeltest ("monitor" prints axis codes and button indices, "ffb" the force direction)
 * or run the game with -WheelDebug (logs every axis/button change with the same numbering).
 * Axis codes are Linux ABS_* values (ABS_X=0x00, ABS_Y=0x01, ABS_Z=0x02, ABS_RX=0x03, ABS_RY=0x04, ABS_RZ=0x05).
 * Button indices count the device's supported key codes in ascending order (as printed by wheeltest).
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Wheel Input"))
class WHEELINPUT_API UWheelInputSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Explicit /dev/input/eventN path (or /dev/input/by-id/... link); empty = auto-detect the first Logitech device with force feedback. */
	UPROPERTY(Config, EditAnywhere, Category = "Device")
	FString DevicePath;

	/** Rotation range (lock to lock) to set on the wheel, in degrees. G923 max is 900. 0 = leave unchanged. */
	UPROPERTY(Config, EditAnywhere, Category = "Device", meta = (ClampMin = "0", ClampMax = "900"))
	int32 WheelRangeDegrees = 900;

	UPROPERTY(Config, EditAnywhere, Category = "Axes")
	int32 SteeringAxis = 0x00;

	/** Flip if turning the wheel to the right reports a negative steering value. */
	UPROPERTY(Config, EditAnywhere, Category = "Axes")
	bool bInvertSteering = false;

	UPROPERTY(Config, EditAnywhere, Category = "Axes")
	int32 ThrottleAxis = 0x02;

	UPROPERTY(Config, EditAnywhere, Category = "Axes")
	int32 BrakeAxis = 0x05;

	UPROPERTY(Config, EditAnywhere, Category = "Axes")
	int32 ClutchAxis = 0x01;

	/** Logitech pedals usually report their maximum when released; true maps that to 0 (released). */
	UPROPERTY(Config, EditAnywhere, Category = "Axes")
	bool bInvertThrottle = true;

	UPROPERTY(Config, EditAnywhere, Category = "Axes")
	bool bInvertBrake = true;

	UPROPERTY(Config, EditAnywhere, Category = "Axes")
	bool bInvertClutch = true;

	/** Pedal travel ignored at the released end, 0..1. */
	UPROPERTY(Config, EditAnywhere, Category = "Axes")
	float PedalDeadzone = 0.03f;

	/** Button indices for gears 1..N of the H-shifter. No gear button pressed = neutral. */
	UPROPERTY(Config, EditAnywhere, Category = "Shifter")
	TArray<int32> GearButtonIndices = {12, 13, 14, 15, 16, 17};

	UPROPERTY(Config, EditAnywhere, Category = "Shifter")
	int32 ReverseButtonIndex = 18;

	/** Wheel buttons used by the game (-1 = unused; pick any wheel button index from wheeltest monitor). */
	UPROPERTY(Config, EditAnywhere, Category = "Buttons")
	int32 StartEngineButtonIndex = -1;

	/** Toggles the parking brake. */
	UPROPERTY(Config, EditAnywhere, Category = "Buttons")
	int32 HandbrakeButtonIndex = -1;

	UPROPERTY(Config, EditAnywhere, Category = "Buttons")
	int32 RecenterViewButtonIndex = -1;

	/** Puts the car back upright on the road at its current position. */
	UPROPERTY(Config, EditAnywhere, Category = "Buttons")
	int32 ResetCarButtonIndex = -1;

	/** Flip if a positive (clockwise) torque request pulls the wheel to the LEFT (wheeltest ffb). */
	UPROPERTY(Config, EditAnywhere, Category = "Force Feedback")
	bool bInvertForce = false;

	/** Global force multiplier, 0..1. */
	UPROPERTY(Config, EditAnywhere, Category = "Force Feedback", meta = (ClampMin = "0", ClampMax = "1"))
	float ForceGain = 1.0f;

	/** Log every axis and button change (also enabled by the -WheelDebug command-line switch). */
	UPROPERTY(Config, EditAnywhere, Category = "Debug")
	bool bLogInput = false;
};
