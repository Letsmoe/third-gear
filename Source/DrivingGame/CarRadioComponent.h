#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RadioStream.h"
#include "CarRadioComponent.generated.h"

class ACarPawn;
class SRadioOverlay;
class URadioSynthComponent;

/**
 * The car radio on the player car: live Hamburg stations (URadioSettings) played through the cabin speakers, with the
 * station and the current song shown in the top right corner of the screen on the desktop. It plays while it is switched
 * on and the ignition is on, and gives the connection up a few seconds after either ends. The station and the on state are
 * kept in UDrivingPreferences; `-RadioStation=<n>` switches it on at station n for a test without saving anything.
 */
UCLASS(ClassGroup = "Audio", meta = (BlueprintSpawnableComponent))
class DRIVINGGAME_API UCarRadioComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCarRadioComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Switches the radio on or off. */
	void ToggleRadio();

	/** Tunes the next station (wraps around) and switches the radio on. */
	void NextStation();

	/** Tunes the previous station (wraps around) and switches the radio on. */
	void PreviousStation();

	bool IsRadioOn() const { return bRadioOn; }
	int32 GetStationIndex() const { return StationIndex; }

private:
	/** Tunes the station at Index (wrapped into the list) and switches the radio on. */
	void TuneTo(int32 Index);
	/** Stores the on state and station in the per-user preferences, unless a test switch set them. */
	void SavePreferences();
	/** Applies the radio volume from the preferences to the speakers. */
	void ApplyPreferences();

	ACarPawn* GetCar() const;
	bool IsPlayerCar() const;
	/** Whether sound can come out at all: there is an audio device (the screenshot runs have none). */
	bool CanPlay() const;
	/** The radio is switched on and the ignition is on. */
	bool IsPowered() const;

	void CreateSpeakers();
	void StartStream();
	void StopStream();
	/** Starts, stops and fades the stream to follow the power state. */
	void UpdatePower(float DeltaTime);

	void CreateOverlay();
	void RemoveOverlay();
	/** Reads new song titles from the stream and refreshes the overlay. */
	void UpdateOverlay(float DeltaTime);
	/** Station name for the first line and song for the second, empty when the stream names only the station. */
	void BuildOverlayTexts(FString& OutStation, FString& OutSong) const;
	bool ShouldShowOverlay() const;

	UPROPERTY(Transient)
	TObjectPtr<URadioSynthComponent> Speakers;

	TSharedPtr<FRadioStreamer, ESPMode::ThreadSafe> Streamer;
	TSharedPtr<SRadioOverlay> Overlay;
	FDelegateHandle PreferencesHandle;

	bool bRadioOn = false;
	int32 StationIndex = 0;
	bool bForcedByTest = false;
	FString SongTitle;
	float IdleSeconds = 0.f;
	float StatusFlashSecondsLeft = 0.f;
	bool bOverlayAdded = false;
};
