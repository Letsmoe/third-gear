#pragma once

#include "CoreMinimal.h"
#include "Components/SynthComponent.h"
#include "RadioStream.h"
#include <atomic>
#include "RadioSynthComponent.generated.h"

/** What the audio thread plays: the current stream (swapped from the game thread) and the gain it fades to. */
class FRadioPlayback
{
public:
	void SetStreamer(const TSharedPtr<FRadioStreamer, ESPMode::ThreadSafe>& InStreamer);
	TSharedPtr<FRadioStreamer, ESPMode::ThreadSafe> GetStreamer();
	void SetTargetGain(float Gain) { TargetGain = Gain; }
	float GetTargetGain() const { return TargetGain; }

private:
	FCriticalSection Lock;
	TSharedPtr<FRadioStreamer, ESPMode::ThreadSafe> Streamer;
	std::atomic<float> TargetGain{0.f};
};

/**
 * Plays the car radio's stream through the audio mixer as the sound inside the cabin: not positioned in the world, and
 * band-limited like small car speakers (no deep bass, no air above 12 kHz). The game thread picks the stream and the gain,
 * the audio thread pulls the samples from the stream's ring buffer.
 */
UCLASS(ClassGroup = "Audio", meta = (BlueprintSpawnableComponent))
class DRIVINGGAME_API URadioSynthComponent : public USynthComponent
{
	GENERATED_BODY()

public:
	URadioSynthComponent(const FObjectInitializer& ObjectInitializer);

	TSharedPtr<FRadioPlayback, ESPMode::ThreadSafe> GetPlayback() const { return Playback; }

protected:
	virtual bool Init(int32& SampleRate) override;
	virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;

private:
	TSharedPtr<FRadioPlayback, ESPMode::ThreadSafe> Playback;
};
