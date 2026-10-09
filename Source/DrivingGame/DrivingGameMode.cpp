#include "DrivingGameMode.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "CarPawn.h"
#include "DriveTest.h"
#include "GameFlow.h"
#include "StreamTest.h"
#include "GameFramework/PlayerStart.h"
#include "SeatedVRPawn.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "EngineUtils.h"
#include "WorldStreamer.h"

ADrivingGameMode::ADrivingGameMode()
{
	DefaultPawnClass = ACarPawn::StaticClass();
}

bool ADrivingGameMode::UseFreeCamera()
{
	FString Value;
	return FParse::Param(FCommandLine::Get(), TEXT("FreeCam")) || FParse::Value(FCommandLine::Get(), TEXT("Shots="), Value)
		|| FParse::Value(FCommandLine::Get(), TEXT("StreamTest="), Value);
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
	GetWorld()->LineTraceMultiByObjectType(Hits, Location, Location - FVector(0, 0, 1000), FCollisionObjectQueryParams(ECC_WorldStatic), Params);
	for (const FHitResult& Hit : Hits)
	{
		if (Hit.bBlockingHit && !Cast<APlayerStart>(Hit.GetActor()) && !Cast<APawn>(Hit.GetActor()))
		{
			return Hit.ImpactPoint;
		}
	}
	FHitResult Hit;
	if (GetWorld()->LineTraceSingleByObjectType(Hit, Location, Location - FVector(0, 0, 1000), FCollisionObjectQueryParams(ECC_WorldStatic), Params))
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

	// Driver's view from the car seat (desktop render): -SeatShot [-ShotDelay=s] [-ShotName=x] -> Screenshots/<x>_seat.png
	if (FParse::Param(FCommandLine::Get(), TEXT("SeatShot")) && !UseFreeCamera())
	{
		float Delay = 12.f;
		FParse::Value(FCommandLine::Get(), TEXT("ShotDelay="), Delay);
		FTimerHandle Handle;
		GetWorldTimerManager().SetTimer(Handle, [this]()
		{
			FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() /
				FString::Printf(TEXT("Screenshots/%s_seat.png"), *ShotName)), /*bShowUI=*/!GameFlow::GetDebugMenuPage().IsEmpty(), /*bAddFilenameSuffix=*/false);
			FTimerHandle QuitHandle;
			GetWorldTimerManager().SetTimer(QuitHandle, [this]() { GEngine->Exec(GetWorld(), TEXT("quit")); }, 2.f, false);
		}, Delay, false);
	}

	// Automated drivetrain test (see Scripts/drive_test.sh).
	if (FParse::Param(FCommandLine::Get(), TEXT("DriveTest")) && !UseFreeCamera())
	{
		GetWorld()->SpawnActor<ADriveTestRunner>();
	}

	// Automated run through the generated world (see Scripts/stream_test.sh).
	FString StreamRoute;
	if (FParse::Value(FCommandLine::Get(), TEXT("StreamTest="), StreamRoute))
	{
		GetWorld()->SpawnActor<AStreamTestRunner>();
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
	//   -Shots="x,y,z,pitch,yaw;..." (metres / degrees, world coords; z omitted = keep) -ShotDelay=<seconds>
	FString ShotSpec;
	FParse::Value(FCommandLine::Get(), TEXT("ShotName="), ShotName); // file name prefix, default "shot"
	if (FParse::Value(FCommandLine::Get(), TEXT("Shots="), ShotSpec, /*bShouldStopOnSeparator=*/false))
	{
		ShotSpec.ParseIntoArray(PendingShots, TEXT(";"));
		float Delay = 15.f;
		FParse::Value(FCommandLine::Get(), TEXT("ShotDelay="), Delay);
		FTimerHandle Handle;
		GetWorldTimerManager().SetTimer(Handle, this, &ADrivingGameMode::TakeNextShot, Delay, false);
	}
}

void ADrivingGameMode::TakeNextShot()
{
	if (PendingShots.IsEmpty())
	{
		GEngine->Exec(GetWorld(), TEXT("quit"));
		return;
	}
	TArray<FString> Parts;
	PendingShots[0].ParseIntoArray(Parts, TEXT(","));
	PendingShots.RemoveAt(0);
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (PC && PC->GetPawn() && Parts.Num() >= 5)
	{
		const FVector Location(FCString::Atof(*Parts[0]) * 100.0, FCString::Atof(*Parts[1]) * 100.0, FCString::Atof(*Parts[2]) * 100.0);
		const FRotator Rotation(FCString::Atof(*Parts[3]), FCString::Atof(*Parts[4]), 0.0);
		PC->GetPawn()->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
		PC->SetControlRotation(Rotation);
		if (AWorldStreamer* Streamer = FindWorldStreamer())
		{
			Streamer->LoadAroundBlocking(Location);
		}
	}
	// Give streaming/Lumen a moment to settle at the new viewpoint, then capture.
	FTimerHandle CaptureHandle;
	GetWorldTimerManager().SetTimer(CaptureHandle, [this]()
	{
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() /
			FString::Printf(TEXT("Screenshots/%s_%02d.png"), *ShotName, ShotIndex++));
		FScreenshotRequest::RequestScreenshot(File, /*bShowUI=*/!GameFlow::GetDebugMenuPage().IsEmpty(), /*bAddFilenameSuffix=*/false);
		FTimerHandle NextHandle;
		GetWorldTimerManager().SetTimer(NextHandle, this, &ADrivingGameMode::TakeNextShot, 2.f, false);
	}, 4.f, false);
}
