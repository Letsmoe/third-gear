#include "CarSoundDsp.h"

namespace CarSoundDetail
{
constexpr float Pi = 3.14159265358979f;
constexpr float TwoPi = 6.28318530717959f;
constexpr int32 BlockFrames = 64;
constexpr int32 MaxPartials = 64;          // half orders of the crank speed: partial m is order m/2
constexpr float MaxPartialHz = 5000.f;
constexpr int32 MaxVoices = 24;
constexpr int32 SurfaceCount = static_cast<int32>(ECarRoadSurface::Count);
constexpr float IndicatorPeriodSeconds = 0.667f; // 90 flashes per minute

float DbToLinear(float Db)
{
	return FMath::Pow(10.f, Db * 0.05f);
}

/** Smooth bump in dB around a centre frequency, Gaussian in octaves. */
float BellDb(float Frequency, float CentreHz, float WidthOctaves, float GainDb)
{
	const float Octaves = FMath::Log2(FMath::Max(Frequency, 1.f) / CentreHz) / WidthOctaves;
	return GainDb * FMath::Exp(-0.5f * Octaves * Octaves);
}

/** One-pole smoothing step towards a target; Coefficient is the fraction of the gap closed per call. */
float Approach(float Current, float Target, float Coefficient)
{
	return Current + (Target - Current) * Coefficient;
}

/** Time constant in seconds to a per-call coefficient when called every StepSeconds. */
float CoefficientFor(float TimeConstantSeconds, float StepSeconds)
{
	return 1.f - FMath::Exp(-StepSeconds / FMath::Max(TimeConstantSeconds, 1e-4f));
}

/** Cheap white noise, uniform in -sqrt(3)..sqrt(3) (unit variance). */
struct FNoise
{
	uint32 State = 2463534242u;

	float Next()
	{
		State ^= State << 13;
		State ^= State >> 17;
		State ^= State << 5;
		return (static_cast<float>(State) * (1.f / 2147483648.f) - 1.f) * 1.7320508f;
	}

	/** Uniform 0..1. */
	float Unit()
	{
		return Next() * 0.28867513f + 0.5f;
	}
};

/** Second-order filter section (RBJ cookbook), transposed direct form II. */
struct FBiquad
{
	float B0 = 1.f, B1 = 0.f, B2 = 0.f, A1 = 0.f, A2 = 0.f;
	float Z1 = 0.f, Z2 = 0.f;

	void SetCoefficients(float InB0, float InB1, float InB2, float InA0, float InA1, float InA2)
	{
		B0 = InB0 / InA0;
		B1 = InB1 / InA0;
		B2 = InB2 / InA0;
		A1 = InA1 / InA0;
		A2 = InA2 / InA0;
	}

	void SetLowpass(float SampleRate, float Frequency, float Q)
	{
		const float W = TwoPi * FMath::Min(Frequency, SampleRate * 0.45f) / SampleRate;
		const float Alpha = FMath::Sin(W) / (2.f * Q);
		const float C = FMath::Cos(W);
		SetCoefficients((1.f - C) * 0.5f, 1.f - C, (1.f - C) * 0.5f, 1.f + Alpha, -2.f * C, 1.f - Alpha);
	}

	void SetHighpass(float SampleRate, float Frequency, float Q)
	{
		const float W = TwoPi * FMath::Min(Frequency, SampleRate * 0.45f) / SampleRate;
		const float Alpha = FMath::Sin(W) / (2.f * Q);
		const float C = FMath::Cos(W);
		SetCoefficients((1.f + C) * 0.5f, -(1.f + C), (1.f + C) * 0.5f, 1.f + Alpha, -2.f * C, 1.f - Alpha);
	}

	/** Band-pass with 0 dB gain at the centre frequency. */
	void SetBandpass(float SampleRate, float Frequency, float Q)
	{
		const float W = TwoPi * FMath::Min(Frequency, SampleRate * 0.45f) / SampleRate;
		const float Alpha = FMath::Sin(W) / (2.f * Q);
		SetCoefficients(Alpha, 0.f, -Alpha, 1.f + Alpha, -2.f * FMath::Cos(W), 1.f - Alpha);
	}

	float Process(float Input)
	{
		const float Output = B0 * Input + Z1;
		Z1 = B1 * Input - A1 * Output + Z2;
		Z2 = B2 * Input - A2 * Output;
		return Output;
	}
};

/** A one-shot sound in progress. */
struct FVoice
{
	ECarSoundEvent Type = ECarSoundEvent::GearClunk;
	float Time = 0.f;
	float Strength = 0.f;
	bool bActive = false;
};

struct FPendingEvent
{
	ECarSoundEvent Type;
	float Strength;
};

/** Partial phase offsets, fixed per partial: firing orders are phase aligned (a pulse), the rest are scattered. */
float PartialPhaseOffset(int32 Partial)
{
	if (Partial % 4 == 0)
	{
		return 0.f;
	}
	return FMath::Frac(Partial * 0.6180339f) * 0.35f * TwoPi;
}
}

using namespace CarSoundDetail;

struct FCarSoundDsp::FImpl
{
	explicit FImpl(int32 InSampleRate)
		: SampleRate(static_cast<float>(InSampleRate))
	{
		for (int32 Partial = 0; Partial < MaxPartials; ++Partial)
		{
			PhaseCos[Partial] = FMath::Cos(PartialPhaseOffset(Partial + 1));
			PhaseSin[Partial] = FMath::Sin(PartialPhaseOffset(Partial + 1));
		}
		NoiseLowpass.SetLowpass(SampleRate, 250.f, 0.7f);
		NoiseHighpass.SetHighpass(SampleRate, 2500.f, 0.7f);
		NoiseBand.SetBandpass(SampleRate, 3000.f, 1.2f);
		CombustionBand.SetBandpass(SampleRate, 1700.f, 0.8f);
		CombustionSmooth.SetLowpass(SampleRate, 2600.f, 0.7f);
		IntakeBand.SetBandpass(SampleRate, 420.f, 0.7f);
		IntakeSmooth.SetLowpass(SampleRate, 1200.f, 0.7f);
		BurbleBand.SetBandpass(SampleRate, 95.f, 2.f);
		InjectorBand.SetBandpass(SampleRate, 3600.f, 4.f);
		StarterBuzz.SetBandpass(SampleRate, 900.f, 2.5f);
		TurboHiss.SetBandpass(SampleRate, 5200.f, 1.2f);
		GrindBand.SetBandpass(SampleRate, 1800.f, 1.f);
		BlowOffBand.SetBandpass(SampleRate, 2600.f, 1.2f);
		BlowOffLow.SetBandpass(SampleRate, 700.f, 0.8f);
		EngineHighpass.SetHighpass(SampleRate, 22.f, 0.7f);
		for (int32 Channel = 0; Channel < 2; ++Channel)
		{
			RoadRoar[Channel].SetBandpass(SampleRate, 150.f, 0.7f);
			RoadMid[Channel].SetBandpass(SampleRate, 600.f, 0.6f);
			RoadHiss[Channel].SetBandpass(SampleRate, 3200.f, 0.5f);
			RoadSmooth[Channel].SetLowpass(SampleRate, 2500.f, 0.7f);
			WindBody[Channel].SetBandpass(SampleRate, 650.f, 0.5f);
			WindHiss[Channel].SetBandpass(SampleRate, 3000.f, 0.6f);
			WindSmooth[Channel].SetLowpass(SampleRate, 4000.f, 0.7f);
		}
		CobbleLow.SetBandpass(SampleRate, 120.f, 3.f);
		CobbleMid.SetBandpass(SampleRate, 430.f, 2.5f);
		CobbleHigh.SetBandpass(SampleRate, 1500.f, 2.f);
		PaverLow.SetBandpass(SampleRate, 135.f, 2.5f);
		PaverMid.SetBandpass(SampleRate, 700.f, 2.f);
		SquealA.SetBandpass(SampleRate, 1000.f, 30.f);
		SquealB.SetBandpass(SampleRate, 2100.f, 25.f);
		SquealScrub.SetBandpass(SampleRate, 1800.f, 1.f);
		for (int32 Channel = 0; Channel < 2; ++Channel)
		{
			NoiseGenerators[Channel].State = 2463534242u + 7919u * (Channel + 1);
		}
		EngineNoise.State = 123456789u;
		EventNoise.State = 987654321u;
	}

	// --- Cross-thread exchange ---
	FCriticalSection Lock;
	FCarSoundInputs PendingInputs;
	TArray<FPendingEvent> PendingEvents;

	// --- Configuration ---
	const float SampleRate;
	uint32 StemMask = CarSoundStem::All;

	// --- Current inputs and smoothed values ---
	FCarSoundInputs Inputs;
	float Rpm = 0.f;
	float Throttle = 0.f;
	float Load = 0.f;
	float Boost = 0.f;
	float Combustion = 0.f;          // 1 firing, 0 only compression pulses (cranking, spinning down)
	float StarterLevel = 0.f;
	float StarterPhase = 0.f;
	float SurfaceMix[SurfaceCount] = {1.f, 0.f, 0.f, 0.f};
	float WetnessSmooth = 0.f;
	float SpeedSmooth = 0.f;
	float AirSpeedSmooth = 0.f;
	float SquealSmooth = 0.f;
	float SquealPitchSmooth = 0.f;
	float PreviousThrottle = 0.f;
	float TurboSpeed = 0.f;
	float GrindLevel = 0.f;
	float RevLimiterGate = 1.f;
	float RevLimiterClock = 0.f;
	bool bPreviousIndicator = false;
	bool bThrottleWasHigh = false;
	float IndicatorClock = 0.f;

	// --- Engine oscillator state ---
	double CrankHalfPhase = 0.0;     // pi * crank revolutions; partial m is sin(m * CrankHalfPhase + offset)
	float PhaseCos[MaxPartials] = {};
	float PhaseSin[MaxPartials] = {};
	float PartialAmplitude[MaxPartials] = {};
	float PartialTarget[MaxPartials] = {};
	float PartialWobble[MaxPartials] = {};
	float CycleGain = 1.f;
	float CycleGainTarget = 1.f;
	float IdleWander = 0.f;
	float BurbleEnvelope = 0.f;
	float BlowOffEnvelope = 0.f;
	float InjectorEnvelope = 0.f;
	int32 PreviousFiringIndex = 0;

	// --- Filters and noise ---
	FNoise EngineNoise, EventNoise, NoiseGenerators[2];
	FBiquad NoiseLowpass, NoiseHighpass, NoiseBand;
	FBiquad CombustionBand, CombustionSmooth, IntakeBand, IntakeSmooth, BurbleBand, InjectorBand, StarterBuzz, TurboHiss;
	FBiquad GrindBand, BlowOffBand, BlowOffLow, EngineHighpass;
	FBiquad RoadRoar[2], RoadMid[2], RoadHiss[2], RoadSmooth[2], WindBody[2], WindHiss[2], WindSmooth[2];
	FBiquad CobbleLow, CobbleMid, CobbleHigh, PaverLow, PaverMid;
	FBiquad SquealA, SquealB, SquealScrub;
	float PaverClock = 0.f;
	float TurboPhase = 0.f;
	float SquealWobble = 0.f;
	float SquealClock = 0.f;
	float WindGust = 1.f;
	float WindGustTarget = 1.f;

	FVoice Voices[MaxVoices];

	/** Takes over the latest inputs and starts the posted one-shots. */
	void PullFromGameThread()
	{
		TArray<FPendingEvent> Events;
		{
			FScopeLock ScopeLock(&Lock);
			Inputs = PendingInputs;
			Events = MoveTemp(PendingEvents);
			PendingEvents.Reset();
		}
		for (const FPendingEvent& Event : Events)
		{
			StartVoice(Event.Type, Event.Strength);
		}
	}

	void StartVoice(ECarSoundEvent Type, float Strength)
	{
		FVoice* Slot = &Voices[0];
		for (FVoice& Voice : Voices)
		{
			if (!Voice.bActive)
			{
				Slot = &Voice;
				break;
			}
			if (Voice.Time > Slot->Time)
			{
				Slot = &Voice;
			}
		}
		Slot->Type = Type;
		Slot->Strength = FMath::Clamp(Strength, 0.f, 1.5f);
		Slot->Time = 0.f;
		Slot->bActive = true;
	}

	// --- Block rate: engine spectrum ---

	/** Source spectrum weight of partial m (order m/2): firing orders dominate, the others make it irregular. */
	float SourceWeight(int32 Partial, float LoadFraction, float Roughness) const
	{
		const float Order = Partial * 0.5f;
		const float Slope = FMath::Lerp(1.9f, 1.0f, LoadFraction);
		if (Partial % 4 == 0)
		{
			return FMath::Pow(Order * 0.5f, -Slope);
		}
		const float Irregular = Partial % 2 == 0 ? 0.30f : 0.20f; // whole orders 1, 3, 5 and half orders
		return Irregular * Roughness * FMath::Pow(FMath::Max(Order, 1.f), -1.3f) * (1.2f - 0.7f * LoadFraction);
	}

	/** Weight of partial m while the engine is only being turned over: compression pulses at the firing orders. */
	float MotoringWeight(int32 Partial) const
	{
		const float Order = Partial * 0.5f;
		if (Partial % 4 == 0)
		{
			return FMath::Pow(Order * 0.5f, -1.0f);
		}
		return Partial % 2 == 0 ? 0.25f * FMath::Pow(Order, -1.0f) : 0.08f / Order;
	}

	/** Interior transfer function in dB: cabin boom, intake and exhaust body, and the low pass of a closed car. */
	float CabinTransferDb(float Frequency, float LoadFraction) const
	{
		float Db = BellDb(Frequency, 92.f, 0.30f, 6.f) + BellDb(Frequency, 185.f, 0.25f, 3.f)
			+ BellDb(Frequency, 420.f, 0.40f, 2.f + 2.f * LoadFraction) + BellDb(Frequency, 900.f, 0.40f, 1.5f * LoadFraction);
		const float Corner = FMath::Lerp(650.f, 1300.f, LoadFraction) * FMath::Lerp(2.5f, 1.f, Inputs.CabinClosed);
		const float Ratio = Frequency / Corner;
		Db -= 10.f * FMath::LogX(10.f, 1.f + Ratio * Ratio * Ratio * Ratio);
		const float LowRatio = 30.f / FMath::Max(Frequency, 1.f);
		Db -= 10.f * FMath::LogX(10.f, 1.f + LowRatio * LowRatio * LowRatio * LowRatio);
		return Db;
	}

	/** Overall engine loudness in dB: louder with revs and load, quiet at idle and on the overrun. */
	float EngineLevelDb(float LoadFraction) const
	{
		const float RpmFraction = FMath::Clamp((Rpm - 700.f) / 5500.f, 0.f, 1.f);
		const float IdleBoost = FMath::Pow(1.f - RpmFraction, 6.f);
		return -40.f + 17.f * RpmFraction + 10.f * LoadFraction + 1.5f * (1.f - RpmFraction) * (1.f - LoadFraction) + 6.f * IdleBoost;
	}

	/** Recomputes the target amplitude of every partial from rpm, load and combustion state. */
	void UpdateEngineSpectrum(float BlockSeconds)
	{
		const float LoadFraction = FMath::Clamp(Load, 0.f, 1.f);
		const float Overrun = FMath::Clamp(-Load * 3.f, 0.f, 1.f);
		const float IdleFactor = FMath::Clamp(1.f - (Rpm - 800.f) / 1500.f, 0.f, 1.f);
		const float Roughness = 1.f + 0.8f * IdleFactor;
		const float CrankHz = Rpm / 60.f;
		const float Level = DbToLinear(EngineLevelDb(LoadFraction)) * FMath::Lerp(1.f, 0.6f, Overrun);
		const float MotoringLevel = DbToLinear(-27.f) * FMath::Clamp(Rpm / 450.f, 0.f, 1.f);
		const float WobbleStep = FMath::Sqrt(BlockSeconds / 0.3f);

		float CombustionNorm = 0.f;
		float MotoringNorm = 0.f;
		float CombustionTarget[MaxPartials];
		float MotoringTarget[MaxPartials];
		for (int32 Index = 0; Index < MaxPartials; ++Index)
		{
			const int32 Partial = Index + 1;
			const float Frequency = Partial * 0.5f * CrankHz;
			if (Frequency > MaxPartialHz || Frequency > SampleRate * 0.45f)
			{
				CombustionTarget[Index] = MotoringTarget[Index] = 0.f;
				continue;
			}
			PartialWobble[Index] = FMath::Clamp(PartialWobble[Index] * (1.f - BlockSeconds / 0.3f)
				+ WobbleStep * 0.9f * EngineNoise.Next(), -1.f, 1.f);
			const float Wobble = Partial % 4 == 0 ? 1.f : FMath::Max(0.f, 0.8f + 0.6f * PartialWobble[Index]);
			CombustionTarget[Index] = SourceWeight(Partial, LoadFraction, Roughness) * Wobble;
			MotoringTarget[Index] = MotoringWeight(Partial);
			CombustionNorm += CombustionTarget[Index] * CombustionTarget[Index];
			MotoringNorm += MotoringTarget[Index] * MotoringTarget[Index];
		}
		// Unit RMS source spectra, so that the level in dB above is the level of the result before the cabin shaping.
		const float CombustionScale = 1.41421356f / FMath::Sqrt(FMath::Max(CombustionNorm, 1e-6f));
		const float MotoringScale = 1.41421356f / FMath::Sqrt(FMath::Max(MotoringNorm, 1e-6f));
		for (int32 Index = 0; Index < MaxPartials; ++Index)
		{
			const float Frequency = (Index + 1) * 0.5f * CrankHz;
			const float Cabin = DbToLinear(CabinTransferDb(Frequency, LoadFraction));
			const float Fired = CombustionTarget[Index] * CombustionScale * Level * Combustion;
			const float Turned = MotoringTarget[Index] * MotoringScale * MotoringLevel * (1.f - Combustion);
			PartialTarget[Index] = (Fired + Turned) * Cabin;
		}
	}

	// --- Per-sample pieces ---

	/** Sum of the engine partials at the current crank phase; advances the amplitude ramps. */
	float EnginePartialsSample(float Cos, float Sin, const float* RampStep)
	{
		float Sum = 0.f;
		float PowerCos = Cos;
		float PowerSin = Sin;
		for (int32 Index = 0; Index < MaxPartials; ++Index)
		{
			PartialAmplitude[Index] += RampStep[Index];
			// sin(m * alpha + offset) = sin(m alpha) cos(offset) + cos(m alpha) sin(offset)
			Sum += PartialAmplitude[Index] * (PowerSin * PhaseCos[Index] + PowerCos * PhaseSin[Index]);
			const float NextCos = PowerCos * Cos - PowerSin * Sin;
			PowerSin = PowerSin * Cos + PowerCos * Sin;
			PowerCos = NextCos;
		}
		return Sum;
	}

	/** Quiet one-shot noise: lowpassed, high passed, band passed white noise shared by all voices. */
	struct FEventNoise
	{
		float Low = 0.f, High = 0.f, Band = 0.f;
	};

	float VoiceSample(const FVoice& Voice, const FEventNoise& Noise) const
	{
		const float T = Voice.Time;
		const float S = Voice.Strength;
		switch (Voice.Type)
		{
		case ECarSoundEvent::GearClunk:
		{
			const float Second = T > 0.055f ? T - 0.055f : 0.f; // the synchro ring seating, a little after the engagement
			float Y = 0.55f * FMath::Sin(TwoPi * 135.f * T) * FMath::Exp(-T / 0.045f)
				+ 0.30f * FMath::Sin(TwoPi * 340.f * T) * FMath::Exp(-T / 0.030f) + 0.22f * Noise.Low * FMath::Exp(-T / 0.020f);
			if (Second > 0.f)
			{
				Y += 0.30f * FMath::Sin(TwoPi * 180.f * Second) * FMath::Exp(-Second / 0.04f);
			}
			return 0.22f * S * Y;
		}
		case ECarSoundEvent::SuspensionThump:
		{
			const float Y = 0.9f * FMath::Sin(TwoPi * (52.f - 100.f * FMath::Min(T, 0.1f)) * T) * FMath::Exp(-T / 0.11f)
				+ 0.5f * Noise.Low * FMath::Exp(-T / 0.05f) + 0.12f * Noise.Band * FMath::Exp(-T / 0.012f);
			return 0.40f * S * Y;
		}
		case ECarSoundEvent::HandbrakePull:
		{
			float Y = 0.f;
			float ClickTime = 0.f;
			for (int32 Click = 0; Click < 6; ++Click)
			{
				ClickTime += 0.045f + 0.014f * Click;
				const float Local = T - ClickTime;
				if (Local > 0.f)
				{
					Y += 0.55f * Noise.Band * FMath::Exp(-Local / 0.0025f) + 0.35f * FMath::Sin(TwoPi * (900.f + 60.f * Click) * Local) * FMath::Exp(-Local / 0.007f);
				}
			}
			return 0.10f * S * Y;
		}
		case ECarSoundEvent::HandbrakeRelease:
		{
			const float Second = T - 0.17f;
			float Y = 0.5f * Noise.Band * FMath::Exp(-T / 0.003f) + 0.45f * FMath::Sin(TwoPi * 520.f * T) * FMath::Exp(-T / 0.012f);
			if (Second > 0.f)
			{
				Y += 0.5f * FMath::Sin(TwoPi * 110.f * Second) * FMath::Exp(-Second / 0.05f) + 0.2f * Noise.Low * FMath::Exp(-Second / 0.03f);
			}
			return 0.13f * S * Y;
		}
		case ECarSoundEvent::StallShudder:
		{
			const float Envelope = FMath::Exp(-T / 0.35f) * FMath::Min(1.f, T / 0.03f);
			const float Y = 0.7f * FMath::Sin(TwoPi * 9.f * T) + 0.5f * FMath::Sin(TwoPi * 23.f * T + 1.f) + 0.35f * FMath::Sin(TwoPi * 41.f * T)
				+ 0.5f * Noise.Low;
			return 0.20f * S * Envelope * Y;
		}
		case ECarSoundEvent::StarterSolenoid:
		{
			const float Y = 0.5f * FMath::Sin(TwoPi * 220.f * T) * FMath::Exp(-T / 0.030f)
				+ 0.4f * FMath::Sin(TwoPi * 780.f * T) * FMath::Exp(-T / 0.012f) + 0.3f * Noise.Band * FMath::Exp(-T / 0.006f);
			return 0.22f * S * Y;
		}
		case ECarSoundEvent::BlowOff:
			return 0.f; // rendered by BlowOffSample, which has its own filters
		case ECarSoundEvent::IndicatorOn:
		{
			const float Y = 0.8f * Noise.Band * FMath::Exp(-T / 0.0018f) + 0.5f * FMath::Sin(TwoPi * 2900.f * T) * FMath::Exp(-T / 0.0035f)
				+ 0.5f * FMath::Sin(TwoPi * 1250.f * T) * FMath::Exp(-T / 0.009f) + 0.35f * FMath::Sin(TwoPi * 380.f * T) * FMath::Exp(-T / 0.018f);
			return 0.050f * S * Y;
		}
		case ECarSoundEvent::IndicatorOff:
		{
			const float Y = 0.6f * Noise.Band * FMath::Exp(-T / 0.0018f) + 0.45f * FMath::Sin(TwoPi * 2300.f * T) * FMath::Exp(-T / 0.0035f)
				+ 0.5f * FMath::Sin(TwoPi * 1000.f * T) * FMath::Exp(-T / 0.010f) + 0.3f * FMath::Sin(TwoPi * 330.f * T) * FMath::Exp(-T / 0.02f);
			return 0.040f * S * Y;
		}
		}
		return 0.f;
	}

	float VoiceDuration(ECarSoundEvent Type) const
	{
		switch (Type)
		{
		case ECarSoundEvent::StallShudder: return 1.6f;
		case ECarSoundEvent::HandbrakePull: return 0.75f;
		case ECarSoundEvent::BlowOff: return 1.f;
		default: return 0.5f;
		}
	}

	/** Mixes all running one-shots for one sample. */
	float VoicesSample(float DeltaSeconds, const FEventNoise& Noise)
	{
		float Sum = 0.f;
		for (FVoice& Voice : Voices)
		{
			if (!Voice.bActive)
			{
				continue;
			}
			if (Voice.Type == ECarSoundEvent::BlowOff)
			{
				Sum += BlowOffSample(Voice);
			}
			else
			{
				Sum += VoiceSample(Voice, Noise);
			}
			Voice.Time += DeltaSeconds;
			if (Voice.Time > VoiceDuration(Voice.Type))
			{
				Voice.bActive = false;
			}
		}
		return Sum;
	}

	/** Recirculation valve: a soft broadband whoosh with a little tonal flutter, loudest at high boost. */
	float BlowOffSample(const FVoice& Voice)
	{
		const float Envelope = FMath::Min(1.f, Voice.Time / 0.010f) * FMath::Exp(-Voice.Time / 0.26f);
		const float White = EventNoise.Next();
		const float Whoosh = BlowOffBand.Process(White) * 0.9f + BlowOffLow.Process(White) * 0.7f;
		const float Flutter = FMath::Sin(TwoPi * 2900.f * Voice.Time) * 0.15f * (0.5f + 0.5f * FMath::Sin(TwoPi * 22.f * Voice.Time));
		return 0.028f * Voice.Strength * Envelope * (Whoosh + Flutter);
	}

	// --- Road, tyres, wind ---

	struct FRoadBlock
	{
		float RoarGain = 0.f, MidGain = 0.f, HissGain = 0.f;
		float WindBodyGain = 0.f, WindHissGain = 0.f;
		float CobbleRate = 0.f, CobbleGain = 0.f, PaverRate = 0.f, PaverGain = 0.f;
		float SquealGain = 0.f, ScrubGain = 0.f;
		float SquealCentreHz = 1000.f;
	};

	FRoadBlock ComputeRoadBlock(float BlockSeconds)
	{
		FRoadBlock Block;
		const float Speed = FMath::Max(SpeedSmooth, 0.f);
		const float Fraction = Speed / 27.8f; // 100 km/h
		const float Asphalt = SurfaceMix[0] + 0.6f * SurfaceMix[3];
		const float Cobble = SurfaceMix[1];
		const float Pavers = SurfaceMix[2];
		const float Wet = WetnessSmooth;

		// Tyre noise power rises roughly with speed^3 to ^4; amplitude with speed^1.5 to ^2.
		const float RoarSpeed = FMath::Pow(Fraction, 1.5f);
		const float MidSpeed = FMath::Pow(Fraction, 1.5f);
		Block.RoarGain = 0.10f * RoarSpeed * (0.9f * Asphalt + 1.7f * Cobble + 1.3f * Pavers + 1.4f * SurfaceMix[3]);
		Block.MidGain = 0.10f * MidSpeed * (0.9f * Asphalt + 1.5f * Cobble + 1.2f * Pavers) * (1.f - 0.2f * Wet);
		Block.HissGain = 0.025f * FMath::Pow(Fraction, 2.f) * (0.25f + 2.6f * Wet) * (1.f + 0.4f * Cobble);
		RoadMid[0].SetBandpass(SampleRate, 450.f + 450.f * FMath::Min(Fraction, 1.6f), 0.6f);
		RoadMid[1].SetBandpass(SampleRate, 470.f + 440.f * FMath::Min(Fraction, 1.6f), 0.6f);

		Block.CobbleRate = Speed / 0.10f;
		Block.CobbleGain = Cobble * FMath::Pow(FMath::Min(Speed / 14.f, 1.6f), 1.2f);
		Block.PaverRate = Speed / 0.20f;
		Block.PaverGain = Pavers * FMath::Pow(FMath::Min(Speed / 14.f, 1.6f), 1.2f);

		const float Wind = FMath::Pow(AirSpeedSmooth / 36.f, 2.6f);
		Block.WindBodyGain = 0.045f * Wind * WindGust;
		Block.WindHissGain = 0.009f * Wind * WindGust;

		Block.SquealGain = 0.11f * FMath::Pow(SquealSmooth, 1.3f);
		Block.ScrubGain = 0.045f * SquealSmooth;
		SquealClock += BlockSeconds * 7.f;
		SquealWobble = 0.06f * FMath::Sin(TwoPi * SquealClock) + 0.025f * FMath::Sin(TwoPi * SquealClock * 2.3f + 1.f);
		const float Centre = (820.f + 6.f * Speed * 3.6f * 0.5f + 380.f * SquealPitchSmooth) * (1.f + SquealWobble);
		SquealA.SetBandpass(SampleRate, Centre, 28.f);
		SquealB.SetBandpass(SampleRate, Centre * 2.07f, 24.f);
		return Block;
	}

	/** One stereo sample of tyres on the road and wind. */
	void RoadAndWindSample(const FRoadBlock& Block, float* OutRoad, float* OutWind)
	{
		float Impulse = 0.f;
		if (Block.CobbleGain > 0.001f)
		{
			if (NoiseGenerators[0].Unit() < Block.CobbleRate / SampleRate)
			{
				Impulse = 2.5f * (0.3f + 0.7f * NoiseGenerators[1].Unit());
			}
		}
		const float CobbleSound = Block.CobbleGain
			* (CobbleLow.Process(Impulse) * 1.4f + CobbleMid.Process(Impulse) * 1.0f + CobbleHigh.Process(Impulse) * 0.5f);

		float PaverImpulse = 0.f;
		if (Block.PaverGain > 0.001f)
		{
			PaverClock += Block.PaverRate / SampleRate;
			if (PaverClock >= 1.f)
			{
				PaverClock -= 1.f + 0.2f * (NoiseGenerators[1].Unit() - 0.5f);
				PaverImpulse = 2.0f * (0.7f + 0.3f * NoiseGenerators[0].Unit());
			}
		}
		const float PaverSound = Block.PaverGain * (PaverLow.Process(PaverImpulse) * 1.1f + PaverMid.Process(PaverImpulse) * 0.7f);

		const float SharedNoise = NoiseGenerators[0].Next();
		const float SquealSource = SharedNoise * 0.5f + NoiseGenerators[1].Next() * 0.5f;
		const float Squeal = Block.SquealGain * (SquealA.Process(SquealSource) * 6.f + SquealB.Process(SquealSource) * 2.5f)
			+ Block.ScrubGain * SquealScrub.Process(SquealSource);

		for (int32 Channel = 0; Channel < 2; ++Channel)
		{
			const float White = NoiseGenerators[Channel].Next();
			const float Roar = RoadRoar[Channel].Process(White) * Block.RoarGain * 2.4f;
			const float Mid = RoadMid[Channel].Process(White) * Block.MidGain * 1.6f;
			const float Hiss = RoadHiss[Channel].Process(White) * Block.HissGain;
			const float Road = RoadSmooth[Channel].Process(Roar + Mid) + Hiss;
			const float WindWhite = NoiseGenerators[Channel].Next();
			const float Wind = WindBody[Channel].Process(WindWhite) * Block.WindBodyGain * 2.f
				+ WindHiss[Channel].Process(WindWhite) * Block.WindHissGain * 2.f;
			const float Side = Channel == 0 ? 1.f : 0.92f;
			OutRoad[Channel] = Road + (CobbleSound + PaverSound) * Side + Squeal * Side * 0.5f;
			OutWind[Channel] = Wind;
		}
	}

	// --- Engine sample (everything except the partial sum) ---

	struct FEngineBlock
	{
		float CombustionNoiseGain = 0.f, IntakeGain = 0.f, TurboGain = 0.f, BurbleRate = 0.f, InjectorGain = 0.f;
		float StarterGain = 0.f, StarterHz = 0.f, GrindGain = 0.f, TurboHz = 0.f;
		float RevGate = 1.f;
	};

	FEngineBlock ComputeEngineBlock(float BlockSeconds)
	{
		FEngineBlock Block;
		const float LoadFraction = FMath::Clamp(Load, 0.f, 1.f);
		const float RpmFraction = FMath::Clamp((Rpm - 600.f) / 5600.f, 0.f, 1.f);
		const float Level = DbToLinear(EngineLevelDb(LoadFraction));
		const float Cabin = FMath::Lerp(2.5f, 1.f, Inputs.CabinClosed);

		Block.CombustionNoiseGain = Combustion * Level * (0.15f + 0.85f * LoadFraction) * 0.9f * Cabin;
		Block.IntakeGain = Combustion * DbToLinear(-39.f) * Throttle * FMath::Pow(RpmFraction, 1.2f) * (0.4f + 0.6f * LoadFraction) * 1.5f * Cabin;
		Block.InjectorGain = Combustion * DbToLinear(-46.f) * (1.2f - 0.7f * RpmFraction);

		TurboSpeed = Approach(TurboSpeed, Boost * (0.35f + 0.65f * RpmFraction) * Throttle, CoefficientFor(0.25f, BlockSeconds));
		Block.TurboHz = 2600.f + 3800.f * TurboSpeed;
		Block.TurboGain = DbToLinear(-68.f + 24.f * TurboSpeed) * TurboSpeed * Cabin;

		const bool bOverrun = Load < -0.04f && Throttle < 0.05f && Rpm > 1700.f && Combustion > 0.5f;
		Block.BurbleRate = bOverrun ? 3.f + 7.f * FMath::Clamp((Rpm - 1700.f) / 3500.f, 0.f, 1.f) : 0.f;

		const float StarterTarget = Inputs.bCranking && !Inputs.bEngineRunning ? 1.f : 0.f;
		StarterLevel = Approach(StarterLevel, StarterTarget, CoefficientFor(StarterTarget > StarterLevel ? 0.05f : 0.09f, BlockSeconds));
		Block.StarterGain = StarterLevel * DbToLinear(-29.f);
		Block.StarterHz = FMath::Clamp(520.f * Rpm / 250.f, 160.f, 950.f);

		GrindLevel = Approach(GrindLevel, Inputs.bGrinding ? 1.f : 0.f, CoefficientFor(0.03f, BlockSeconds));
		Block.GrindGain = GrindLevel * DbToLinear(-34.f);

		if (Inputs.bRevLimiter)
		{
			RevLimiterClock += BlockSeconds * 13.f;
			const float Phase = FMath::Frac(RevLimiterClock);
			RevLimiterGate = Approach(RevLimiterGate, Phase < 0.62f ? 1.f : 0.2f, CoefficientFor(0.004f, BlockSeconds));
		}
		else
		{
			RevLimiterGate = Approach(RevLimiterGate, 1.f, CoefficientFor(0.01f, BlockSeconds));
		}
		Block.RevGate = RevLimiterGate;
		return Block;
	}

	/** Engine sounds other than the harmonic series, for one sample. */
	float EngineExtrasSample(const FEngineBlock& Block, float FiringPulse, float DeltaSeconds)
	{
		const float White = EngineNoise.Next();
		float Sum = 0.f;
		Sum += CombustionSmooth.Process(CombustionBand.Process(White)) * Block.CombustionNoiseGain * FiringPulse * 1.6f * Block.RevGate;
		Sum += IntakeSmooth.Process(IntakeBand.Process(White)) * Block.IntakeGain * (0.8f + 0.4f * FiringPulse);

		if (Block.TurboGain > 1e-6f)
		{
			TurboPhase = FMath::Frac(TurboPhase + Block.TurboHz * DeltaSeconds);
			Sum += Block.TurboGain * (FMath::Sin(TwoPi * TurboPhase) * 0.8f + TurboHiss.Process(White) * 0.9f);
		}
		if (Block.InjectorGain > 1e-7f)
		{
			Sum += InjectorBand.Process(White) * Block.InjectorGain * InjectorEnvelope * 8.f;
		}
		if (BurbleEnvelope > 1e-4f)
		{
			Sum += BurbleBand.Process(White) * BurbleEnvelope * DbToLinear(-26.f) * 4.f;
			BurbleEnvelope *= FMath::Exp(-DeltaSeconds / 0.05f);
		}
		if (Block.BurbleRate > 0.f && EngineNoise.Unit() < Block.BurbleRate * DeltaSeconds)
		{
			BurbleEnvelope = 0.3f + 0.7f * EngineNoise.Unit();
		}
		if (Block.StarterGain > 1e-6f)
		{
			StarterPhase = FMath::Frac(StarterPhase + Block.StarterHz * DeltaSeconds);
			const float Whine = FMath::Sin(TwoPi * StarterPhase) + 0.45f * FMath::Sin(TwoPi * 2.f * StarterPhase) + 0.25f * FMath::Sin(TwoPi * 0.5f * StarterPhase);
			const float Strain = 0.75f + 0.25f * FMath::Sin(4.f * static_cast<float>(CrankHalfPhase)); // slows on each compression
			Sum += Block.StarterGain * (Whine * 0.7f + StarterBuzz.Process(White) * 0.9f) * Strain;
		}
		if (Block.GrindGain > 1e-6f)
		{
			Sum += GrindBand.Process(White) * Block.GrindGain * (0.5f + 0.5f * FMath::Sin(TwoPi * 83.f * static_cast<float>(CrankHalfPhase)));
		}
		InjectorEnvelope *= FMath::Exp(-DeltaSeconds / 0.0025f);
		return Sum;
	}

	/** Clicks the flasher relay: on at the start of each period, off in the middle, and off once more when cancelled. */
	void UpdateIndicatorRelay(float BlockSeconds)
	{
		if (Inputs.bIndicatorOn && !bPreviousIndicator)
		{
			IndicatorClock = 0.f;
			StartVoice(ECarSoundEvent::IndicatorOn, 1.f);
		}
		else if (Inputs.bIndicatorOn)
		{
			const float PreviousPhase = FMath::Frac(IndicatorClock / IndicatorPeriodSeconds);
			IndicatorClock += BlockSeconds;
			const float Phase = FMath::Frac(IndicatorClock / IndicatorPeriodSeconds);
			if (Phase < PreviousPhase)
			{
				StartVoice(ECarSoundEvent::IndicatorOn, 1.f);
			}
			else if (PreviousPhase < 0.5f && Phase >= 0.5f)
			{
				StartVoice(ECarSoundEvent::IndicatorOff, 1.f);
			}
		}
		else if (bPreviousIndicator && FMath::Frac(IndicatorClock / IndicatorPeriodSeconds) < 0.5f)
		{
			StartVoice(ECarSoundEvent::IndicatorOff, 1.f);
		}
		bPreviousIndicator = Inputs.bIndicatorOn;
	}

	void RenderBlock(float* OutStereo, int32 Frames)
	{
		const float BlockSeconds = Frames / SampleRate;
		const float DeltaSeconds = 1.f / SampleRate;

		// Smooth the control inputs at block rate.
		const float Fast = CoefficientFor(0.015f, BlockSeconds);
		const float TargetRpm = Inputs.bEngineRunning || Inputs.bCranking || Inputs.EngineRpm > 1.f ? Inputs.EngineRpm : 0.f;
		const float IdleFactor = FMath::Clamp(1.f - (TargetRpm - 800.f) / 1500.f, 0.f, 1.f);
		IdleWander = Approach(IdleWander, EngineNoise.Next() * 0.012f * IdleFactor * (Inputs.bEngineRunning ? 1.f : 0.f), CoefficientFor(0.25f, BlockSeconds));
		Rpm = Approach(Rpm, TargetRpm * (1.f + IdleWander), Fast);
		Throttle = Approach(Throttle, Inputs.Throttle, CoefficientFor(0.03f, BlockSeconds));
		Load = Approach(Load, Inputs.Load, CoefficientFor(0.05f, BlockSeconds));
		Boost = Approach(Boost, Inputs.Boost, CoefficientFor(0.05f, BlockSeconds));
		Combustion = Approach(Combustion, Inputs.bEngineRunning ? 1.f : 0.f, CoefficientFor(Inputs.bEngineRunning ? 0.04f : 0.03f, BlockSeconds));
		if (Throttle > 0.6f)
		{
			bThrottleWasHigh = true;
		}
		else if (Throttle < 0.2f && bThrottleWasHigh)
		{
			bThrottleWasHigh = false;
			if (Boost > 0.35f)
			{
				StartVoice(ECarSoundEvent::BlowOff, Boost); // the recirculation valve opens when the throttle closes under boost
			}
		}
		SpeedSmooth = Approach(SpeedSmooth, Inputs.SpeedMps, CoefficientFor(0.1f, BlockSeconds));
		AirSpeedSmooth = Approach(AirSpeedSmooth, Inputs.AirSpeedMps, CoefficientFor(0.3f, BlockSeconds));
		WetnessSmooth = Approach(WetnessSmooth, Inputs.Wetness, CoefficientFor(1.f, BlockSeconds));
		SquealSmooth = Approach(SquealSmooth, Inputs.SquealLevel, CoefficientFor(Inputs.SquealLevel > SquealSmooth ? 0.04f : 0.12f, BlockSeconds));
		SquealPitchSmooth = Approach(SquealPitchSmooth, Inputs.SquealPitch, CoefficientFor(0.1f, BlockSeconds));
		for (int32 Surface = 0; Surface < SurfaceCount; ++Surface)
		{
			SurfaceMix[Surface] = Approach(SurfaceMix[Surface], Inputs.SurfaceWeights[Surface], CoefficientFor(0.15f, BlockSeconds));
		}
		WindGustTarget = FMath::Clamp(WindGustTarget + 0.5f * FMath::Sqrt(BlockSeconds) * EngineNoise.Next() - (WindGustTarget - 1.f) * BlockSeconds * 0.8f, 0.6f, 1.5f);
		WindGust = Approach(WindGust, WindGustTarget, CoefficientFor(0.4f, BlockSeconds));

		UpdateIndicatorRelay(BlockSeconds);

		UpdateEngineSpectrum(BlockSeconds);
		const FEngineBlock EngineBlock = ComputeEngineBlock(BlockSeconds);
		const FRoadBlock RoadBlock = ComputeRoadBlock(BlockSeconds);

		float RampStep[MaxPartials];
		for (int32 Index = 0; Index < MaxPartials; ++Index)
		{
			RampStep[Index] = (PartialTarget[Index] - PartialAmplitude[Index]) / Frames;
		}
		const double HalfPhaseStep = static_cast<double>(Pi) * Rpm / 60.0 / SampleRate;
		const float CycleCoefficient = CoefficientFor(0.006f, DeltaSeconds);
		const float JitterAmount = (0.04f + 0.12f * IdleFactor) * Combustion;

		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			CrankHalfPhase += HalfPhaseStep;
			if (CrankHalfPhase > 1e5)
			{
				CrankHalfPhase = FMath::Fmod(CrankHalfPhase, static_cast<double>(TwoPi));
			}
			const float Angle = static_cast<float>(FMath::Fmod(CrankHalfPhase, static_cast<double>(TwoPi)));
			const int32 FiringIndex = static_cast<int32>(CrankHalfPhase / (0.5 * Pi)); // one firing per quarter of a half-phase turn pair
			if (FiringIndex != PreviousFiringIndex)
			{
				PreviousFiringIndex = FiringIndex;
				CycleGainTarget = 1.f + JitterAmount * EngineNoise.Next();
				InjectorEnvelope = 0.5f + 0.5f * EngineNoise.Unit();
			}
			CycleGain += (CycleGainTarget - CycleGain) * CycleCoefficient;

			const float Cos = FMath::Cos(Angle);
			const float Sin = FMath::Sin(Angle);
			const float PulseBase = 0.5f + 0.5f * FMath::Cos(4.f * Angle);
			const float FiringPulse = PulseBase * PulseBase * PulseBase * PulseBase * PulseBase;
			float Engine = EnginePartialsSample(Cos, Sin, RampStep) * CycleGain * (0.35f + 0.65f * EngineBlock.RevGate);
			Engine += EngineExtrasSample(EngineBlock, FiringPulse, DeltaSeconds);

			FEventNoise Noise;
			const float EventWhite = EventNoise.Next();
			Noise.Low = NoiseLowpass.Process(EventWhite) * 2.f;
			Noise.High = NoiseHighpass.Process(EventWhite);
			Noise.Band = NoiseBand.Process(EventWhite) * 2.f;

			float Road[2] = {}, Wind[2] = {};
			RoadAndWindSample(RoadBlock, Road, Wind);
			const float OneShots = VoicesSample(DeltaSeconds, Noise) * ((StemMask & CarSoundStem::Events) ? 1.f : 0.f);
			Engine = EngineHighpass.Process(Engine) * ((StemMask & CarSoundStem::Engine) ? 1.f : 0.f);
			const float RoadGain = (StemMask & CarSoundStem::Road) ? 1.f : 0.f;
			const float WindGain = (StemMask & CarSoundStem::Wind) ? 1.f : 0.f;

			OutStereo[2 * Frame] = SoftLimit(Engine + Road[0] * RoadGain + Wind[0] * WindGain + OneShots);
			OutStereo[2 * Frame + 1] = SoftLimit(Engine + Road[1] * RoadGain + Wind[1] * WindGain + OneShots);
		}
		for (int32 Index = 0; Index < MaxPartials; ++Index)
		{
			PartialAmplitude[Index] = PartialTarget[Index];
		}
	}

	/** Linear below 0.7, then a tanh knee towards 1, so a loud moment never clips. */
	static float SoftLimit(float Sample)
	{
		const float Magnitude = FMath::Abs(Sample);
		if (Magnitude < 0.7f)
		{
			return Sample;
		}
		return FMath::Sign(Sample) * (0.7f + 0.3f * FMath::Tanh((Magnitude - 0.7f) / 0.3f));
	}
};

FCarSoundDsp::FCarSoundDsp(int32 InSampleRate)
	: Impl(MakeUnique<FImpl>(InSampleRate))
{
}

FCarSoundDsp::~FCarSoundDsp() = default;

void FCarSoundDsp::SetInputs(const FCarSoundInputs& NewInputs)
{
	FScopeLock ScopeLock(&Impl->Lock);
	Impl->PendingInputs = NewInputs;
}

void FCarSoundDsp::SetStemMask(uint32 Mask)
{
	Impl->StemMask = Mask;
}

void FCarSoundDsp::PostEvent(ECarSoundEvent Event, float Strength)
{
	FScopeLock ScopeLock(&Impl->Lock);
	if (Impl->PendingEvents.Num() < 32)
	{
		Impl->PendingEvents.Add({Event, Strength});
	}
}

void FCarSoundDsp::Render(float* OutStereo, int32 NumFrames)
{
	Impl->PullFromGameThread();
	int32 Done = 0;
	while (Done < NumFrames)
	{
		const int32 Frames = FMath::Min(BlockFrames, NumFrames - Done);
		Impl->RenderBlock(OutStereo + 2 * Done, Frames);
		Done += Frames;
	}
}
