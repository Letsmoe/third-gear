#include "GameFlow.h"

#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace GameFlow
{
const TCHAR* const StreamedMapPath = TEXT("/Game/Maps/Streamed");

namespace
{
/** Command-line switches of the automated runs and tools; any of them skips the start menu. */
bool HasBatchSwitch()
{
	const TCHAR* CommandLine = FCommandLine::Get();
	FString IgnoredValue;
	return FParse::Param(CommandLine, TEXT("NoMenu")) || FParse::Param(CommandLine, TEXT("SeatShot")) || FParse::Param(CommandLine, TEXT("DriveTest"))
		|| FParse::Param(CommandLine, TEXT("SpawnCar")) || FParse::Param(CommandLine, TEXT("FreeCam")) || FParse::Param(CommandLine, TEXT("benchmark"))
		|| FParse::Value(CommandLine, TEXT("Shots="), IgnoredValue) || FParse::Value(CommandLine, TEXT("StreamTest="), IgnoredValue)
		|| FParse::Value(CommandLine, TEXT("ProfileGPUAfter="), IgnoredValue);
}

bool HasUrlOption(const UWorld* World, const TCHAR* Option)
{
	return World && World->URL.HasOption(Option);
}
}

bool IsBatchLaunch()
{
	return FApp::IsUnattended() || HasBatchSwitch();
}

bool IsStartMenuLevel(const UWorld* World)
{
	if (HasUrlOption(World, TEXT("Menu")) || GetDebugMenuPage() == TEXT("Main"))
	{
		return true;
	}
	const bool bMenuDeclined = HasUrlOption(World, TEXT("Drive")) || HasUrlOption(World, TEXT("FreeCam"));
	return !bMenuDeclined && !IsBatchLaunch() && GetDebugMenuPage().IsEmpty();
}

bool IsFreeCameraLevel(const UWorld* World)
{
	return IsStartMenuLevel(World) || HasUrlOption(World, TEXT("FreeCam"));
}

FString GetDebugMenuPage()
{
	FString Page;
	FParse::Value(FCommandLine::Get(), TEXT("MenuPage="), Page);
	return Page;
}
}
