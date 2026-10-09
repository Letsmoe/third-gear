#pragma once

#include "CoreMinimal.h"
#include "Components/SynthComponent.h"
#include "CarSynthComponent.generated.h"

class FCarSoundDsp;

/**
 * Plays the procedural car sound (FCarSoundDsp) through the audio mixer. The game thread sets inputs and posts events
 * on the shared DSP object, the audio thread renders it. A SynthComponent rather than a MetaSound: the DSP is plain
 * C++ that the offline test renders to a WAV file with the same code, and there is no binary graph to review.
 */
UCLASS(ClassGroup = "Audio", meta = (BlueprintSpawnableComponent))
class DRIVINGGAME_API UCarSynthComponent : public USynthComponent
{
	GENERATED_BODY()

public:
	UCarSynthComponent(const FObjectInitializer& ObjectInitializer);

	/** The synthesiser behind this component; valid from construction. */
	TSharedPtr<FCarSoundDsp, ESPMode::ThreadSafe> GetDsp() const { return Dsp; }

protected:
	virtual bool Init(int32& SampleRate) override;
	virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;

private:
	TSharedPtr<FCarSoundDsp, ESPMode::ThreadSafe> Dsp;
	int32 CurrentSampleRate = 48000;
};
