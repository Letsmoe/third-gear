#include "RadioStream.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "RadioSettings.h"
#include <csignal>
#include <sys/types.h>

DEFINE_LOG_CATEGORY(LogRadio);

namespace
{
constexpr int32 RingSeconds = 5;
constexpr double StallSeconds = 12.0;
constexpr double ConnectTimeoutSeconds = 20.0;
constexpr double WorkerPollSeconds = 0.01;
const TCHAR* ParentDeathWrapper = TEXT("/usr/bin/setpriv");

/** Case-insensitive value of a header line such as "icy-metaint: 16384", or an empty string. */
FString FindHeaderValue(const TArray<FString>& Lines, const FString& Name)
{
	const FString Prefix = Name + TEXT(":");
	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return Line.Mid(Prefix.Len()).TrimStartAndEnd();
		}
	}
	return FString();
}
}

// --- child process ---------------------------------------------------------------------------------------------------

bool FRadioChildProcess::Launch(const FString& Executable, const FString& Arguments)
{
	FScopeLock ScopeLock(&Lock);
	if (!FPlatformProcess::CreatePipe(OutputRead, OutputWrite) || !FPlatformProcess::CreatePipe(ErrorRead, ErrorWrite))
	{
		return false;
	}
	FString Program = Executable;
	FString FullArguments = Arguments;
	if (FPaths::FileExists(ParentDeathWrapper))
	{
		// The kernel kills the child when the game process dies, however it dies, so no orphan keeps streaming.
		Program = ParentDeathWrapper;
		FullArguments = FString::Printf(TEXT("--pdeathsig KILL %s %s"), *Executable, *Arguments);
	}
	uint32 NewProcessId = 0;
	Handle = FPlatformProcess::CreateProc(*Program, *FullArguments, /*bLaunchDetached=*/false, /*bLaunchHidden=*/true,
		/*bLaunchReallyHidden=*/true, &NewProcessId, 0, nullptr, OutputWrite, nullptr, ErrorWrite);
	ProcessId = Handle.IsValid() ? NewProcessId : 0;
	return Handle.IsValid();
}

void FRadioChildProcess::ForceStop()
{
	const uint32 Pid = ProcessId.load();
	if (Pid != 0)
	{
		::kill(static_cast<pid_t>(Pid), SIGKILL);
	}
}

bool FRadioChildProcess::ReadOutput(TArray<uint8>& Output)
{
	FScopeLock ScopeLock(&Lock);
	return OutputRead && FPlatformProcess::ReadPipeToArray(OutputRead, Output);
}

void FRadioChildProcess::ReadErrorText(FString& Text)
{
	FScopeLock ScopeLock(&Lock);
	TArray<uint8> Bytes;
	if (ErrorRead && FPlatformProcess::ReadPipeToArray(ErrorRead, Bytes))
	{
		Bytes.Add(0);
		Text = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData()));
	}
}

bool FRadioChildProcess::IsRunning()
{
	FScopeLock ScopeLock(&Lock);
	return Handle.IsValid() && FPlatformProcess::IsProcRunning(Handle);
}

void FRadioChildProcess::Kill()
{
	ForceStop();
	FScopeLock ScopeLock(&Lock);
	if (Handle.IsValid())
	{
		ProcessId = 0; // before the reaping: the number may be reused afterwards
		FPlatformProcess::CloseProc(Handle); // waits for the exit that ForceStop caused
		Handle.Reset();
	}
	FPlatformProcess::ClosePipe(OutputRead, OutputWrite);
	FPlatformProcess::ClosePipe(ErrorRead, ErrorWrite);
	OutputRead = OutputWrite = ErrorRead = ErrorWrite = nullptr;
}

// --- streamer ----------------------------------------------------------------------------------------------------------

FRadioStreamer::FRadioStreamer(const FString& InStationName, const FString& InUrl)
	: StationName(InStationName)
	, Url(InUrl)
{
	const URadioSettings* Settings = GetDefault<URadioSettings>();
	FfmpegPath = Settings->FfmpegPath;
	CurlPath = Settings->CurlPath;
	PrebufferFloats = FMath::RoundToInt(Settings->PrebufferSeconds * SampleRate) * Channels;
	Ring.SetNumZeroed(RingSeconds * SampleRate * Channels);
}

FRadioStreamer::~FRadioStreamer()
{
	RequestStop();
}

TSharedRef<FRadioStreamer, ESPMode::ThreadSafe> FRadioStreamer::Start(const FString& InStationName, const FString& InUrl)
{
	TSharedRef<FRadioStreamer, ESPMode::ThreadSafe> Streamer = MakeShareable(new FRadioStreamer(InStationName, InUrl));
	Async(EAsyncExecution::Thread, [Streamer]() { Streamer->Run(); });
	return Streamer;
}

void FRadioStreamer::RequestStop()
{
	bStopRequested = true;
	Decoder.ForceStop();
	Metadata.ForceStop();
}

float FRadioStreamer::GetBufferedSeconds() const
{
	const int64 Buffered = WriteFloats.load() - ReadFloats.load();
	return static_cast<float>(Buffered) / static_cast<float>(SampleRate * Channels);
}

int32 FRadioStreamer::ReadAudio(float* Out, int32 FloatCount)
{
	const int64 Available = WriteFloats.load(std::memory_order_acquire) - ReadFloats.load(std::memory_order_relaxed);
	if (!bPlaying && Available < PrebufferFloats)
	{
		FMemory::Memzero(Out, FloatCount * sizeof(float));
		return 0;
	}
	bPlaying = true;
	const int32 Count = static_cast<int32>(FMath::Min<int64>(Available, FloatCount));
	const int64 Start = ReadFloats.load(std::memory_order_relaxed);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Out[Index] = Ring[(Start + Index) % Ring.Num()];
	}
	ReadFloats.store(Start + Count, std::memory_order_release);
	if (Count < FloatCount)
	{
		FMemory::Memzero(Out + Count, (FloatCount - Count) * sizeof(float));
		bPlaying = false; // underrun: refill before playing on
	}
	return Count;
}

bool FRadioStreamer::ConsumeTitle(FString& OutTitle)
{
	FScopeLock ScopeLock(&TitleLock);
	const double Now = FPlatformTime::Seconds();
	bool bFound = false;
	while (!PendingTitles.IsEmpty() && PendingTitles[0].DueTime <= Now)
	{
		OutTitle = PendingTitles[0].Title;
		PendingTitles.RemoveAt(0);
		bFound = true;
	}
	return bFound;
}

void FRadioStreamer::PushTitle(const FString& Title)
{
	if (Title == LastTitle)
	{
		return;
	}
	LastTitle = Title;
	UE_LOG(LogRadio, Log, TEXT("%s: now playing \"%s\""), *StationName, *Title);
	FScopeLock ScopeLock(&TitleLock);
	FPendingTitle& Pending = PendingTitles.AddDefaulted_GetRef();
	Pending.DueTime = FPlatformTime::Seconds() + GetBufferedSeconds();
	Pending.Title = Title;
}

bool FRadioStreamer::SleepUnlessStopped(double Seconds) const
{
	const double End = FPlatformTime::Seconds() + Seconds;
	while (!bStopRequested && FPlatformTime::Seconds() < End)
	{
		FPlatformProcess::Sleep(0.05f);
	}
	return !bStopRequested;
}

void FRadioStreamer::Run()
{
	double RetrySeconds = 1.0;
	while (!bStopRequested)
	{
		const bool bReceivedAudio = RunSession();
		bConnected = false;
		Decoder.Kill();
		Metadata.Kill();
		if (bStopRequested)
		{
			break;
		}
		RetrySeconds = bReceivedAudio ? 1.0 : FMath::Min(RetrySeconds * 2.0, 15.0);
		UE_LOG(LogRadio, Warning, TEXT("%s: connection lost, retrying in %.0f s"), *StationName, RetrySeconds);
		if (!SleepUnlessStopped(RetrySeconds))
		{
			break;
		}
	}
	UE_LOG(LogRadio, Log, TEXT("%s: stream closed"), *StationName);
}

bool FRadioStreamer::RunSession()
{
	const double LaunchTime = FPlatformTime::Seconds();
	const FString DecoderArguments = FString::Printf(TEXT("-hide_banner -nostdin -loglevel warning -reconnect 1 -reconnect_streamed 1 ")
		TEXT("-reconnect_delay_max 4 -probesize 65536 -analyzeduration 1000000 -i \"%s\" -vn -ac 2 -ar 48000 -f f32le pipe:1"), *Url);
	const FString MetadataArguments = FString::Printf(TEXT("-sS -L -i -A ThirdGear -H \"Icy-MetaData: 1\" --connect-timeout 10 -y 30 -Y 1 \"%s\""), *Url);
	if (!Decoder.Launch(FfmpegPath, DecoderArguments))
	{
		UE_LOG(LogRadio, Error, TEXT("%s: could not start \"%s\" (is ffmpeg installed?)"), *StationName, *FfmpegPath);
		return false;
	}
	ResetMetadataParser();
	if (!Metadata.Launch(CurlPath, MetadataArguments))
	{
		UE_LOG(LogRadio, Warning, TEXT("%s: could not start \"%s\", no song titles"), *StationName, *CurlPath);
	}

	TArray<uint8> Pending;
	bool bReceivedAudio = false;
	double LastAudioTime = LaunchTime;
	while (!bStopRequested)
	{
		PumpAudio(Decoder, Pending, LastAudioTime, LaunchTime, bReceivedAudio);
		PumpMetadata(Metadata);
		FString ErrorText;
		Decoder.ReadErrorText(ErrorText);
		if (!ErrorText.IsEmpty())
		{
			UE_LOG(LogRadio, Warning, TEXT("%s: ffmpeg says: %s"), *StationName, *ErrorText.TrimStartAndEnd());
		}
		const double Now = FPlatformTime::Seconds();
		if (!Decoder.IsRunning())
		{
			break;
		}
		const double SilentSeconds = Now - LastAudioTime;
		if (SilentSeconds > (bReceivedAudio ? StallSeconds : ConnectTimeoutSeconds))
		{
			UE_LOG(LogRadio, Warning, TEXT("%s: no audio for %.0f s"), *StationName, SilentSeconds);
			break;
		}
		FPlatformProcess::Sleep(WorkerPollSeconds);
	}
	return bReceivedAudio;
}

void FRadioStreamer::PumpAudio(FRadioChildProcess& Child, TArray<uint8>& Pending, double& LastAudioTime, double LaunchTime, bool& bReceivedAudio)
{
	// Only read from the pipe once the previous bytes are in the ring: a full ring makes ffmpeg wait, which paces the stream.
	if (Pending.Num() < static_cast<int32>(sizeof(float)))
	{
		TArray<uint8> Fresh;
		if (Child.ReadOutput(Fresh))
		{
			Pending.Append(Fresh);
		}
	}
	if (Pending.Num() < static_cast<int32>(sizeof(float)))
	{
		return;
	}
	if (!bReceivedAudio)
	{
		bReceivedAudio = true;
		bConnected = true;
		UE_LOG(LogRadio, Log, TEXT("%s: connected, first audio after %.2f s"), *StationName, FPlatformTime::Seconds() - LaunchTime);
	}
	if (WriteToRing(Pending))
	{
		LastAudioTime = FPlatformTime::Seconds();
	}
}

bool FRadioStreamer::WriteToRing(TArray<uint8>& Pending)
{
	const int64 Write = WriteFloats.load(std::memory_order_relaxed);
	const int64 Free = Ring.Num() - (Write - ReadFloats.load(std::memory_order_acquire));
	const int32 FloatsPending = Pending.Num() / sizeof(float);
	const int32 Count = static_cast<int32>(FMath::Min<int64>(Free, FloatsPending));
	if (Count <= 0)
	{
		return false;
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		float Sample;
		FMemory::Memcpy(&Sample, Pending.GetData() + Index * sizeof(float), sizeof(float));
		Ring[(Write + Index) % Ring.Num()] = Sample;
	}
	WriteFloats.store(Write + Count, std::memory_order_release);
	Pending.RemoveAt(0, Count * sizeof(float), EAllowShrinking::No);
	return true;
}

// --- ICY metadata via curl ----------------------------------------------------------------------------------------

void FRadioStreamer::ResetMetadataParser()
{
	HeaderBytes.Reset();
	bHeadersDone = false;
	MetaInt = 0;
	AudioUntilMeta = 0;
	MetaBlockRemaining = 0;
	bReadingMetaLength = false;
	MetaBlock.Reset();
}

void FRadioStreamer::PumpMetadata(FRadioChildProcess& Child)
{
	TArray<uint8> Bytes;
	if (Child.ReadOutput(Bytes))
	{
		ParseIcyBytes(Bytes.GetData(), Bytes.Num());
	}
}

bool FRadioStreamer::TakeHttpHeaderBlock(TArray<FString>& OutLines)
{
	const uint8 Terminator[] = {'\r', '\n', '\r', '\n'};
	for (int32 Index = 0; Index + 4 <= HeaderBytes.Num(); ++Index)
	{
		if (FMemory::Memcmp(HeaderBytes.GetData() + Index, Terminator, 4) != 0)
		{
			continue;
		}
		TArray<uint8> Block(HeaderBytes.GetData(), Index);
		Block.Add(0);
		FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Block.GetData()))).ParseIntoArrayLines(OutLines);
		HeaderBytes.RemoveAt(0, Index + 4, EAllowShrinking::No);
		return true;
	}
	return false;
}

void FRadioStreamer::ConsumeHttpHeaders()
{
	TArray<FString> Lines;
	while (!bHeadersDone && TakeHttpHeaderBlock(Lines))
	{
		const bool bRedirect = !Lines.IsEmpty() && Lines[0].Contains(TEXT(" 30"));
		if (bRedirect)
		{
			Lines.Reset(); // curl prints the headers of every hop; the real response follows
			continue;
		}
		bHeadersDone = true;
		MetaInt = FCString::Atoi(*FindHeaderValue(Lines, TEXT("icy-metaint")));
		AudioUntilMeta = MetaInt;
		UE_LOG(LogRadio, Log, TEXT("%s: %s, ICY metadata interval %d"), *StationName, Lines.IsEmpty() ? TEXT("?") : *Lines[0], MetaInt);
	}
}

void FRadioStreamer::ParseIcyBytes(const uint8* Bytes, int32 Count)
{
	if (bHeadersDone)
	{
		ParseIcyBody(Bytes, Count);
		return;
	}
	HeaderBytes.Append(Bytes, Count);
	ConsumeHttpHeaders();
	if (bHeadersDone)
	{
		const TArray<uint8> Body = MoveTemp(HeaderBytes);
		HeaderBytes.Reset();
		ParseIcyBody(Body.GetData(), Body.Num());
	}
}

void FRadioStreamer::ParseIcyBody(const uint8* Bytes, int32 Count)
{
	if (MetaInt <= 0)
	{
		return; // this station sends no titles
	}
	int32 Position = 0;
	while (Position < Count)
	{
		if (AudioUntilMeta > 0)
		{
			const int32 Skip = FMath::Min(AudioUntilMeta, Count - Position);
			AudioUntilMeta -= Skip;
			Position += Skip;
			bReadingMetaLength = AudioUntilMeta == 0;
		}
		else if (bReadingMetaLength)
		{
			MetaBlockRemaining = Bytes[Position++] * 16;
			bReadingMetaLength = false;
			MetaBlock.Reset();
			AudioUntilMeta = MetaBlockRemaining == 0 ? MetaInt : 0;
		}
		else
		{
			const int32 Take = FMath::Min(MetaBlockRemaining, Count - Position);
			MetaBlock.Append(Bytes + Position, Take);
			MetaBlockRemaining -= Take;
			Position += Take;
			if (MetaBlockRemaining == 0)
			{
				PushTitle(ParseStreamTitle(MetaBlock));
				AudioUntilMeta = MetaInt;
			}
		}
	}
}

FString FRadioStreamer::ParseStreamTitle(const TArray<uint8>& Block) const
{
	TArray<uint8> Terminated = Block;
	Terminated.Add(0);
	const FString Text = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Terminated.GetData()));
	const FString Key = TEXT("StreamTitle='");
	const int32 Start = Text.Find(Key);
	if (Start == INDEX_NONE)
	{
		return FString();
	}
	const int32 ValueStart = Start + Key.Len();
	const int32 End = Text.Find(TEXT("';"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ValueStart);
	return (End == INDEX_NONE ? Text.Mid(ValueStart) : Text.Mid(ValueStart, End - ValueStart)).TrimStartAndEnd();
}
