#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Templates/SharedPointer.h"
#include <atomic>

DECLARE_LOG_CATEGORY_EXTERN(LogRadio, Log, All);

/** An external program with its output pipes: ffmpeg decodes the audio, curl carries the song titles. */
class FRadioChildProcess
{
public:
	~FRadioChildProcess() { Kill(); }

	/** Starts the program; false if it could not be launched. It dies with the game even if the game crashes (setpriv --pdeathsig). */
	bool Launch(const FString& Executable, const FString& Arguments);

	/** Moves everything the child wrote to its standard output since the last call to Output. */
	bool ReadOutput(TArray<uint8>& Output);

	/** Moves everything the child wrote to its standard error as text to Text. */
	void ReadErrorText(FString& Text);

	bool IsRunning();

	/** Sends SIGKILL and returns at once, from any thread. (SIGTERM would not do: a child blocked on a full pipe ignores it.) */
	void ForceStop();

	/** Ends the child, waits for it and closes its pipes. Waits briefly, so only the worker thread calls it. */
	void Kill();

private:
	FCriticalSection Lock;
	FProcHandle Handle;
	std::atomic<uint32> ProcessId{0};
	void* OutputRead = nullptr;
	void* OutputWrite = nullptr;
	void* ErrorRead = nullptr;
	void* ErrorWrite = nullptr;
};

/**
 * One live connection to a radio station: a worker thread runs ffmpeg to decode the stream to 48 kHz stereo floats
 * into a ring buffer (read by the audio thread) and curl with `Icy-MetaData: 1` to pick the song titles out of the
 * interleaved stream. Both children are restarted with a growing delay when the connection drops or stalls. Nothing
 * here ever blocks the game thread: RequestStop only flags the worker and kills the children.
 */
class FRadioStreamer : public TSharedFromThis<FRadioStreamer, ESPMode::ThreadSafe>
{
public:
	static constexpr int32 SampleRate = 48000;
	static constexpr int32 Channels = 2;

	/** Creates the streamer and starts its worker thread. */
	static TSharedRef<FRadioStreamer, ESPMode::ThreadSafe> Start(const FString& InStationName, const FString& InUrl);

	/** Asks the worker to end and kills the children at once; the worker cleans up in the background. */
	void RequestStop();

	/**
	 * Audio thread: fills Out with interleaved stereo samples and returns how many floats are real audio. The rest is
	 * zero: nothing plays while the buffer fills up and again after an underrun, until PrebufferSeconds are in.
	 */
	int32 ReadAudio(float* Out, int32 FloatCount);

	/** Seconds of audio waiting in the buffer. */
	float GetBufferedSeconds() const;

	/** Game thread: the newest title that has become due (it is held back by the buffer's length so it matches what is heard). */
	bool ConsumeTitle(FString& OutTitle);

	bool IsConnected() const { return bConnected; }
	const FString& GetStationName() const { return StationName; }

	~FRadioStreamer();

private:
	FRadioStreamer(const FString& InStationName, const FString& InUrl);

	struct FPendingTitle
	{
		double DueTime = 0.0;
		FString Title;
	};

	/** The worker thread: sessions with a pause that grows after failures, until stopped. */
	void Run();
	/** One connection from launch to drop; returns true if audio arrived (the next retry then starts quickly). */
	bool RunSession();
	/** Moves decoded audio from the pipe into the ring and notes when audio last arrived. */
	void PumpAudio(FRadioChildProcess& Decoder, TArray<uint8>& Pending, double& LastAudioTime, double LaunchTime, bool& bReceivedAudio);
	/** Feeds whatever curl has written so far to the ICY parser. */
	void PumpMetadata(FRadioChildProcess& Metadata);
	/** Moves whole floats from Pending into the ring as far as they fit; true if any moved. */
	bool WriteToRing(TArray<uint8>& Pending);
	/** Queues a new song title for the game thread, due when the audio that goes with it is played. */
	void PushTitle(const FString& Title);
	/** Handles curl output: first the HTTP headers, then the audio with title blocks. */
	void ParseIcyBytes(const uint8* Bytes, int32 Count);
	/** Takes one complete HTTP header block off the front of the curl output, as lines; false if it is not complete yet. */
	bool TakeHttpHeaderBlock(TArray<FString>& OutLines);
	/** Reads the header blocks (one per redirect) until the real response, and learns the metadata interval from it. */
	void ConsumeHttpHeaders();
	/** Splits the audio bytes after the headers from the title blocks that are inserted every MetaInt bytes. */
	void ParseIcyBody(const uint8* Bytes, int32 Count);
	/** Forgets the state of a previous connection's parser. */
	void ResetMetadataParser();
	/** The text of StreamTitle='...' in a metadata block. */
	FString ParseStreamTitle(const TArray<uint8>& Block) const;
	/** Sleeps in short steps; false if a stop was requested meanwhile. */
	bool SleepUnlessStopped(double Seconds) const;

	const FString StationName;
	const FString Url;
	FString FfmpegPath;
	FString CurlPath;
	int32 PrebufferFloats = 0;

	std::atomic<bool> bStopRequested{false};
	std::atomic<bool> bConnected{false};
	std::atomic<bool> bPlaying{false};

	// Single producer (worker) and single consumer (audio thread) ring of float samples.
	TArray<float> Ring;
	std::atomic<int64> WriteFloats{0};
	std::atomic<int64> ReadFloats{0};

	FRadioChildProcess Decoder;
	FRadioChildProcess Metadata;

	FCriticalSection TitleLock;
	TArray<FPendingTitle> PendingTitles;

	// Worker-only state of the ICY parser (curl output: HTTP headers, then audio with a title block every MetaInt bytes).
	TArray<uint8> HeaderBytes;
	bool bHeadersDone = false;
	int32 MetaInt = 0;
	int32 AudioUntilMeta = 0;
	int32 MetaBlockRemaining = 0;
	bool bReadingMetaLength = false;
	TArray<uint8> MetaBlock;
	FString LastTitle;
};
