#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DrivingPreferences.generated.h"

DECLARE_MULTICAST_DELEGATE(FOnDrivingPreferencesChanged);

/**
 * Player preferences that the in-game settings menu edits: graphics and audio. The menu saves them as per-user
 * overrides (FUserSettingsFile), so Config/DefaultGame.ini only holds the defaults. A value of 0 means "not set by the
 * player": the command line and the engine's own defaults apply (for example the screen percentage from run_vr.sh).
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Preferences"))
class DRIVINGGAME_API UDrivingPreferences : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Names of the properties saved as per-user overrides. */
	static const TArray<FName>& GetUserEditableProperties();

	/** Fires after the menu changed a value, so systems that depend on a preference can re-read it (audio, for example). */
	static FOnDrivingPreferencesChanged& OnChanged();

	/** Resolution scale of the 3D view in percent (r.ScreenPercentage). 0 = leave as launched. */
	UPROPERTY(Config, EditAnywhere, Category = "Graphics", meta = (ClampMin = "0", ClampMax = "100"))
	int32 ScreenPercentage = 0;

	/** How far the world is generated around the car, in metres (the far detail ring of the world streamer). 0 = streamer default. */
	UPROPERTY(Config, EditAnywhere, Category = "Graphics", meta = (ClampMin = "0"))
	int32 ViewDistanceMeters = 0;

	/** Overall volume, 0..1. Applied to the whole game. */
	UPROPERTY(Config, EditAnywhere, Category = "Audio", meta = (ClampMin = "0", ClampMax = "1"))
	float MasterVolume = 1.f;

	/** Engine, gearbox and tyre sounds relative to the master volume, 0..1. For the sound system to read. */
	UPROPERTY(Config, EditAnywhere, Category = "Audio", meta = (ClampMin = "0", ClampMax = "1"))
	float EngineVolume = 1.f;

	/** Wind, rain, birds and traffic relative to the master volume, 0..1. For the sound system to read. */
	UPROPERTY(Config, EditAnywhere, Category = "Audio", meta = (ClampMin = "0", ClampMax = "1"))
	float AmbienceVolume = 1.f;

	/** Car radio relative to the master volume, 0..1. */
	UPROPERTY(Config, EditAnywhere, Category = "Audio", meta = (ClampMin = "0", ClampMax = "1"))
	float RadioVolume = 0.6f;

	/** Whether the radio is switched on. Kept between sessions; it still stays silent while the ignition is off. */
	UPROPERTY(Config, EditAnywhere, Category = "Audio")
	bool bRadioOn = false;

	/** Index of the tuned station in URadioSettings::Stations. Kept between sessions. */
	UPROPERTY(Config, EditAnywhere, Category = "Audio", meta = (ClampMin = "0"))
	int32 RadioStationIndex = 0;
};
