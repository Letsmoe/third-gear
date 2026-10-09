#include "RadioSynthComponent.h"

#include "Sound/SoundGenerator.h"

namespace
{
constexpr int32 RenderBlockFrames = 512;
constexpr float HighPassHz = 110.f;
constexpr float LowPassHz = 12000.f;
constexpr float GainRampSeconds = 0.08f;

/** Second-order filter section (RBJ cookbook), one per channel and band edge. */
struct FBiquad
{
	float B0 = 1.f, B1 = 0.f, B2 = 0.f, A1 = 0.f, A2 = 0.f;
	float Z1 = 0.f, Z2 = 0.f;

	/** Butterworth high pass (bHighPass) or low pass at CutoffHz. */
	void Design(bool bHighPass, float CutoffHz, float SampleRate)
	{
		const float Omega = 2.f * PI * CutoffHz / SampleRate;
		const float Alpha = FMath::Sin(Omega) / (2.f * 0.70710678f);
		const float Cosine = FMath::Cos(Omega);
		const float A0 = 1.f + Alpha;
		const float Edge = bHighPass ? (1.f + Cosine) * 0.5f : (1.f - Cosine) * 0.5f;
		B0 = Edge / A0;
		B1 = (bHighPass ? -2.f * Edge : 2.f * Edge) / A0;
		B2 = Edge / A0;
		A1 = -2.f * Cosine / A0;
		A2 = (1.f - Alpha) / A0;
	}

	float Process(float Input)
	{
		const float Output = B0 * Input + Z1;
		Z1 = B1 * Input - A1 * Output + Z2;
		Z2 = B2 * Input - A2 * Output;
		return Output;
	}
};

/** Audio-thread side: pulls the stream, shapes it like a car's speakers and fades it with the gain. */
class FRadioSoundGenerator : public ISoundGenerator
{
public:
	FRadioSoundGenerator(TSharedPtr<FRadioPlayback, ESPMode::ThreadSafe> InPlayback, float InSampleRate)
		: Playback(MoveTemp(InPlayback))
		, GainStep(1.f / (GainRampSeconds * InSampleRate))
	{
		for (int32 Channel = 0; Channel < 2; ++Channel)
		{
			HighPass[Channel].Design(true, HighPassHz, InSampleRate);
			LowPass[Channel].Design(false, LowPassHz, InSampleRate);
		}
	}

	virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override
	{
		const TSharedPtr<FRadioStreamer, ESPMode::ThreadSafe> Streamer = Playback->GetStreamer();
		const float TargetGain = Streamer.IsValid() ? Playback->GetTargetGain() : 0.f;
		if (Streamer.IsValid()) // also read while muted, so the stream stays live
		{
			Streamer->ReadAudio(OutAudio, NumSamples);
		}
		else
		{
			FMemory::Memzero(OutAudio, NumSamples * sizeof(float));
		}
		ShapeAndFade(OutAudio, NumSamples / 2, TargetGain);
		return NumSamples;
	}

	virtual int32 GetDesiredNumSamplesToRenderPerCallback() const override { return RenderBlockFrames * 2; }

private:
	/** Band-limits the interleaved stereo block and moves the gain towards its target without clicks. */
	void ShapeAndFade(float* Audio, int32 Frames, float TargetGain)
	{
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			Gain = FMath::FInterpConstantTo(Gain, TargetGain, 1.f, GainStep);
			for (int32 Channel = 0; Channel < 2; ++Channel)
			{
				float& Sample = Audio[2 * Frame + Channel];
				Sample = LowPass[Channel].Process(HighPass[Channel].Process(Sample)) * Gain;
			}
		}
	}

	TSharedPtr<FRadioPlayback, ESPMode::ThreadSafe> Playback;
	FBiquad HighPass[2];
	FBiquad LowPass[2];
	float Gain = 0.f;
	float GainStep = 0.001f;
};
}

void FRadioPlayback::SetStreamer(const TSharedPtr<FRadioStreamer, ESPMode::ThreadSafe>& InStreamer)
{
	FScopeLock ScopeLock(&Lock);
	Streamer = InStreamer;
}

TSharedPtr<FRadioStreamer, ESPMode::ThreadSafe> FRadioPlayback::GetStreamer()
{
	FScopeLock ScopeLock(&Lock);
	return Streamer;
}

URadioSynthComponent::URadioSynthComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NumChannels = 2;
	PreferredBufferLength = RenderBlockFrames;
	bAutoActivate = true;
	bIsUISound = true;          // not positioned in the world: it is the sound inside the cabin
	bAllowSpatialization = false;
	Playback = MakeShared<FRadioPlayback, ESPMode::ThreadSafe>();
}

bool URadioSynthComponent::Init(int32& SampleRate)
{
	SampleRate = FRadioStreamer::SampleRate;
	NumChannels = 2;
	return true;
}

ISoundGeneratorPtr URadioSynthComponent::CreateSoundGenerator(const FSoundGeneratorInitParams& InParams)
{
	return MakeShared<FRadioSoundGenerator, ESPMode::ThreadSafe>(Playback, static_cast<float>(FRadioStreamer::SampleRate));
}
