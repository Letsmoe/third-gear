#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "RadioSettings.generated.h"

/** One radio station: the name shown on screen and the address of its public internet stream. */
USTRUCT()
struct FRadioStation
{
	GENERATED_BODY()

	UPROPERTY(Config, EditAnywhere, Category = "Radio")
	FString Name;

	/** MP3 or AAC over HTTP(S). The stream should carry ICY metadata (the current song) but does not have to. */
	UPROPERTY(Config, EditAnywhere, Category = "Radio")
	FString Url;
};

/**
 * The car radio's stations and tools, set in Config/DefaultGame.ini under [/Script/DrivingGame.RadioSettings].
 * The stations are listed in the order the next and previous station keys step through them.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Radio"))
class DRIVINGGAME_API URadioSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** The stations to tune, in order. */
	UPROPERTY(Config, EditAnywhere, Category = "Radio")
	TArray<FRadioStation> Stations;

	/** Decodes the streams. A runtime dependency: it must be installed (a name without a slash is looked up in the PATH). */
	UPROPERTY(Config, EditAnywhere, Category = "Radio")
	FString FfmpegPath = TEXT("ffmpeg");

	/** Reads the song titles that ride along in the stream. A runtime dependency like ffmpeg. */
	UPROPERTY(Config, EditAnywhere, Category = "Radio")
	FString CurlPath = TEXT("curl");

	/** Audio collected before playback starts, so a slow network does not make it stutter. */
	UPROPERTY(Config, EditAnywhere, Category = "Radio", meta = (ClampMin = "0.2", ClampMax = "5"))
	float PrebufferSeconds = 1.f;
};
