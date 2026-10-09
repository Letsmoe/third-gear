#pragma once

#include "CoreMinimal.h"
#include "MenuTypes.h"

class FMenuController;
class UWheelInputSettings;

/** What the menu pages ask the game to do; implemented by UGameMenuSubsystem. */
class IGameMenuHost
{
public:
	virtual ~IGameMenuHost() = default;

	/** Loads the region and starts driving in the car, or flying the free camera. */
	virtual void StartDrive(const FString& Region, bool bFreeCamera) = 0;

	/** Closes the pause menu and continues the game. */
	virtual void Resume() = 0;

	/** Puts the car back upright on the road where it stands. */
	virtual void ResetCar() = 0;

	/** Leaves the current drive and shows the start menu. */
	virtual void ReturnToMainMenu() = 0;

	virtual void QuitGame() = 0;

	/** Applies the graphics and audio preferences to the running game and tells the systems that listen to them. */
	virtual void ApplyPreferences() = 0;

	/** True if the player is sitting in a car, so resetting it makes sense. */
	virtual bool HasCar() const = 0;

	/** Screen percentage in effect: the player's choice, else what the game was launched with. */
	virtual int32 GetEffectiveScreenPercentage() const = 0;

	/** Far streaming distance in effect, metres. */
	virtual int32 GetEffectiveViewDistanceMeters() const = 0;
};

/** Builds the pages of the start menu, the pause menu and the settings. Must outlive the pages it builds. */
class FMenuPages
{
public:
	FMenuPages(FMenuController& InController, IGameMenuHost& InHost);

	FMenuPage BuildMainMenu();
	FMenuPage BuildPauseMenu();

	/** Region choice that follows "Drive" or "Free camera". */
	FMenuPage BuildRegionPage(bool bFreeCamera);

	FMenuPage BuildSettingsPage();
	FMenuPage BuildWheelPage();
	FMenuPage BuildGraphicsPage();
	FMenuPage BuildAudioPage();

	/** Writes the wheel, force feedback and preference values that differ from the defaults to the user settings file. */
	void SaveAll();

private:
	/** A wheel-button row, remembered so that assigning a button frees it from its previous function. */
	struct FButtonBinding
	{
		FString Label;
		TFunction<int32()> Get;
		TFunction<void(int32)> Set;
	};

	struct FGraphicsPreset
	{
		const TCHAR* Name;
		int32 ScreenPercentage;
		int32 ViewDistanceMeters;
	};

	void AddWheelSteeringRows(FMenuPage& Page);
	void AddWheelPedalRows(FMenuPage& Page);
	void AddWheelShifterRows(FMenuPage& Page);
	void AddWheelButtonRows(FMenuPage& Page);
	void AddWheelForceRows(FMenuPage& Page);
	void AddWheelFooterRows(FMenuPage& Page);

	void AddHeader(FMenuPage& Page, const FString& Label);
	void AddBackRow(FMenuPage& Page);
	void AddAction(FMenuPage& Page, const FString& Label, const FString& Hint, TFunction<void()> Activate);
	void AddStepper(FMenuPage& Page, const FString& Label, const FString& Hint, TFunction<float()> Get, TFunction<void(float)> Set,
		float Minimum, float Maximum, float Step, int32 Decimals, const FString& Unit);
	void AddChoice(FMenuPage& Page, const FString& Label, const FString& Hint, TArray<FString> Choices, TFunction<int32()> Get, TFunction<void(int32)> Set);
	void AddToggle(FMenuPage& Page, const FString& Label, const FString& Hint, const FString& OffText, const FString& OnText,
		TFunction<bool()> Get, TFunction<void(bool)> Set);
	void AddButtonAssign(FMenuPage& Page, const FString& Label, const FString& Hint, TFunction<int32()> Get, TFunction<void(int32)> Set);
	void AddAxisAssign(FMenuPage& Page, const FString& Label, const FString& Hint, int32 UWheelInputSettings::* AxisMember,
		bool UWheelInputSettings::* InvertMember);
	void AddWheelStepper(FMenuPage& Page, const FString& Label, const FString& Hint, float UWheelInputSettings::* Member,
		float Minimum, float Maximum, float Step, int32 Decimals, float DisplayScale, const FString& Unit);
	void AddWheelButtonAssign(FMenuPage& Page, const FString& Label, const FString& Hint, int32 UWheelInputSettings::* Member);

	void AssignButton(const FString& Label, const TFunction<void(int32)>& Set, int32 ButtonIndex);
	void ApplyWheelSettings();
	void ResetWheelSettings();
	int32 FindMatchingPreset() const;
	void ApplyPreset(const FGraphicsPreset& Preset);

	FMenuController& Controller;
	IGameMenuHost& Host;
	TArray<FButtonBinding> ButtonBindings;
	double ResetArmedUntilSeconds = 0.0;
};
