#include "MenuPages.h"

#include "CarSettings.h"
#include "DrivingPreferences.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"
#include "MenuController.h"
#include "UserSettingsFile.h"
#include "WheelInputSettings.h"
#include "WheelInputSubsystem.h"

namespace
{
/** A second press of "reset" within this time confirms it. */
constexpr double ResetConfirmSeconds = 4.0;

const FString RegionHamburg = TEXT("hamburg");
const FString RegionBergedorfCore = TEXT("bergedorf_core");
const FString RegionBergedorfTest = TEXT("bergedorf_test");

/** Resolution scale and view distance combinations offered as presets. */
const TArray<int32> PresetScreenPercentages = {70, 85, 100};
const TArray<int32> PresetViewDistances = {1500, 3000, 5000};
const TArray<FString> PresetNames = {TEXT("Performance"), TEXT("Balanced"), TEXT("Quality")};
constexpr int32 CustomPresetIndex = 3;

FString FormatNumber(float Value, int32 Decimals, const FString& Unit)
{
	const FString Number = FString::Printf(TEXT("%.*f"), Decimals, Value);
	return Unit.IsEmpty() ? Number : Number + TEXT(" ") + Unit;
}

UWheelInputSettings* GetWheelSettings()
{
	return GetMutableDefault<UWheelInputSettings>();
}

UCarSettings* GetCarSettings()
{
	return GetMutableDefault<UCarSettings>();
}

UDrivingPreferences* GetPreferences()
{
	return GetMutableDefault<UDrivingPreferences>();
}
}

FMenuPages::FMenuPages(FMenuController& InController, IGameMenuHost& InHost)
	: Controller(InController)
	, Host(InHost)
{
}

// ---------------------------------------------------------------------------------------------------------------------
// Row helpers
// ---------------------------------------------------------------------------------------------------------------------

void FMenuPages::AddHeader(FMenuPage& Page, const FString& Label)
{
	FMenuRow& Row = Page.Rows.AddDefaulted_GetRef();
	Row.Kind = FMenuRow::EKind::Header;
	Row.Label = Label;
}

void FMenuPages::AddAction(FMenuPage& Page, const FString& Label, const FString& Hint, TFunction<void()> Activate)
{
	FMenuRow& Row = Page.Rows.AddDefaulted_GetRef();
	Row.Kind = FMenuRow::EKind::Action;
	Row.Label = Label;
	Row.Hint = Hint;
	Row.Activate = MoveTemp(Activate);
}

void FMenuPages::AddBackRow(FMenuPage& Page)
{
	AddAction(Page, TEXT("Back"), TEXT("Return to the previous page."), [this]() { Controller.Back(); });
}

void FMenuPages::AddStepper(FMenuPage& Page, const FString& Label, const FString& Hint, TFunction<float()> Get, TFunction<void(float)> Set,
	float Minimum, float Maximum, float Step, int32 Decimals, const FString& Unit)
{
	FMenuRow& Row = Page.Rows.AddDefaulted_GetRef();
	Row.Kind = FMenuRow::EKind::Value;
	Row.Label = Label;
	Row.Hint = Hint;
	Row.bRepeatable = true;
	Row.GetValue = [Get, Decimals, Unit]() { return FormatNumber(Get(), Decimals, Unit); };
	Row.GetFraction = [Get, Minimum, Maximum]() { return FMath::Clamp((Get() - Minimum) / (Maximum - Minimum), 0.f, 1.f); };
	Row.Adjust = [Get, Set, Minimum, Maximum, Step](int32 Direction)
	{
		const float Stepped = FMath::GridSnap(Get() + Direction * Step, Step);
		Set(FMath::Clamp(Stepped, Minimum, Maximum));
	};
}

void FMenuPages::AddChoice(FMenuPage& Page, const FString& Label, const FString& Hint, TArray<FString> Choices, TFunction<int32()> Get,
	TFunction<void(int32)> Set)
{
	FMenuRow& Row = Page.Rows.AddDefaulted_GetRef();
	Row.Kind = FMenuRow::EKind::Value;
	Row.Label = Label;
	Row.Hint = Hint;
	Row.GetValue = [Choices, Get]() { return Choices.IsValidIndex(Get()) ? Choices[Get()] : FString(); };
	Row.Adjust = [Choices, Get, Set](int32 Direction)
	{
		const int32 Count = Choices.Num();
		Set(((Get() + Direction) % Count + Count) % Count);
	};
}

void FMenuPages::AddToggle(FMenuPage& Page, const FString& Label, const FString& Hint, const FString& OffText, const FString& OnText,
	TFunction<bool()> Get, TFunction<void(bool)> Set)
{
	FMenuRow& Row = Page.Rows.AddDefaulted_GetRef();
	Row.Kind = FMenuRow::EKind::Value;
	Row.Label = Label;
	Row.Hint = Hint;
	Row.GetValue = [Get, OffText, OnText]() { return Get() ? OnText : OffText; };
	Row.Adjust = [Get, Set](int32) { Set(!Get()); };
}

void FMenuPages::AddButtonAssign(FMenuPage& Page, const FString& Label, const FString& Hint, TFunction<int32()> Get, TFunction<void(int32)> Set)
{
	ButtonBindings.Add({Label, Get, Set});
	FMenuRow& Row = Page.Rows.AddDefaulted_GetRef();
	Row.Kind = FMenuRow::EKind::Assign;
	Row.Label = Label;
	Row.Hint = Hint + TEXT(" Confirm, then press the button on the wheel. Left clears it.");
	Row.GetValue = [Get]() { return FMenuController::DescribeButton(Get()); };
	Row.Activate = [this, Label, Set]()
	{
		Controller.BeginButtonCapture(FString::Printf(TEXT("Press the button for \"%s\""), *Label), [this, Label, Set](int32 ButtonIndex)
		{
			AssignButton(Label, Set, ButtonIndex);
		});
	};
	Row.Adjust = [this, Label, Set](int32 Direction)
	{
		if (Direction < 0)
		{
			Set(-1);
			ApplyWheelSettings();
			Controller.ShowMessage(FString::Printf(TEXT("%s cleared"), *Label));
		}
	};
}

void FMenuPages::AddWheelButtonAssign(FMenuPage& Page, const FString& Label, const FString& Hint, int32 UWheelInputSettings::* Member)
{
	AddButtonAssign(Page, Label, Hint, [Member]() { return GetWheelSettings()->*Member; }, [Member](int32 Index) { GetWheelSettings()->*Member = Index; });
}

void FMenuPages::AddAxisAssign(FMenuPage& Page, const FString& Label, const FString& Hint, int32 UWheelInputSettings::* AxisMember,
	bool UWheelInputSettings::* InvertMember)
{
	FMenuRow& Row = Page.Rows.AddDefaulted_GetRef();
	Row.Kind = FMenuRow::EKind::Assign;
	Row.Label = Label;
	Row.Hint = Hint + (InvertMember ? TEXT(" Confirm, then press the pedal down once; the direction is detected.") : TEXT(" Confirm, then turn the wheel a quarter turn."));
	Row.GetValue = [AxisMember]() { return FMenuController::DescribeAxis(GetWheelSettings()->*AxisMember); };
	Row.Activate = [this, Label, AxisMember, InvertMember]()
	{
		const FString Prompt = InvertMember ? FString::Printf(TEXT("Press the %s pedal"), *Label.ToLower()) : FString(TEXT("Turn the wheel a quarter turn"));
		Controller.BeginAxisCapture(Prompt, [this, Label, AxisMember, InvertMember](int32 AxisCode, bool bRestedHigh)
		{
			GetWheelSettings()->*AxisMember = AxisCode;
			if (InvertMember)
			{
				GetWheelSettings()->*InvertMember = bRestedHigh;
			}
			ApplyWheelSettings();
			Controller.ShowMessage(FString::Printf(TEXT("%s is now %s"), *Label, *FMenuController::DescribeAxis(AxisCode)));
		});
	};
}

void FMenuPages::AddWheelStepper(FMenuPage& Page, const FString& Label, const FString& Hint, float UWheelInputSettings::* Member,
	float Minimum, float Maximum, float Step, int32 Decimals, float DisplayScale, const FString& Unit)
{
	AddStepper(Page, Label, Hint,
		[Member, DisplayScale]() { return GetWheelSettings()->*Member * DisplayScale; },
		[this, Member, DisplayScale](float Value)
		{
			GetWheelSettings()->*Member = Value / DisplayScale;
			ApplyWheelSettings();
		},
		Minimum, Maximum, Step, Decimals, Unit);
}

void FMenuPages::AssignButton(const FString& Label, const TFunction<void(int32)>& Set, int32 ButtonIndex)
{
	FString TakenFrom;
	for (const FButtonBinding& Binding : ButtonBindings)
	{
		if (Binding.Label != Label && Binding.Get() == ButtonIndex)
		{
			Binding.Set(-1);
			TakenFrom = Binding.Label;
		}
	}
	Set(ButtonIndex);
	ApplyWheelSettings();
	const FString Moved = TakenFrom.IsEmpty() ? FString() : FString::Printf(TEXT(", taken from \"%s\""), *TakenFrom);
	Controller.ShowMessage(FString::Printf(TEXT("%s is now button %d%s"), *Label, ButtonIndex, *Moved));
}

void FMenuPages::ApplyWheelSettings()
{
	if (GEngine == nullptr)
	{
		return;
	}
	if (UWheelInputSubsystem* Wheel = GEngine->GetEngineSubsystem<UWheelInputSubsystem>())
	{
		Wheel->ApplySettings();
	}
}

void FMenuPages::SaveAll()
{
	FUserSettingsFile::SaveOverrides(GetWheelSettings(), UWheelInputSettings::GetUserEditableProperties());
	FUserSettingsFile::SaveOverrides(GetCarSettings(), UCarSettings::GetUserEditableProperties());
	FUserSettingsFile::SaveOverrides(GetPreferences(), UDrivingPreferences::GetUserEditableProperties());
}

// ---------------------------------------------------------------------------------------------------------------------
// Start and pause menu
// ---------------------------------------------------------------------------------------------------------------------

FMenuPage FMenuPages::BuildMainMenu()
{
	FMenuPage Page;
	Page.Title = TEXT("Start");
	Page.bShowGameTitle = true;
	AddAction(Page, TEXT("Drive"), TEXT("Take the car out on the road."), [this]() { Controller.PushPage(BuildRegionPage(false)); });
	AddAction(Page, TEXT("Free camera"), TEXT("Fly around the map without a car. Left Shift is 50 km/h."), [this]() { Controller.PushPage(BuildRegionPage(true)); });
	AddAction(Page, TEXT("Settings"), TEXT("Wheel, graphics and audio."), [this]() { Controller.PushPage(BuildSettingsPage()); });
	AddAction(Page, TEXT("Quit"), TEXT("Close the game."), [this]() { Host.QuitGame(); });
	return Page;
}

FMenuPage FMenuPages::BuildPauseMenu()
{
	FMenuPage Page;
	Page.Title = TEXT("Paused");
	AddAction(Page, TEXT("Resume"), TEXT("Back to the road."), [this]() { Host.Resume(); });
	if (Host.HasCar())
	{
		AddAction(Page, TEXT("Reset car"), TEXT("Put the car back upright on the road where it stands."), [this]() { Host.ResetCar(); });
	}
	AddAction(Page, TEXT("Settings"), TEXT("Wheel, graphics and audio."), [this]() { Controller.PushPage(BuildSettingsPage()); });
	AddAction(Page, TEXT("Main menu"), TEXT("Leave this drive and go back to the start."), [this]() { Host.ReturnToMainMenu(); });
	AddAction(Page, TEXT("Quit"), TEXT("Close the game."), [this]() { Host.QuitGame(); });
	return Page;
}

FMenuPage FMenuPages::BuildRegionPage(bool bFreeCamera)
{
	FMenuPage Page;
	Page.Title = bFreeCamera ? TEXT("Free camera: choose a region") : TEXT("Drive: choose a region");
	AddAction(Page, TEXT("Hamburg"), TEXT("The whole city, starting in Bergedorf. Traffic takes about half a minute to load."),
		[this, bFreeCamera]() { Host.StartDrive(RegionHamburg, bFreeCamera); });
	AddAction(Page, TEXT("Bergedorf"), TEXT("The town centre and its surroundings, 2 by 2 km. Takes a little longer to load."),
		[this, bFreeCamera]() { Host.StartDrive(RegionBergedorfCore, bFreeCamera); });
	AddAction(Page, TEXT("Bergedorf test area"), TEXT("A 500 m patch of the town. Loads quickly."),
		[this, bFreeCamera]() { Host.StartDrive(RegionBergedorfTest, bFreeCamera); });
	AddBackRow(Page);
	return Page;
}

FMenuPage FMenuPages::BuildSettingsPage()
{
	FMenuPage Page;
	Page.Title = TEXT("Settings");
	Page.OnLeave = [this]() { SaveAll(); };
	AddAction(Page, TEXT("Wheel"), TEXT("Axes, pedals, shifter, buttons and force feedback. Shows what the wheel reports."), [this]() { Controller.PushPage(BuildWheelPage()); });
	AddAction(Page, TEXT("Graphics"), TEXT("Resolution scale and how far the world is drawn."), [this]() { Controller.PushPage(BuildGraphicsPage()); });
	AddAction(Page, TEXT("Audio"), TEXT("Volume of the engine and the surroundings."), [this]() { Controller.PushPage(BuildAudioPage()); });
	AddBackRow(Page);
	return Page;
}

// ---------------------------------------------------------------------------------------------------------------------
// Wheel
// ---------------------------------------------------------------------------------------------------------------------

FMenuPage FMenuPages::BuildWheelPage()
{
	ButtonBindings.Reset();
	FMenuPage Page;
	Page.Title = TEXT("Wheel");
	Page.bShowWheelReadout = true;
	Page.OnLeave = [this]() { SaveAll(); };
	AddWheelSteeringRows(Page);
	AddWheelPedalRows(Page);
	AddWheelShifterRows(Page);
	AddWheelButtonRows(Page);
	AddWheelForceRows(Page);
	AddWheelFooterRows(Page);
	return Page;
}

void FMenuPages::AddWheelSteeringRows(FMenuPage& Page)
{
	AddHeader(Page, TEXT("Steering"));
	AddStepper(Page, TEXT("Wheel range"), TEXT("Lock-to-lock rotation. The G923 does up to 900 degrees; lower values make the car turn in faster."),
		[]() { return static_cast<float>(GetWheelSettings()->WheelRangeDegrees); },
		[this](float Value)
		{
			GetWheelSettings()->WheelRangeDegrees = FMath::RoundToInt(Value);
			ApplyWheelSettings();
		},
		180.f, 900.f, 10.f, 0, TEXT("deg"));
	AddAxisAssign(Page, TEXT("Steering axis"), TEXT("The axis that reports the wheel angle."), &UWheelInputSettings::SteeringAxis, nullptr);
	AddToggle(Page, TEXT("Steering direction"), TEXT("Flip this if turning the wheel to the right steers the car to the left."), TEXT("Normal"), TEXT("Reversed"),
		[]() { return GetWheelSettings()->bInvertSteering; },
		[this](bool bValue)
		{
			GetWheelSettings()->bInvertSteering = bValue;
			ApplyWheelSettings();
		});
}

void FMenuPages::AddWheelPedalRows(FMenuPage& Page)
{
	AddHeader(Page, TEXT("Pedals"));
	const auto AddPedal = [this, &Page](const FString& Name, int32 UWheelInputSettings::* AxisMember, bool UWheelInputSettings::* InvertMember)
	{
		AddAxisAssign(Page, Name, FString::Printf(TEXT("The axis of the %s pedal."), *Name.ToLower()), AxisMember, InvertMember);
		AddToggle(Page, Name + TEXT(" signal"), TEXT("Logitech pedals report their maximum when released, which is the inverted setting."),
			TEXT("Normal"), TEXT("Inverted"),
			[InvertMember]() { return GetWheelSettings()->*InvertMember; },
			[this, InvertMember](bool bValue)
			{
				GetWheelSettings()->*InvertMember = bValue;
				ApplyWheelSettings();
			});
	};
	AddPedal(TEXT("Throttle"), &UWheelInputSettings::ThrottleAxis, &UWheelInputSettings::bInvertThrottle);
	AddPedal(TEXT("Brake"), &UWheelInputSettings::BrakeAxis, &UWheelInputSettings::bInvertBrake);
	AddPedal(TEXT("Clutch"), &UWheelInputSettings::ClutchAxis, &UWheelInputSettings::bInvertClutch);
	AddWheelStepper(Page, TEXT("Pedal deadzone"), TEXT("Travel at the released end that is ignored, so a resting foot does not count."),
		&UWheelInputSettings::PedalDeadzone, 0.f, 30.f, 1.f, 0, 100.f, TEXT("%"));
	AddWheelStepper(Page, TEXT("Brake full travel"), TEXT("How far the brake pedal has to go to count as fully pressed. The G923 brake is stiff at the end, so about 75 % works well."),
		&UWheelInputSettings::BrakeFullTravel, 30.f, 100.f, 5.f, 0, 100.f, TEXT("%"));
}

void FMenuPages::AddWheelShifterRows(FMenuPage& Page)
{
	AddHeader(Page, TEXT("Shifter"));
	for (int32 GearIndex = 0; GearIndex < 6; ++GearIndex)
	{
		AddButtonAssign(Page, FString::Printf(TEXT("Gear %d"), GearIndex + 1), TEXT("The button the shifter presses in this gear."),
			[GearIndex]()
			{
				const TArray<int32>& Indices = GetWheelSettings()->GearButtonIndices;
				return Indices.IsValidIndex(GearIndex) ? Indices[GearIndex] : -1;
			},
			[GearIndex](int32 ButtonIndex)
			{
				TArray<int32>& Indices = GetWheelSettings()->GearButtonIndices;
				while (Indices.Num() <= GearIndex)
				{
					Indices.Add(-1);
				}
				Indices[GearIndex] = ButtonIndex;
			});
	}
	AddWheelButtonAssign(Page, TEXT("Reverse"), TEXT("The button the shifter presses in reverse."), &UWheelInputSettings::ReverseButtonIndex);
}

void FMenuPages::AddWheelButtonRows(FMenuPage& Page)
{
	AddHeader(Page, TEXT("Buttons"));
	AddWheelButtonAssign(Page, TEXT("Start engine"), TEXT("Starts and stops the engine."), &UWheelInputSettings::StartEngineButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Parking brake"), TEXT("Pulls and releases the parking brake."), &UWheelInputSettings::HandbrakeButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Recentre view"), TEXT("Puts the view back where the driver looks straight ahead."), &UWheelInputSettings::RecenterViewButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Reset car"), TEXT("Puts the car back upright on the road."), &UWheelInputSettings::ResetCarButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Low beam"), TEXT("Switches the low beam and the tail lights on and off."), &UWheelInputSettings::LowBeamButtonIndex);
	AddWheelButtonAssign(Page, TEXT("High beam"), TEXT("Switches the high beam on and off."), &UWheelInputSettings::HighBeamButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Indicator left"), TEXT("Starts and stops the left indicator. It cancels itself after the turn."), &UWheelInputSettings::IndicatorLeftButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Indicator right"), TEXT("Starts and stops the right indicator. It cancels itself after the turn."), &UWheelInputSettings::IndicatorRightButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Hazard lights"), TEXT("Switches the hazard warning lights on and off."), &UWheelInputSettings::HazardButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Radio on and off"), TEXT("Switches the car radio on and off."), &UWheelInputSettings::RadioToggleButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Next station"), TEXT("Tunes the next radio station."), &UWheelInputSettings::RadioNextButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Previous station"), TEXT("Tunes the previous radio station."), &UWheelInputSettings::RadioPreviousButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Minimap zoom in"), TEXT("Shows less of the minimap, in more detail. The + key does the same."), &UWheelInputSettings::MinimapZoomInButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Minimap zoom out"), TEXT("Shows more of the minimap. The - key does the same."), &UWheelInputSettings::MinimapZoomOutButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Open menu"), TEXT("Pauses the game and opens the menu. Holding the D-pad up for a second does the same."), &UWheelInputSettings::MenuButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Menu confirm"), TEXT("Chooses the selected entry in the menu. D-pad right does this without a button."), &UWheelInputSettings::MenuConfirmButtonIndex);
	AddWheelButtonAssign(Page, TEXT("Menu back"), TEXT("Goes back one page. D-pad left on an entry does this without a button."), &UWheelInputSettings::MenuBackButtonIndex);
}

void FMenuPages::AddWheelForceRows(FMenuPage& Page)
{
	AddHeader(Page, TEXT("Force feedback"));
	AddToggle(Page, TEXT("Force direction"), TEXT("Flip this if the wheel pulls away from the centre instead of back to it."), TEXT("Normal"), TEXT("Reversed"),
		[]() { return !GetWheelSettings()->bInvertForce; },
		[this](bool bNormal)
		{
			GetWheelSettings()->bInvertForce = !bNormal;
			ApplyWheelSettings();
		});
	AddWheelStepper(Page, TEXT("Force strength"), TEXT("Scales everything the wheel motor does."), &UWheelInputSettings::ForceGain, 0.f, 100.f, 5.f, 0, 100.f, TEXT("%"));
	AddStepper(Page, TEXT("Full-scale torque"), TEXT("The torque at the driver's hands that uses the whole motor. Lower values make the wheel heavier."),
		[]() { return GetCarSettings()->FfbFullScaleNm; },
		[](float Value) { GetCarSettings()->FfbFullScaleNm = Value; },
		2.f, 12.f, 0.5f, 1, TEXT("Nm"));
	AddStepper(Page, TEXT("Force limit"), TEXT("The most force the wheel is ever asked for. Lower it if the wheel feels violent."),
		[]() { return GetCarSettings()->FfbMaxForce * 100.f; },
		[](float Value) { GetCarSettings()->FfbMaxForce = Value / 100.f; },
		20.f, 100.f, 5.f, 0, TEXT("%"));
}

void FMenuPages::AddWheelFooterRows(FMenuPage& Page)
{
	AddHeader(Page, TEXT(""));
	AddAction(Page, TEXT("Reset wheel settings"), TEXT("Back to the values from the game's configuration. Confirm twice."), [this]() { ResetWheelSettings(); });
	AddBackRow(Page);
}

void FMenuPages::ResetWheelSettings()
{
	const double Now = FPlatformTime::Seconds();
	if (Now > ResetArmedUntilSeconds)
	{
		ResetArmedUntilSeconds = Now + ResetConfirmSeconds;
		Controller.ShowMessage(TEXT("Confirm again to reset all wheel and force feedback settings"));
		return;
	}
	ResetArmedUntilSeconds = 0.0;
	FUserSettingsFile::ResetToDefaults(GetWheelSettings(), UWheelInputSettings::GetUserEditableProperties());
	FUserSettingsFile::ResetToDefaults(GetCarSettings(), UCarSettings::GetUserEditableProperties());
	ApplyWheelSettings();
	Controller.ShowMessage(TEXT("Wheel settings reset"));
}

// ---------------------------------------------------------------------------------------------------------------------
// Graphics and audio
// ---------------------------------------------------------------------------------------------------------------------

int32 FMenuPages::FindMatchingPreset() const
{
	for (int32 Index = 0; Index < PresetNames.Num(); ++Index)
	{
		if (Host.GetEffectiveScreenPercentage() == PresetScreenPercentages[Index] && Host.GetEffectiveViewDistanceMeters() == PresetViewDistances[Index])
		{
			return Index;
		}
	}
	return CustomPresetIndex;
}

void FMenuPages::ApplyPreset(const FGraphicsPreset& Preset)
{
	UDrivingPreferences* Preferences = GetPreferences();
	Preferences->ScreenPercentage = Preset.ScreenPercentage;
	Preferences->ViewDistanceMeters = Preset.ViewDistanceMeters;
	Host.ApplyPreferences();
}

FMenuPage FMenuPages::BuildGraphicsPage()
{
	FMenuPage Page;
	Page.Title = TEXT("Graphics");
	Page.OnLeave = [this]() { SaveAll(); };

	TArray<FString> PresetChoices = PresetNames;
	PresetChoices.Add(TEXT("Custom"));
	AddChoice(Page, TEXT("Preset"), TEXT("Performance, balanced or quality: sets the resolution scale and the view distance together."), PresetChoices,
		[this]() { return FindMatchingPreset(); },
		[this](int32 Index)
		{
			if (Index < PresetNames.Num())
			{
				ApplyPreset({*PresetNames[Index], PresetScreenPercentages[Index], PresetViewDistances[Index]});
			}
		});
	AddStepper(Page, TEXT("Resolution scale"), TEXT("How many pixels are rendered before the image is upscaled. Lower is faster, higher is sharper."),
		[this]() { return static_cast<float>(Host.GetEffectiveScreenPercentage()); },
		[this](float Value)
		{
			GetPreferences()->ScreenPercentage = FMath::RoundToInt(Value);
			Host.ApplyPreferences();
		},
		50.f, 100.f, 5.f, 0, TEXT("%"));
	AddStepper(Page, TEXT("View distance"), TEXT("How far out the world is generated. Houses and trees far away are simplified; further distances cost frame time."),
		[this]() { return static_cast<float>(Host.GetEffectiveViewDistanceMeters()); },
		[this](float Value)
		{
			GetPreferences()->ViewDistanceMeters = FMath::RoundToInt(Value);
			Host.ApplyPreferences();
		},
		500.f, 8000.f, 250.f, 0, TEXT("m"));
	AddBackRow(Page);
	return Page;
}

FMenuPage FMenuPages::BuildAudioPage()
{
	FMenuPage Page;
	Page.Title = TEXT("Audio");
	Page.OnLeave = [this]() { SaveAll(); };

	const auto AddVolume = [this, &Page](const FString& Label, const FString& Hint, float UDrivingPreferences::* Member)
	{
		AddStepper(Page, Label, Hint,
			[Member]() { return GetPreferences()->*Member * 100.f; },
			[this, Member](float Value)
			{
				GetPreferences()->*Member = Value / 100.f;
				Host.ApplyPreferences();
			},
			0.f, 100.f, 5.f, 0, TEXT("%"));
	};
	AddVolume(TEXT("Master volume"), TEXT("Everything you hear."), &UDrivingPreferences::MasterVolume);
	AddVolume(TEXT("Engine"), TEXT("Engine, gearbox and tyres, relative to the master volume."), &UDrivingPreferences::EngineVolume);
	AddVolume(TEXT("Ambience"), TEXT("Wind, rain, birds and traffic, relative to the master volume."), &UDrivingPreferences::AmbienceVolume);
	AddVolume(TEXT("Radio"), TEXT("The car radio, relative to the master volume."), &UDrivingPreferences::RadioVolume);
	AddBackRow(Page);
	return Page;
}
