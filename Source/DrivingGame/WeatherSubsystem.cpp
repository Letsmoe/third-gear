#include "WeatherSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "IsobarClimate.h"
#include "IsobarElevationSource.h"
#include "IsobarTime.h"
#include "IsobarWeather.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeather, Log, All);

namespace
{
	/** The project's map origin (Tools/osmimport geo.py): Bergedorf, near the Schloss. */
	constexpr double MapOriginLatitudeDegrees = 53.4880;
	constexpr double MapOriginLongitudeDegrees = 10.2120;
	constexpr int32 MapEpsgCode = 25832;

	/** Level ground at the climate station's altitude, so the flat map changes nothing about the climate. */
	constexpr double FlatGroundMeters = 11.0;

	/** Terrain tiles are kept this far around the viewer, and dropped beyond twice that. */
	constexpr double TerrainReachMeters = 1500.0;

	constexpr double CentimetresPerMetre = 100.0;

	/**
	 * Unreal centimetres, X east and Y south, to Isobar metres, X east and Y north.
	 * Z is already height above NHN, the datum the climate's station altitude uses.
	 */
	FVector2d ToIsobarMeters(const FVector& WorldLocation)
	{
		// Subtracted from zero rather than negated, so the origin reads 0 and not -0.
		return FVector2d(WorldLocation.X / CentimetresPerMetre, 0.0 - WorldLocation.Y / CentimetresPerMetre);
	}

	double CommandLineDouble(const TCHAR* Key, double Default)
	{
		double Value = Default;
		FParse::Value(FCommandLine::Get(), Key, Value);
		return Value;
	}

	FIsobarWeatherSetup MakeHamburgSetup(FString& OutError)
	{
		FIsobarWeatherSetup Setup;
		IsobarClimatePresets::LoadPreset(IsobarClimatePresets::TemperateMaritime, Setup.Climate, OutError);
		Setup.Georeference.LatitudeDegrees = MapOriginLatitudeDegrees;
		Setup.Georeference.LongitudeDegrees = MapOriginLongitudeDegrees;
		Setup.Georeference.SourceEpsgCode = MapEpsgCode;
		Setup.Elevation = MakeShared<FIsobarFlatElevationSource>(FlatGroundMeters);
		Setup.MasterSeed = static_cast<uint32>(CommandLineDouble(TEXT("WeatherSeed="), 1.0));
		return Setup;
	}

	/** Start time from the command line: a 1-based day of the year and an hour. */
	double StartSecondsFromCommandLine()
	{
		const int32 Day = FMath::Clamp(static_cast<int32>(CommandLineDouble(TEXT("WeatherDay="), 172.0)), 1, IsobarDaysPerYear);
		const double Hour = FMath::Clamp(CommandLineDouble(TEXT("WeatherHour="), 12.0), 0.0, 23.99);
		return static_cast<double>(IsobarSecondsAt(Day - 1, Hour));
	}

	UWeatherSubsystem* FindSubsystem(UWorld* World)
	{
		if (!World)
		{
			return nullptr;
		}
		return World->GetSubsystem<UWeatherSubsystem>();
	}

	/** `Weather.Print`: the weather where the player is, and the clock. */
	void PrintWeather(const TArray<FString>& Args, UWorld* World, FOutputDevice& Output)
	{
		const UWeatherSubsystem* Subsystem = FindSubsystem(World);
		FVector Location;
		FIsobarPointWeather Here;
		if (!Subsystem || !Subsystem->FindViewerLocation(Location) || !Subsystem->SampleAt(Location, Here))
		{
			Output.Log(TEXT("No weather here: needs a game world with a player"));
			return;
		}

		const FIsobarCalendar Calendar = IsobarCalendarAt(Subsystem->GetWeatherSeconds());
		const FVector2d Meters = ToIsobarMeters(Location);
		Output.Logf(TEXT("Weather on day %d at %02d:%02d solar, %.0f m east and %.0f m north of the origin, %.0f m above NHN"),
			Calendar.DayOfYear + 1, FMath::FloorToInt32(Calendar.GetHourOfDay()),
			FMath::FloorToInt32(FMath::Fmod(Calendar.GetHourOfDay(), 1.0) * 60.0), Meters.X, Meters.Y, Location.Z / CentimetresPerMetre);
		TArray<FString> Lines;
		IsobarDescribePointWeather(Here).ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			Output.Log(Line);
		}
	}

	/** `Weather.Skip <hours>`: moves the clock forward. */
	void SkipWeather(const TArray<FString>& Args, UWorld* World, FOutputDevice& Output)
	{
		UWeatherSubsystem* Subsystem = FindSubsystem(World);
		if (!Subsystem || Args.Num() < 1)
		{
			Output.Log(TEXT("Usage: Weather.Skip <hours>"));
			return;
		}
		Subsystem->SkipHours(FCString::Atod(*Args[0]));
		PrintWeather(Args, World, Output);
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice PrintCommand(
		TEXT("Weather.Print"), TEXT("Prints the weather at the car or camera."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&PrintWeather));

	FAutoConsoleCommandWithWorldArgsAndOutputDevice SkipCommand(
		TEXT("Weather.Skip"), TEXT("Weather.Skip <hours>: advances the weather clock and prints the weather."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&SkipWeather));
}

bool UWeatherSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld();
}

void UWeatherSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	FString Error;
	FIsobarWeatherSetup Setup = MakeHamburgSetup(Error);
	if (!Error.IsEmpty())
	{
		UE_LOG(LogWeather, Error, TEXT("No weather: %s"), *Error);
		return;
	}

	WeatherSeconds = StartSecondsFromCommandLine();
	TimeScale = CommandLineDouble(TEXT("WeatherTimeScale="), 1.0);
	Setup.StartSeconds = static_cast<int64>(WeatherSeconds);
	Weather = MakeUnique<FIsobarWeather>(Setup);
	KeepTerrainAroundViewer();
	UE_LOG(LogWeather, Log, TEXT("Weather: %s, seed %u"), *Setup.Climate.Name, Setup.MasterSeed);
}

void UWeatherSubsystem::Deinitialize()
{
	Weather.Reset();
	Super::Deinitialize();
}

void UWeatherSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Weather)
	{
		return;
	}
	WeatherSeconds += DeltaTime * TimeScale;
	Weather->AdvanceTo(WeatherSeconds);
	KeepTerrainAroundViewer();
}

TStatId UWeatherSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWeatherSubsystem, STATGROUP_Tickables);
}

bool UWeatherSubsystem::SampleAt(const FVector& WorldLocation, FIsobarPointWeather& OutWeather) const
{
	if (!Weather)
	{
		return false;
	}
	OutWeather = Weather->SampleAt(ToIsobarMeters(WorldLocation), WorldLocation.Z / CentimetresPerMetre, WeatherSeconds);
	return true;
}

void UWeatherSubsystem::SkipHours(double Hours)
{
	if (!Weather)
	{
		return;
	}
	WeatherSeconds += FMath::Max(0.0, Hours) * 3600.0;
	Weather->AdvanceTo(WeatherSeconds);
}

double UWeatherSubsystem::GetWeatherSeconds() const
{
	return WeatherSeconds;
}

bool UWeatherSubsystem::FindViewerLocation(FVector& OutLocation) const
{
	const APlayerController* Player = GetWorld()->GetFirstPlayerController();
	if (!Player)
	{
		return false;
	}
	if (const APawn* Pawn = Player->GetPawn())
	{
		OutLocation = Pawn->GetActorLocation();
		return true;
	}
	if (Player->PlayerCameraManager)
	{
		OutLocation = Player->PlayerCameraManager->GetCameraLocation();
		return true;
	}
	return false;
}

void UWeatherSubsystem::KeepTerrainAroundViewer()
{
	FVector Location;
	if (!FindViewerLocation(Location))
	{
		return;
	}
	const FVector2d Centre = ToIsobarMeters(Location);
	Weather->GetTerrain().EnsureTilesAround(Centre, TerrainReachMeters);
	Weather->GetTerrain().ReleaseTilesBeyond(Centre, 2.0 * TerrainReachMeters);
}
