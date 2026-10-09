#include "DrivingGameMode.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "CarPawn.h"
#include "AudioTest.h"
#include "DriveTest.h"
#include "GameFlow.h"
#include "LeafTest.h"
#include "RuleTest.h"
#include "StreamTest.h"
#include "TrafficTest.h"
#include "GameFramework/PlayerStart.h"
#include "SeatedVRPawn.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "EngineUtils.h"
#include "WorldStreamer.h"
#include "ContentStreaming.h"
#if WITH_EDITOR
#include "ShaderCompiler.h"
#endif

ADrivingGameMode::ADrivingGameMode()
{
	DefaultPawnClass = ACarPawn::StaticClass();
}

bool ADrivingGameMode::UseFreeCamera()
{
	FString Value;
	return FParse::Param(FCommandLine::Get(), TEXT("FreeCam")) || FParse::Value(FCommandLine::Get(), TEXT("Shots="), Value)
		|| FParse::Value(FCommandLine::Get(), TEXT("StreamTest="), Value) || FParse::Param(FCommandLine::Get(), TEXT("RuleTest"))
		|| FParse::Value(FCommandLine::Get(), TEXT("TrafficTest="), Value);
}

bool ADrivingGameMode::UsesFreeCameraPawn() const
{
	return UseFreeCamera() || GameFlow::IsFreeCameraLevel(GetWorld());
}

UClass* ADrivingGameMode::GetDefaultPawnClassForController_Implementation(AController* InController)
{
	return UsesFreeCameraPawn() ? ASeatedVRPawn::StaticClass() : Super::GetDefaultPawnClassForController_Implementation(InController);
}

FVector ADrivingGameMode::FindGroundBelow(const FVector& Location) const
{
	// First static surface below the player start (ignoring the player start's own capsule, pawns etc.).
	TArray<FHitResult> Hits;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(CarSpawnTrace), /*bTraceComplex=*/true);
	GetWorld()->LineTraceMultiByObjectType(Hits, Location, Location - FVector(0, 0, 8000), FCollisionObjectQueryParams(ECC_WorldStatic), Params);
	for (const FHitResult& Hit : Hits)
	{
		if (Hit.bBlockingHit && !Cast<APlayerStart>(Hit.GetActor()) && !Cast<APawn>(Hit.GetActor()))
		{
			return Hit.ImpactPoint;
		}
	}
	FHitResult Hit;
	if (GetWorld()->LineTraceSingleByObjectType(Hit, Location, Location - FVector(0, 0, 8000), FCollisionObjectQueryParams(ECC_WorldStatic), Params))
	{
		return Hit.ImpactPoint;
	}
	return Location - FVector(0, 0, 120); // player starts are placed at eye height (1.2 m) above the road
}

AWorldStreamer* ADrivingGameMode::FindWorldStreamer() const
{
	TActorIterator<AWorldStreamer> It(GetWorld());
	return It ? *It : nullptr;
}

AActor* ADrivingGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	// A generated world has no player start in the level: make one where its data says, and generate the ground
	// around it before anything is placed on it.
	AWorldStreamer* Streamer = FindWorldStreamer();
	FTransform Start;
	if (!Streamer || !Streamer->GetStartTransform(Start))
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}
	if (!GeneratedStart)
	{
		FActorSpawnParameters SpawnInfo;
		SpawnInfo.ObjectFlags |= RF_Transient;
		SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		GeneratedStart = GetWorld()->SpawnActor<APlayerStart>(Start.GetLocation(), Start.Rotator(), SpawnInfo);
		Streamer->LoadAroundBlocking(Start.GetLocation());
	}
	return GeneratedStart;
}

APawn* ADrivingGameMode::SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform)
{
	UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer);
	if (!PawnClass || !PawnClass->IsChildOf(ACarPawn::StaticClass()))
	{
		return Super::SpawnDefaultPawnAtTransform_Implementation(NewPlayer, SpawnTransform);
	}
	// The car stands on the road below the player start, level, facing the player start's direction.
	FActorSpawnParameters SpawnInfo;
	SpawnInfo.Instigator = GetInstigator();
	SpawnInfo.ObjectFlags |= RF_Transient;
	SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector Ground = FindGroundBelow(SpawnTransform.GetLocation());
	const float Yaw = SpawnTransform.Rotator().Yaw;
	ACarPawn* Car = GetWorld()->SpawnActor<ACarPawn>(PawnClass, Ground + FVector(0, 0, 50), FRotator(0, Yaw, 0), SpawnInfo);
	if (Car)
	{
		Car->PlaceCar(Ground, Yaw, /*bEngineRunning=*/true);
		UE_LOG(LogTemp, Log, TEXT("Spawned car on the ground at %s (player start %s), yaw %.1f"), *Ground.ToString(), *SpawnTransform.GetLocation().ToString(), Yaw);
	}
	return Car;
}

void ADrivingGameMode::BeginPlay()
{
	Super::BeginPlay();

	FParse::Value(FCommandLine::Get(), TEXT("ShotSettle="), ShotSettleSeconds);

	// Driver's view from the car seat (desktop render): -SeatShot [-ShotDelay=s] [-ShotName=x] -> Screenshots/<x>_seat.png
	// The delay lets the car settle on its springs; shaders and textures are then waited for as for -Shots.
	if (FParse::Param(FCommandLine::Get(), TEXT("SeatShot")) && !UseFreeCamera())
	{
		float Delay = 4.f;
		FParse::Value(FCommandLine::Get(), TEXT("ShotDelay="), Delay);
		FTimerHandle Handle;
		GetWorldTimerManager().SetTimer(Handle, [this]()
		{
			WaitForShadersAndTextures();
			FTimerHandle CaptureHandle;
			GetWorldTimerManager().SetTimer(CaptureHandle, this, &ADrivingGameMode::CaptureSeatShot, ShotSettleSeconds, false);
		}, Delay, false);
	}

	// Automated drivetrain test (see Scripts/drive_test.sh).
	if (FParse::Param(FCommandLine::Get(), TEXT("DriveTest")) && !UseFreeCamera())
	{
		GetWorld()->SpawnActor<ADriveTestRunner>();
	}

	// Scripted drive for the audio recordings (see Scripts/audio_test.sh).
	if (FParse::Param(FCommandLine::Get(), TEXT("AudioTest")) && !UseFreeCamera())
	{
		GetWorld()->SpawnActor<AAudioTestRunner>();
	}

	// Rule checker test on the real signals and roads of the region (see Scripts/rule_test.sh).
	if (FParse::Param(FCommandLine::Get(), TEXT("RuleTest")))
	{
		GetWorld()->SpawnActor<ARuleTestRunner>();
	}

	// AI traffic test: the free camera rides the lane graph while cars spawn around it (see Scripts/traffic_test.sh).
	FString TrafficTestMinutes;
	if (FParse::Value(FCommandLine::Get(), TEXT("TrafficTest="), TrafficTestMinutes))
	{
		GetWorld()->SpawnActor<ATrafficTestRunner>();
	}

	// Automated run through the generated world (see Scripts/stream_test.sh).
	FString StreamRoute;
	if (FParse::Value(FCommandLine::Get(), TEXT("StreamTest="), StreamRoute))
	{
		GetWorld()->SpawnActor<AStreamTestRunner>();
	}

	// Patch of fallen leaf cards for looking at the leaf assets (see LeafTest.h).
	if (FParse::Param(FCommandLine::Get(), TEXT("LeafTest")))
	{
		GetWorld()->SpawnActor<ALeafTestPatch>();
	}

	// Free camera with a parked car to look at (e.g. screenshots of the car model).
	if (UseFreeCamera() && FParse::Param(FCommandLine::Get(), TEXT("SpawnCar")))
	{
		if (AActor* Start = FindPlayerStart(nullptr))
		{
			const FVector Ground = FindGroundBelow(Start->GetActorLocation());
			const float Yaw = Start->GetActorRotation().Yaw;
			FActorSpawnParameters SpawnInfo;
			SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (ACarPawn* Car = GetWorld()->SpawnActor<ACarPawn>(ACarPawn::StaticClass(), Ground + FVector(0, 0, 50), FRotator(0, Yaw, 0), SpawnInfo))
			{
				Car->PlaceCar(Ground, Yaw, /*bEngineRunning=*/true);
			}
		}
	}

	// Headless performance capture (see Scripts/profile_gpu.sh):
	//   -ProfileGPUAfter=<seconds>  run "ProfileGPU" (per-pass GPU timings into the log) after warm-up, then quit.
	float ProfileDelay = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("ProfileGPUAfter="), ProfileDelay) && ProfileDelay > 0.f)
	{
		FTimerHandle Handle;
		GetWorldTimerManager().SetTimer(Handle, [this]()
		{
			GEngine->Exec(GetWorld(), TEXT("ProfileGPU"));
			FTimerHandle QuitHandle;
			GetWorldTimerManager().SetTimer(QuitHandle, [this]() { GEngine->Exec(GetWorld(), TEXT("quit")); }, 3.f, false);
		}, ProfileDelay, false);
	}

	// Headless screenshots (see Scripts/screenshot.sh):
	//   -Shots="x,y,z,pitch,yaw[,command|command...];..." (metres / degrees, world coords). The optional commands run
	//   before that shot, so one run can shoot several weathers or times of day (Weather.Override, Weather.SetTime).
	//   -ShotDelay=<seconds> before the first shot (the world itself is waited for, see SettleAndCapture).
	FString ShotSpec;
	FParse::Value(FCommandLine::Get(), TEXT("ShotName="), ShotName); // file name prefix, default "shot"
	if (FParse::Value(FCommandLine::Get(), TEXT("Shots="), ShotSpec, /*bShouldStopOnSeparator=*/false))
	{
		ShotSpec.ParseIntoArray(PendingShots, TEXT(";"));
		float Delay = 2.f;
		FParse::Value(FCommandLine::Get(), TEXT("ShotDelay="), Delay);
		FTimerHandle Handle;
		GetWorldTimerManager().SetTimer(Handle, this, &ADrivingGameMode::TakeNextShot, Delay, false);
	}
}

namespace DrivingGameModeShots
{
	/** Splits one -Shots entry into its five numbers and the console commands after them. */
	bool ParseShot(const FString& Entry, FVector& OutLocation, FRotator& OutRotation, TArray<FString>& OutCommands)
	{
		TArray<FString> Parts;
		Entry.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() < 5)
		{
			return false;
		}
		OutLocation = FVector(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2])) * 100.0;
		OutRotation = FRotator(FCString::Atof(*Parts[3]), FCString::Atof(*Parts[4]), 0.0);
		// Commands may contain commas themselves (Weather.Override A=1,B=2), so everything after the fifth comma is theirs.
		FString Rest = Entry;
		for (int32 Field = 0; Field < 5; ++Field)
		{
			FString Head;
			if (!Rest.Split(TEXT(","), &Head, &Rest))
			{
				Rest.Reset();
				break;
			}
		}
		Rest.ParseIntoArray(OutCommands, TEXT("|"));
		return true;
	}
}

void ADrivingGameMode::TakeNextShot()
{
	if (PendingShots.IsEmpty())
	{
		GEngine->Exec(GetWorld(), TEXT("quit"));
		return;
	}
	FVector Location;
	FRotator Rotation;
	TArray<FString> Commands;
	const bool bParsed = DrivingGameModeShots::ParseShot(PendingShots[0], Location, Rotation, Commands);
	PendingShots.RemoveAt(0);
	for (const FString& Command : Commands)
	{
		GEngine->Exec(GetWorld(), *Command.TrimStartAndEnd());
	}
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (bParsed && PC && PC->GetPawn())
	{
		PC->GetPawn()->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
		PC->SetControlRotation(Rotation);
		if (AWorldStreamer* Streamer = FindWorldStreamer())
		{
			Streamer->LoadAroundBlocking(Location);
		}
	}
	// A few frames at the new view first, so the renderer requests the shaders and textures it is missing.
	FTimerHandle SettleHandle;
	GetWorldTimerManager().SetTimer(SettleHandle, this, &ADrivingGameMode::SettleAndCapture, 0.3f, false);
}

void ADrivingGameMode::WaitForShadersAndTextures() const
{
	const double StartTime = FPlatformTime::Seconds();
#if WITH_EDITOR
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
#endif
	IStreamingManager::Get().StreamAllResources(10.f);
	UE_LOG(LogTemp, Log, TEXT("Shot %d: shaders and textures ready after %.1f s"), ShotIndex, FPlatformTime::Seconds() - StartTime);
}

void ADrivingGameMode::SettleAndCapture()
{
	WaitForShadersAndTextures();
	// Lumen's surface cache and TSR's history still need a moment of real frames.
	FTimerHandle CaptureHandle;
	GetWorldTimerManager().SetTimer(CaptureHandle, this, &ADrivingGameMode::CaptureShot, ShotSettleSeconds, false);
}

void ADrivingGameMode::CaptureShot()
{
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() /
		FString::Printf(TEXT("Screenshots/%s_%02d.png"), *ShotName, ShotIndex++));
	FScreenshotRequest::RequestScreenshot(File, /*bShowUI=*/!GameFlow::GetDebugMenuPage().IsEmpty(), /*bAddFilenameSuffix=*/false);
	// The screenshot is written at the end of the next frame.
	FTimerHandle NextHandle;
	GetWorldTimerManager().SetTimer(NextHandle, this, &ADrivingGameMode::TakeNextShot, 0.5f, false);
}

void ADrivingGameMode::CaptureSeatShot()
{
	FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() /
		FString::Printf(TEXT("Screenshots/%s_seat.png"), *ShotName)), /*bShowUI=*/!GameFlow::GetDebugMenuPage().IsEmpty(), /*bAddFilenameSuffix=*/false);
	FTimerHandle QuitHandle;
	GetWorldTimerManager().SetTimer(QuitHandle, [this]() { GEngine->Exec(GetWorld(), TEXT("quit")); }, 1.f, false);
}
