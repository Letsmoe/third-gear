#include "EvdevWheel.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"

#if PLATFORM_LINUX
THIRD_PARTY_INCLUDES_START
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
THIRD_PARTY_INCLUDES_END
#endif

DEFINE_LOG_CATEGORY_STATIC(LogWheelInput, Log, All);

namespace
{
constexpr double ReconnectIntervalSeconds = 1.0;
constexpr uint16 LogitechVendorId = 0x046d;
/** If the game hasn't refreshed the force for this long, it is faded out (pause, hitch, crash). */
constexpr double ForceTimeoutSeconds = 0.25;
/** Low-pass time constant for the measured wheel velocity. */
constexpr float VelocityFilterSeconds = 0.012f;
/** Velocity at which friction reaches ~76 % (tanh(1)) of its value, rad/s. */
constexpr float FrictionVelocityScale = 0.3f;

#if PLATFORM_LINUX
constexpr size_t BitsPerLong = sizeof(unsigned long) * 8;
constexpr size_t NumLongs(size_t Bits) { return (Bits + BitsPerLong - 1) / BitsPerLong; }
bool TestBit(size_t Bit, const unsigned long* Array) { return (Array[Bit / BitsPerLong] >> (Bit % BitsPerLong)) & 1; }

bool IsCandidateWheel(int Fd)
{
	input_id Id;
	unsigned long EvBits[NumLongs(EV_CNT)] = {};
	unsigned long AbsBits[NumLongs(ABS_CNT)] = {};
	if (ioctl(Fd, EVIOCGID, &Id) < 0 || Id.vendor != LogitechVendorId)
	{
		return false;
	}
	if (ioctl(Fd, EVIOCGBIT(0, sizeof(EvBits)), EvBits) < 0 || ioctl(Fd, EVIOCGBIT(EV_ABS, sizeof(AbsBits)), AbsBits) < 0)
	{
		return false;
	}
	return TestBit(EV_FF, EvBits) && TestBit(ABS_X, AbsBits);
}

void SendFF(int Fd, uint16 Code, int32 Value)
{
	input_event Event = {};
	Event.type = EV_FF;
	Event.code = Code;
	Event.value = Value;
	if (write(Fd, &Event, sizeof(Event)) != sizeof(Event))
	{
		UE_LOG(LogWheelInput, Verbose, TEXT("EV_FF write (code %d) failed: %hs"), Code, strerror(errno));
	}
}
#endif
}

FEvdevWheel::FEvdevWheel(const FEvdevWheelConfig& InConfig)
	: Config(InConfig)
{
	RequestedRange = Config.WheelRangeDegrees;
}

void FEvdevWheel::Start()
{
#if PLATFORM_LINUX
	if (!Thread)
	{
		Thread = FRunnableThread::Create(this, TEXT("WheelInput"), 0, TPri_AboveNormal);
	}
#endif
}

FEvdevWheel::~FEvdevWheel()
{
	if (Thread)
	{
		Thread->Kill(/*bShouldWait=*/true);
		delete Thread;
		Thread = nullptr;
	}
	Close();
}

FWheelInputState FEvdevWheel::GetState() const
{
	FScopeLock Lock(&StateLock);
	return State;
}

void FEvdevWheel::SetSteeringForce(float Normalized)
{
	RequestedForce.store(FMath::IsFinite(Normalized) ? FMath::Clamp(Normalized, -1.f, 1.f) : 0.f, std::memory_order_relaxed);
	LastForceRequestTime.store(FPlatformTime::Seconds(), std::memory_order_relaxed);
}

void FEvdevWheel::SetSteeringResistance(float Damping, float Friction)
{
	RequestedDamping.store(FMath::Clamp(FMath::IsFinite(Damping) ? Damping : 0.f, 0.f, 2.f), std::memory_order_relaxed);
	RequestedFriction.store(FMath::Clamp(FMath::IsFinite(Friction) ? Friction : 0.f, 0.f, 1.f), std::memory_order_relaxed);
}

void FEvdevWheel::SetWheelRange(int32 Degrees)
{
	RequestedRange.store(Degrees, std::memory_order_relaxed);
}

void FEvdevWheel::Stop()
{
	bStopRequested = true;
}

uint32 FEvdevWheel::Run()
{
#if PLATFORM_LINUX
	while (!bStopRequested)
	{
		if (Fd < 0)
		{
			const double Now = FPlatformTime::Seconds();
			if (Now < NextOpenAttemptTime || !TryOpen())
			{
				NextOpenAttemptTime = FMath::Max(NextOpenAttemptTime, Now + ReconnectIntervalSeconds);
				FPlatformProcess::Sleep(0.1f);
				continue;
			}
		}

		// ~1 kHz loop: wake on input, otherwise refresh force feedback every millisecond.
		pollfd Poll = {Fd, POLLIN, 0};
		const int Ready = poll(&Poll, 1, 1);
		if (Ready > 0 && (Poll.revents & (POLLERR | POLLHUP | POLLNVAL)))
		{
			UE_LOG(LogWheelInput, Warning, TEXT("Wheel %s disconnected"), *OpenPath);
			Close();
			continue;
		}
		if (Ready > 0 && (Poll.revents & POLLIN))
		{
			ReadEvents();
		}
		if (Fd >= 0)
		{
			const double Now = FPlatformTime::Seconds();
			UpdateSteeringVelocity(Now);
			UpdateForce(Now);
			const int32 Range = RequestedRange.load(std::memory_order_relaxed);
			if (Range != AppliedRange)
			{
				WriteRangeToSysfs(Range);
			}
		}
	}
	Close();
#endif
	return 0;
}

bool FEvdevWheel::TryOpen()
{
#if PLATFORM_LINUX
	TArray<FString> Candidates;
	if (!Config.DevicePath.IsEmpty())
	{
		Candidates.Add(Config.DevicePath);
	}
	else if (DIR* Dir = opendir("/dev/input"))
	{
		while (dirent* Entry = readdir(Dir))
		{
			if (strncmp(Entry->d_name, "event", 5) == 0)
			{
				Candidates.Add(FString::Printf(TEXT("/dev/input/%hs"), Entry->d_name));
			}
		}
		closedir(Dir);
		Candidates.Sort(); // deterministic choice if several wheels are connected
	}

	for (const FString& Path : Candidates)
	{
		const int Candidate = open(TCHAR_TO_UTF8(*Path), O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (Candidate < 0)
		{
			if (errno == EACCES && !Config.DevicePath.IsEmpty())
			{
				UE_LOG(LogWheelInput, Warning, TEXT("No permission to open %s (needs read+write for force feedback)"), *Path);
			}
			continue;
		}
		if (!IsCandidateWheel(Candidate))
		{
			close(Candidate);
			continue;
		}

		Fd = Candidate;
		OpenPath = Path;

		char Name[256] = "?";
		ioctl(Fd, EVIOCGNAME(sizeof(Name)), Name);
		input_id Id = {};
		ioctl(Fd, EVIOCGID, &Id);

		// Axis ranges and initial values of every axis the device has (logged to help verify the mapping).
		AxisRanges.Reset();
		RawAxes.Reset();
		unsigned long AbsBits[NumLongs(ABS_CNT)] = {};
		ioctl(Fd, EVIOCGBIT(EV_ABS, sizeof(AbsBits)), AbsBits);
		FString AxisList;
		for (int32 Code = 0; Code < ABS_CNT; ++Code)
		{
			input_absinfo Info = {};
			if (TestBit(Code, AbsBits) && ioctl(Fd, EVIOCGABS(Code), &Info) == 0)
			{
				AxisRanges.Add(Code, {Info.minimum, FMath::Max(Info.maximum, Info.minimum + 1)});
				RawAxes.Add(Code, Info.value);
				AxisList += FString::Printf(TEXT(" 0x%02x[%d..%d]=%d"), Code, Info.minimum, Info.maximum, Info.value);
			}
		}
		for (const int32 Code : {Config.SteeringAxis, Config.ThrottleAxis, Config.BrakeAxis, Config.ClutchAxis})
		{
			if (!AxisRanges.Contains(Code))
			{
				UE_LOG(LogWheelInput, Warning, TEXT("Configured axis 0x%02x does not exist on %s - check UWheelInputSettings"), Code, *OpenPath);
			}
		}

		// Buttons are addressed by index in the order of supported key codes (stable per device model).
		unsigned long KeyBits[NumLongs(KEY_CNT)] = {};
		ioctl(Fd, EVIOCGBIT(EV_KEY, sizeof(KeyBits)), KeyBits);
		KeyCodeToIndex.Reset();
		for (int32 Code = 0, Index = 0; Code < KEY_CNT; ++Code)
		{
			if (TestBit(Code, KeyBits))
			{
				KeyCodeToIndex.Add(Code, Index++);
			}
		}
		unsigned long KeyState[NumLongs(KEY_CNT)] = {};
		ioctl(Fd, EVIOCGKEY(sizeof(KeyState)), KeyState);
		Buttons = 0;
		for (const TPair<int32, int32>& Key : KeyCodeToIndex)
		{
			if (Key.Value < 64 && TestBit(Key.Key, KeyState))
			{
				Buttons |= int64(1) << Key.Value;
			}
		}

		// We compute all forces ourselves: no built-in centring spring, full gain.
		SendFF(Fd, FF_AUTOCENTER, 0);
		SendFF(Fd, FF_GAIN, 0xFFFF);

		ff_effect Effect = {};
		Effect.type = FF_CONSTANT;
		Effect.id = -1;
		Effect.direction = 0x4000;
		Effect.replay.length = 0; // infinite
		if (ioctl(Fd, EVIOCSFF, &Effect) == 0)
		{
			EffectId = Effect.id;
			LastSentLevel = 0;
			SendFF(Fd, EffectId, 1);
		}
		else
		{
			EffectId = -1;
			UE_LOG(LogWheelInput, Warning, TEXT("Could not upload force feedback effect: %hs"), strerror(errno));
		}

		AppliedRange = 0;
		SmoothedForce = 0.f;
		SteeringVelocity = 0.f;
		LastVelocityTime = 0.0;
		{
			FScopeLock Lock(&StateLock);
			State = FWheelInputState();
			State.bConnected = true;
		}
		PublishState();
		LastSteeringRad = SteeringRad;
		UE_LOG(LogWheelInput, Log, TEXT("Opened wheel %s (%04x:%04x %hs), %d buttons, FFB %s. Axes:%s"), *OpenPath, Id.vendor,
		       Id.product, Name, KeyCodeToIndex.Num(), EffectId >= 0 ? TEXT("on") : TEXT("off"), *AxisList);
		return true;
	}
#endif
	return false;
}

void FEvdevWheel::Close()
{
#if PLATFORM_LINUX
	if (Fd >= 0)
	{
		if (EffectId >= 0)
		{
			SendFF(Fd, EffectId, 0);
			ioctl(Fd, EVIOCRMFF, EffectId);
		}
		close(Fd);
	}
#endif
	Fd = -1;
	EffectId = -1;
	Buttons = 0;
	FScopeLock Lock(&StateLock);
	State = FWheelInputState();
}

void FEvdevWheel::ReadEvents()
{
#if PLATFORM_LINUX
	input_event Events[64];
	bool bChanged = false;

	for (;;)
	{
		const ssize_t Bytes = read(Fd, Events, sizeof(Events));
		if (Bytes <= 0)
		{
			if (Bytes < 0 && errno != EAGAIN && errno != EINTR)
			{
				UE_LOG(LogWheelInput, Warning, TEXT("Read from %s failed: %hs"), *OpenPath, strerror(errno));
				Close();
				return;
			}
			break;
		}
		for (size_t i = 0; i < size_t(Bytes) / sizeof(input_event); ++i)
		{
			const input_event& Event = Events[i];
			if (Event.type == EV_ABS)
			{
				RawAxes.Add(Event.code, Event.value);
				bChanged = true;
				if (Config.bLogInput && Event.code != Config.SteeringAxis)
				{
					UE_LOG(LogWheelInput, Log, TEXT("axis 0x%02x = %d"), Event.code, Event.value);
				}
			}
			else if (Event.type == EV_KEY)
			{
				if (const int32* Index = KeyCodeToIndex.Find(Event.code); Index && *Index < 64)
				{
					Buttons = Event.value ? (Buttons | (int64(1) << *Index)) : (Buttons & ~(int64(1) << *Index));
					bChanged = true;
					if (Config.bLogInput)
					{
						UE_LOG(LogWheelInput, Log, TEXT("button index %d (code 0x%x) %s"), *Index, Event.code, Event.value ? TEXT("down") : TEXT("up"));
					}
				}
			}
			else if (Event.type == EV_SYN && Event.code == SYN_DROPPED)
			{
				// Kernel buffer overflowed: resync absolute values and the button state.
				for (TPair<int32, int32>& Axis : RawAxes)
				{
					input_absinfo Info = {};
					if (ioctl(Fd, EVIOCGABS(Axis.Key), &Info) == 0)
					{
						Axis.Value = Info.value;
					}
				}
				unsigned long KeyState[NumLongs(KEY_CNT)] = {};
				if (ioctl(Fd, EVIOCGKEY(sizeof(KeyState)), KeyState) >= 0)
				{
					Buttons = 0;
					for (const TPair<int32, int32>& Key : KeyCodeToIndex)
					{
						if (Key.Value < 64 && TestBit(Key.Key, KeyState))
						{
							Buttons |= int64(1) << Key.Value;
						}
					}
				}
				bChanged = true;
			}
		}
	}

	if (bChanged)
	{
		PublishState();
	}
#endif
}

void FEvdevWheel::PublishState()
{
	auto Normalized = [this](int32 Code) -> float
	{
		const FAxisRange* Range = AxisRanges.Find(Code);
		const int32* Raw = RawAxes.Find(Code);
		if (!Range || !Raw)
		{
			return 0.f;
		}
		return FMath::Clamp(float(*Raw - Range->Min) / float(Range->Max - Range->Min), 0.f, 1.f);
	};
	auto Pedal = [this, &Normalized](int32 Code, bool bInvert) -> float
	{
		if (!AxisRanges.Contains(Code))
		{
			return 0.f; // missing pedal: treat as released rather than fully pressed
		}
		float Value = Normalized(Code);
		if (bInvert)
		{
			Value = 1.f - Value;
		}
		return FMath::Clamp((Value - Config.PedalDeadzone) / (1.f - Config.PedalDeadzone), 0.f, 1.f);
	};

	int32 Gear = 0;
	for (int32 i = 0; i < Config.GearButtonIndices.Num(); ++i)
	{
		const int32 Index = Config.GearButtonIndices[i];
		if (Index >= 0 && Index < 64 && ((Buttons >> Index) & 1))
		{
			Gear = i + 1;
		}
	}
	if (Config.ReverseButtonIndex >= 0 && Config.ReverseButtonIndex < 64 && ((Buttons >> Config.ReverseButtonIndex) & 1))
	{
		Gear = -1;
	}

	float Steering = Normalized(Config.SteeringAxis) * 2.f - 1.f;
	if (Config.bInvertSteering)
	{
		Steering = -Steering;
	}
	const int32 RangeDegrees = AppliedRange > 0 ? AppliedRange : (Config.WheelRangeDegrees > 0 ? Config.WheelRangeDegrees : 900);
	SteeringRad = FMath::DegreesToRadians(Steering * RangeDegrees * 0.5f);

	FScopeLock Lock(&StateLock);
	State.Steering = Steering;
	State.SteeringDegrees = Steering * RangeDegrees * 0.5f;
	State.Throttle = Pedal(Config.ThrottleAxis, Config.bInvertThrottle);
	State.Brake = FMath::Min(Pedal(Config.BrakeAxis, Config.bInvertBrake) / Config.BrakeFullTravel, 1.f);
	State.Clutch = Pedal(Config.ClutchAxis, Config.bInvertClutch);
	State.ShifterGear = Gear;
	State.Buttons = Buttons;
}

void FEvdevWheel::UpdateSteeringVelocity(double Now)
{
	if (LastVelocityTime <= 0.0)
	{
		LastVelocityTime = Now;
		LastSteeringRad = SteeringRad;
		return;
	}
	const float Dt = float(Now - LastVelocityTime);
	if (Dt < 0.0005f)
	{
		return;
	}
	const float RawVelocity = (SteeringRad - LastSteeringRad) / Dt;
	SteeringVelocity += (RawVelocity - SteeringVelocity) * FMath::Min(1.f, Dt / VelocityFilterSeconds);
	LastSteeringRad = SteeringRad;
	LastVelocityTime = Now;

	FScopeLock Lock(&StateLock);
	State.SteeringVelocityDegPerSec = FMath::RadiansToDegrees(SteeringVelocity);
}

void FEvdevWheel::UpdateForce(double Now)
{
#if PLATFORM_LINUX
	if (EffectId < 0)
	{
		return;
	}
	// Target torque from the game, faded out if the game stopped refreshing it.
	const bool bFresh = Now - LastForceRequestTime.load(std::memory_order_relaxed) < ForceTimeoutSeconds;
	float Force = bFresh ? RequestedForce.load(std::memory_order_relaxed) : 0.f;
	if (bFresh)
	{
		Force -= RequestedDamping.load(std::memory_order_relaxed) * SteeringVelocity;
		Force -= RequestedFriction.load(std::memory_order_relaxed) * FMath::Tanh(SteeringVelocity / FrictionVelocityScale);
	}
	// Short slew limit avoids clicks when the game's target jumps (it is only updated at frame rate).
	const float MaxStep = 0.02f; // per ~1 ms tick: full scale in 50 ms
	SmoothedForce += FMath::Clamp(Force - SmoothedForce, -MaxStep, MaxStep);

	const float Sign = Config.bInvertForce ? -1.f : 1.f;
	const float Output = FMath::Clamp(SmoothedForce * Config.ForceGain * Sign, -1.f, 1.f);
	const int16 Level = int16(FMath::RoundToInt(Output * 32767.f));
	if (Level == LastSentLevel)
	{
		return;
	}

	ff_effect Effect = {};
	Effect.type = FF_CONSTANT;
	Effect.id = EffectId;
	Effect.direction = 0x4000;
	Effect.replay.length = 0;
	Effect.u.constant.level = Level;
	if (ioctl(Fd, EVIOCSFF, &Effect) == 0)
	{
		LastSentLevel = Level;
	}
#endif
}

void FEvdevWheel::WriteRangeToSysfs(int32 Degrees)
{
	AppliedRange = Degrees; // don't retry every tick on failure
#if PLATFORM_LINUX
	if (Degrees <= 0)
	{
		return;
	}
	// /dev/input/eventN -> /sys/class/input/eventN/device/device/range (HID device attribute from new-lg4ff)
	char Resolved[PATH_MAX];
	if (!realpath(TCHAR_TO_UTF8(*OpenPath), Resolved))
	{
		return;
	}
	const char* Node = strrchr(Resolved, '/');
	char SysfsPath[PATH_MAX];
	snprintf(SysfsPath, sizeof(SysfsPath), "/sys/class/input%s/device/device/range", Node ? Node : "");
	if (FILE* File = fopen(SysfsPath, "w"))
	{
		fprintf(File, "%d", Degrees);
		fclose(File);
		UE_LOG(LogWheelInput, Log, TEXT("Wheel range set to %d degrees"), Degrees);
	}
	else
	{
		UE_LOG(LogWheelInput, Warning, TEXT("Could not set wheel range via %hs: %hs (steering angle mapping assumes %d degrees)"),
		       SysfsPath, strerror(errno), Degrees);
	}
	PublishState(); // steering degrees depend on the range
#endif
}
