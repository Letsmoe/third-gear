#include "CarSynthComponent.h"

#include "CarSoundDsp.h"
#include "Sound/SoundGenerator.h"

namespace
{
/** Audio-thread side: renders the shared DSP in whatever block size the mixer asks for. */
class FCarSoundGenerator : public ISoundGenerator
{
public:
	explicit FCarSoundGenerator(TSharedPtr<FCarSoundDsp, ESPMode::ThreadSafe> InDsp)
		: Dsp(MoveTemp(InDsp))
	{
	}

	virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override
	{
		Dsp->Render(OutAudio, NumSamples / 2);
		return NumSamples;
	}

	virtual int32 GetDesiredNumSamplesToRenderPerCallback() const override { return 512; }

private:
	TSharedPtr<FCarSoundDsp, ESPMode::ThreadSafe> Dsp;
};
}

UCarSynthComponent::UCarSynthComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NumChannels = 2;
	PreferredBufferLength = 512;
	bAutoActivate = true;
	bIsUISound = true;          // not positioned in the world: it is the sound inside the cabin
	bAllowSpatialization = false;
	Dsp = MakeShared<FCarSoundDsp, ESPMode::ThreadSafe>(CurrentSampleRate);
}

bool UCarSynthComponent::Init(int32& SampleRate)
{
	SampleRate = CurrentSampleRate;
	NumChannels = 2;
	return true;
}

ISoundGeneratorPtr UCarSynthComponent::CreateSoundGenerator(const FSoundGeneratorInitParams& InParams)
{
	return MakeShared<FCarSoundGenerator, ESPMode::ThreadSafe>(Dsp);
}
