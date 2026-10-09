#include "WheelInputSubsystem.h"

#include "EvdevWheel.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UserSettingsFile.h"
#include "WheelInputSettings.h"

namespace
{
/** Copies the (game-thread) settings object into the plain struct the worker thread uses. */
FEvdevWheelConfig MakeConfigFromSettings()
{
	const UWheelInputSettings* Settings = GetDefault<UWheelInputSettings>();
	FEvdevWheelConfig Config;
	Config.DevicePath = Settings->DevicePath;
	Config.WheelRangeDegrees = Settings->WheelRangeDegrees;
	Config.SteeringAxis = Settings->SteeringAxis;
	Config.bInvertSteering = Settings->bInvertSteering;
	Config.ThrottleAxis = Settings->ThrottleAxis;
	Config.BrakeAxis = Settings->BrakeAxis;
	Config.ClutchAxis = Settings->ClutchAxis;
	Config.bInvertThrottle = Settings->bInvertThrottle;
	Config.bInvertBrake = Settings->bInvertBrake;
	Config.bInvertClutch = Settings->bInvertClutch;
	Config.PedalDeadzone = FMath::Clamp(Settings->PedalDeadzone, 0.f, 0.5f);
	Config.BrakeFullTravel = FMath::Clamp(Settings->BrakeFullTravel, 0.3f, 1.f);
	Config.GearButtonIndices = Settings->GearButtonIndices;
	Config.ReverseButtonIndex = Settings->ReverseButtonIndex;
	Config.DPadXAxis = Settings->DPadXAxis;
	Config.DPadYAxis = Settings->DPadYAxis;
	Config.bInvertForce = Settings->bInvertForce;
	Config.ForceGain = FMath::Clamp(Settings->ForceGain, 0.f, 1.f);
	Config.bLogInput = Settings->bLogInput || FParse::Param(FCommandLine::Get(), TEXT("WheelDebug"));
	return Config;
}
}

void UWheelInputSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Per-user overrides saved by the in-game menu (Saved/Config/<platform>/UserSettings.ini) win over Config/DefaultGame.ini.
	if (!IsRunningCommandlet())
	{
		FUserSettingsFile::LoadOverrides(GetMutableDefault<UWheelInputSettings>(), UWheelInputSettings::GetUserEditableProperties());
	}

	// No device access in commandlets (cooking, headless scripts) or with -NoWheel (automated tests).
	if (IsRunningCommandlet() || FParse::Param(FCommandLine::Get(), TEXT("NoWheel")))
	{
		return;
	}

	Wheel = MakeShared<FEvdevWheel>(MakeConfigFromSettings());
	Wheel->Start();
}

void UWheelInputSubsystem::Deinitialize()
{
	Wheel.Reset();
	Super::Deinitialize();
}

FWheelInputState UWheelInputSubsystem::GetState() const
{
	return Wheel ? Wheel->GetState() : FWheelInputState();
}

void UWheelInputSubsystem::ApplySettings()
{
	if (!Wheel)
	{
		return;
	}
	Wheel->SetConfig(MakeConfigFromSettings());
	Wheel->SetWheelRange(GetDefault<UWheelInputSettings>()->WheelRangeDegrees);
}

void UWheelInputSubsystem::SetSteeringForce(float Normalized)
{
	if (Wheel)
	{
		Wheel->SetSteeringForce(Normalized);
	}
}

void UWheelInputSubsystem::SetSteeringResistance(float Damping, float Friction)
{
	if (Wheel)
	{
		Wheel->SetSteeringResistance(Damping, Friction);
	}
}

void UWheelInputSubsystem::SetWheelRange(int32 Degrees)
{
	if (Wheel)
	{
		Wheel->SetWheelRange(Degrees);
	}
}

bool UWheelInputSubsystem::IsConnected() const
{
	return GetState().bConnected;
}
