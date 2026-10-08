#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "WheelInputTypes.h"

#include <atomic>

/** Copy of UWheelInputSettings taken on the game thread, so the worker never touches UObjects. */
struct FEvdevWheelConfig
{
	FString DevicePath;
	int32 WheelRangeDegrees = 900;
	int32 SteeringAxis = 0;
	bool bInvertSteering = false;
	int32 ThrottleAxis = 2;
	int32 BrakeAxis = 5;
	int32 ClutchAxis = 1;
	bool bInvertThrottle = true;
	bool bInvertBrake = true;
	bool bInvertClutch = true;
	float PedalDeadzone = 0.03f;
	TArray<int32> GearButtonIndices;
	int32 ReverseButtonIndex = -1;
	bool bInvertForce = false;
	float ForceGain = 1.f;
	bool bLogInput = false;
};

/**
 * Linux evdev wheel: auto-detects/reconnects the device, decodes axes and buttons,
 * and streams a constant-force effect (target torque + damping/friction computed here at ~1 kHz).
 * On other platforms it does nothing.
 */
class FEvdevWheel : public FRunnable
{
public:
	explicit FEvdevWheel(const FEvdevWheelConfig& InConfig);
	virtual ~FEvdevWheel() override;

	/** Starts the worker thread (separate from the constructor so the object is fully built first). */
	void Start();

	FWheelInputState GetState() const;
	void SetSteeringForce(float Normalized);
	void SetSteeringResistance(float Damping, float Friction);
	void SetWheelRange(int32 Degrees);

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	bool TryOpen();
	void Close();
	void ReadEvents();
	void PublishState();
	void UpdateSteeringVelocity(double Now);
	void UpdateForce(double Now);
	void WriteRangeToSysfs(int32 Degrees);

	struct FAxisRange
	{
		int32 Min = 0;
		int32 Max = 1;
	};

	FEvdevWheelConfig Config;
	FRunnableThread* Thread = nullptr;
	FThreadSafeBool bStopRequested = false;

	// Worker-thread-only device state.
	int Fd = -1;
	FString OpenPath;
	int16 EffectId = -1;
	int16 LastSentLevel = 0;
	TMap<int32, FAxisRange> AxisRanges;
	TMap<int32, int32> KeyCodeToIndex;
	TMap<int32, int32> RawAxes;
	int64 Buttons = 0;
	double NextOpenAttemptTime = 0.0;
	float SteeringRad = 0.f;           // physical wheel angle, + = clockwise
	float LastSteeringRad = 0.f;
	double LastVelocityTime = 0.0;
	float SteeringVelocity = 0.f;      // rad/s, low-pass filtered
	float SmoothedForce = 0.f;

	mutable FCriticalSection StateLock;
	FWheelInputState State;

	std::atomic<float> RequestedForce{0.f};
	std::atomic<float> RequestedDamping{0.f};
	std::atomic<float> RequestedFriction{0.f};
	std::atomic<double> LastForceRequestTime{0.0};
	std::atomic<int32> RequestedRange{0};
	int32 AppliedRange = 0;
};
