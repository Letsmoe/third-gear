#include "CarRadioComponent.h"

#include "CarPawn.h"
#include "DrivingPreferences.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RadioSettings.h"
#include "RadioSynthComponent.h"
#include "SRadioOverlay.h"
#include "UserSettingsFile.h"
#include "Widgets/SWeakWidget.h"

namespace
{
constexpr float IdleSecondsBeforeDisconnect = 8.f;
constexpr float StatusFlashSeconds = 1.8f;
constexpr int32 OverlayZOrder = 50; // below the menu (100)

/** "Artist - Title" with a proper dash. */
FString TidyTitle(const FString& Title)
{
	return Title.Replace(TEXT(" - "), TEXT(" \u2013 "));
}
}

UCarRadioComponent::UCarRadioComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

ACarPawn* UCarRadioComponent::GetCar() const
{
	return Cast<ACarPawn>(GetOwner());
}

bool UCarRadioComponent::IsPlayerCar() const
{
	const ACarPawn* Car = GetCar();
	return Car && Car->IsPlayerControlled();
}

bool UCarRadioComponent::CanPlay() const
{
	return FApp::CanEverRenderAudio() && GEngine && GEngine->GetMainAudioDevice().IsValid();
}

bool UCarRadioComponent::IsPowered() const
{
	const ACarPawn* Car = GetCar();
	return bRadioOn && Car && Car->IsIgnitionOn();
}

void UCarRadioComponent::BeginPlay()
{
	Super::BeginPlay();
	const UDrivingPreferences* Preferences = GetDefault<UDrivingPreferences>();
	bRadioOn = Preferences->bRadioOn;
	StationIndex = Preferences->RadioStationIndex;
	int32 ForcedStation = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("RadioStation="), ForcedStation))
	{
		bForcedByTest = true;
		bRadioOn = true;
		StationIndex = ForcedStation;
	}
	const int32 StationCount = GetDefault<URadioSettings>()->Stations.Num();
	StationIndex = StationCount > 0 ? FMath::Clamp(StationIndex, 0, StationCount - 1) : 0;
	PreferencesHandle = UDrivingPreferences::OnChanged().AddUObject(this, &UCarRadioComponent::ApplyPreferences);
}

void UCarRadioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UDrivingPreferences::OnChanged().Remove(PreferencesHandle);
	StopStream();
	RemoveOverlay();
	Super::EndPlay(EndPlayReason);
}

// --- controls ------------------------------------------------------------------------------------------------------

void UCarRadioComponent::ToggleRadio()
{
	bRadioOn = !bRadioOn;
	StatusFlashSecondsLeft = StatusFlashSeconds;
	UE_LOG(LogRadio, Log, TEXT("Radio %s"), bRadioOn ? TEXT("on") : TEXT("off"));
	SavePreferences();
}

void UCarRadioComponent::NextStation()
{
	TuneTo(StationIndex + 1);
}

void UCarRadioComponent::PreviousStation()
{
	TuneTo(StationIndex - 1);
}

void UCarRadioComponent::TuneTo(int32 Index)
{
	const TArray<FRadioStation>& Stations = GetDefault<URadioSettings>()->Stations;
	if (Stations.IsEmpty())
	{
		UE_LOG(LogRadio, Warning, TEXT("No radio stations configured (RadioSettings in DefaultGame.ini)"));
		return;
	}
	StationIndex = (Index % Stations.Num() + Stations.Num()) % Stations.Num();
	bRadioOn = true;
	SongTitle.Reset();
	StopStream();
	UE_LOG(LogRadio, Log, TEXT("Tuning %s"), *Stations[StationIndex].Name);
	SavePreferences();
}

void UCarRadioComponent::SavePreferences()
{
	if (bForcedByTest)
	{
		return;
	}
	UDrivingPreferences* Preferences = GetMutableDefault<UDrivingPreferences>();
	Preferences->bRadioOn = bRadioOn;
	Preferences->RadioStationIndex = StationIndex;
	FUserSettingsFile::SaveOverrides(Preferences, UDrivingPreferences::GetUserEditableProperties());
}

void UCarRadioComponent::ApplyPreferences()
{
	if (Speakers)
	{
		Speakers->SetVolumeMultiplier(FMath::Clamp(GetDefault<UDrivingPreferences>()->RadioVolume, 0.f, 1.f));
	}
}

// --- stream and speakers ---------------------------------------------------------------------------------------------

void UCarRadioComponent::CreateSpeakers()
{
	AActor* Owner = GetOwner();
	Speakers = NewObject<URadioSynthComponent>(Owner, TEXT("RadioSpeakers"));
	Speakers->SetupAttachment(Owner->GetRootComponent());
	Speakers->RegisterComponent();
	ApplyPreferences();
	Speakers->Start();
}

void UCarRadioComponent::StartStream()
{
	const TArray<FRadioStation>& Stations = GetDefault<URadioSettings>()->Stations;
	if (!Stations.IsValidIndex(StationIndex))
	{
		return;
	}
	if (!Speakers)
	{
		CreateSpeakers();
	}
	const FRadioStation& Station = Stations[StationIndex];
	UE_LOG(LogRadio, Log, TEXT("Connecting to %s (%s)"), *Station.Name, *Station.Url);
	Streamer = FRadioStreamer::Start(Station.Name, Station.Url);
	Speakers->GetPlayback()->SetStreamer(Streamer);
}

void UCarRadioComponent::StopStream()
{
	if (Speakers)
	{
		Speakers->GetPlayback()->SetStreamer(nullptr);
	}
	if (Streamer.IsValid())
	{
		Streamer->RequestStop();
		Streamer.Reset();
	}
	SongTitle.Reset();
}

void UCarRadioComponent::UpdatePower(float DeltaTime)
{
	const bool bPowered = IsPowered();
	if (bPowered && !Streamer.IsValid())
	{
		StartStream();
	}
	if (Speakers)
	{
		Speakers->GetPlayback()->SetTargetGain(bPowered ? 1.f : 0.f);
	}
	IdleSeconds = bPowered ? 0.f : IdleSeconds + DeltaTime;
	if (!bPowered && Streamer.IsValid() && IdleSeconds > IdleSecondsBeforeDisconnect)
	{
		UE_LOG(LogRadio, Log, TEXT("Radio idle, disconnecting"));
		StopStream();
	}
}

// --- overlay -------------------------------------------------------------------------------------------------------

void UCarRadioComponent::CreateOverlay()
{
	UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport : nullptr;
	if (!Viewport || !FSlateApplication::IsInitialized() || UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled())
	{
		return;
	}
	Overlay = SNew(SRadioOverlay);
	Viewport->AddViewportWidgetContent(Overlay.ToSharedRef(), OverlayZOrder);
	bOverlayAdded = true;
}

void UCarRadioComponent::RemoveOverlay()
{
	UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport : nullptr;
	if (bOverlayAdded && Viewport && Overlay.IsValid())
	{
		Viewport->RemoveViewportWidgetContent(Overlay.ToSharedRef());
	}
	Overlay.Reset();
	bOverlayAdded = false;
}

bool UCarRadioComponent::ShouldShowOverlay() const
{
	return IsPowered() || StatusFlashSecondsLeft > 0.f;
}

void UCarRadioComponent::BuildOverlayTexts(FString& OutStation, FString& OutSong) const
{
	const TArray<FRadioStation>& Stations = GetDefault<URadioSettings>()->Stations;
	const FString StationName = Stations.IsValidIndex(StationIndex) ? Stations[StationIndex].Name : FString();
	OutStation = bRadioOn ? StationName : TEXT("Radio off");
	OutSong.Reset();
	if (!bRadioOn)
	{
		return;
	}
	if (!Streamer.IsValid() || !Streamer->IsConnected())
	{
		OutSong = TEXT("Connecting...");
		return;
	}
	const bool bJustTheStationName = SongTitle.Contains(StationName, ESearchCase::IgnoreCase);
	OutSong = bJustTheStationName ? FString() : TidyTitle(SongTitle);
}

void UCarRadioComponent::UpdateOverlay(float DeltaTime)
{
	StatusFlashSecondsLeft = FMath::Max(0.f, StatusFlashSecondsLeft - DeltaTime);
	FString NewTitle;
	if (Streamer.IsValid() && Streamer->ConsumeTitle(NewTitle))
	{
		SongTitle = NewTitle;
	}
	if (!Overlay.IsValid())
	{
		CreateOverlay();
	}
	if (Overlay.IsValid())
	{
		FString Station;
		FString Song;
		BuildOverlayTexts(Station, Song);
		Overlay->SetContent(ShouldShowOverlay(), Station, Song);
	}
}

// --- tick ----------------------------------------------------------------------------------------------------------

void UCarRadioComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!IsPlayerCar() || DeltaTime <= 0.f)
	{
		return;
	}
	if (CanPlay())
	{
		UpdatePower(DeltaTime);
	}
	UpdateOverlay(DeltaTime);
}
