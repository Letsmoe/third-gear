#include "StreamTest.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogStreamTest, Log, All);

namespace
{
constexpr double CameraHeightCm = 200.0;
constexpr float HitchMilliseconds = 33.3f;
}

AStreamTestRunner::AStreamTestRunner()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
}

void AStreamTestRunner::BeginPlay()
{
	Super::BeginPlay();
	FString Route;
	FParse::Value(FCommandLine::Get(), TEXT("StreamTest="), Route, /*bShouldStopOnSeparator=*/false);
	TArray<FString> Parts;
	Route.ParseIntoArray(Parts, TEXT(","));
	if (Parts.Num() != 4)
	{
		UE_LOG(LogStreamTest, Error, TEXT("STREAMTEST needs -StreamTest=\"x0,y0,x1,y1\" in metres"));
		Finish();
		return;
	}
	RouteStart = FVector2D(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1])) * 100.0;
	RouteEnd = FVector2D(FCString::Atod(*Parts[2]), FCString::Atod(*Parts[3])) * 100.0;
	float SpeedKmh = 100.f;
	FParse::Value(FCommandLine::Get(), TEXT("StreamSpeed="), SpeedKmh);
	SpeedCmPerSecond = SpeedKmh / 3.6 * 100.0;
	LastGroundZ = 0.0;
	UE_LOG(LogStreamTest, Display, TEXT("STREAMTEST route %s to %s at %.0f km/h"), *(RouteStart / 100.0).ToString(), *(RouteEnd / 100.0).ToString(), SpeedKmh);
}

double AStreamTestRunner::GroundHeightAt(const FVector2D& Point)
{
	FHitResult Hit;
	const FVector From(Point, 50000.0);
	const FVector To(Point, -10000.0);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(StreamTestGround), /*bTraceComplex=*/true);
	if (APlayerController* Controller = GetWorld()->GetFirstPlayerController(); Controller && Controller->GetPawn())
	{
		Params.AddIgnoredActor(Controller->GetPawn());
	}
	if (GetWorld()->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Params))
	{
		LastGroundZ = Hit.ImpactPoint.Z;
	}
	return LastGroundZ;
}

void AStreamTestRunner::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (!Controller || !Controller->GetPawn() || SpeedCmPerSecond <= 0.0)
	{
		return;
	}
	const FVector2D Direction = (RouteEnd - RouteStart).GetSafeNormal();
	if (WarmupSeconds > 0.f)
	{
		WarmupSeconds -= DeltaSeconds; // let the start area finish loading and shaders settle
	}
	else
	{
		FrameMilliseconds.Add(DeltaSeconds * 1000.f);
		if (DeltaSeconds * 1000.f > HitchMilliseconds)
		{
			UE_LOG(LogStreamTest, Display, TEXT("STREAMTEST hitch %.1f ms at %.0f m"), DeltaSeconds * 1000.f, Travelled / 100.0);
		}
		Travelled += SpeedCmPerSecond * DeltaSeconds;
	}
	const double Length = FVector2D::Distance(RouteStart, RouteEnd);
	if (Travelled >= Length)
	{
		Finish();
		return;
	}
	const FVector2D Point = RouteStart + Direction * Travelled;
	Controller->GetPawn()->SetActorLocation(FVector(Point, GroundHeightAt(Point) + CameraHeightCm), false, nullptr, ETeleportType::TeleportPhysics);
	Controller->SetControlRotation(FRotator(-3.0, FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X)), 0.0));
}

void AStreamTestRunner::Finish()
{
	SetActorTickEnabled(false);
	TArray<float> Sorted = FrameMilliseconds;
	Sorted.Sort();
	if (!Sorted.IsEmpty())
	{
		double Sum = 0.0;
		int32 Hitches = 0;
		for (const float Milliseconds : Sorted)
		{
			Sum += Milliseconds;
			Hitches += Milliseconds > HitchMilliseconds ? 1 : 0;
		}
		const auto Percentile = [&](double Fraction) { return Sorted[FMath::Min(Sorted.Num() - 1, int32(Sorted.Num() * Fraction))]; };
		UE_LOG(LogStreamTest, Display, TEXT("STREAMTEST %d frames over %.0f m: average %.1f ms, median %.1f ms, 99th percentile %.1f ms, worst %.1f ms, %d frames over %.0f ms"),
			Sorted.Num(), Travelled / 100.0, Sum / Sorted.Num(), Percentile(0.5), Percentile(0.99), Sorted.Last(), Hitches, HitchMilliseconds);
	}
	GEngine->Exec(GetWorld(), TEXT("quit"));
}
