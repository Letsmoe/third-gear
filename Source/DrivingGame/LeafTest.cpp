#include "LeafTest.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogLeafTest, Log, All);

namespace
{
const TCHAR* const LeafShapeNames[] = {TEXT("Cup"), TEXT("Fold"), TEXT("Wave")};
constexpr float HeapRadiusFraction = 0.25f;
constexpr float HeapShare = 0.3f;
constexpr float HeapHeightCm = 2.5f;
constexpr float SurfaceLiftCm = 0.2f;
constexpr float MaximumTiltDegrees = 14.f;
constexpr float MinimumScale = 0.7f;
constexpr float MaximumScale = 1.25f;
}

ALeafTestPatch::ALeafTestPatch()
{
	PrimaryActorTick.bCanEverTick = false;
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

void ALeafTestPatch::BeginPlay()
{
	Super::BeginPlay();

	FString AtText;
	if (FParse::Value(FCommandLine::Get(), TEXT("LeafTestAt="), AtText, /*bShouldStopOnSeparator=*/false))
	{
		TArray<FString> Parts;
		AtText.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 2)
		{
			CentreMetres = FVector2D(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]));
		}
	}
	else
	{
		const TActorIterator<APlayerStart> Start(GetWorld());
		if (Start)
		{
			CentreMetres = FVector2D(Start->GetActorLocation().X, Start->GetActorLocation().Y) / 100.f;
		}
	}
	FParse::Value(FCommandLine::Get(), TEXT("LeafCount="), LeafCount);
	FParse::Value(FCommandLine::Get(), TEXT("LeafRadius="), RadiusMetres);
	int32 NaniteFlag = 0;
	FParse::Value(FCommandLine::Get(), TEXT("LeafNanite="), NaniteFlag);
	bUseNanite = NaniteFlag != 0;
	int32 ShadowFlag = 1;
	FParse::Value(FCommandLine::Get(), TEXT("LeafShadows="), ShadowFlag);
	bCastShadows = ShadowFlag != 0;
	Random.Initialize(41);

	for (const TCHAR* ShapeName : LeafShapeNames)
	{
		const FString Path = FString::Printf(TEXT("/Game/Leaves/SM_LeafCard_%s%s.SM_LeafCard_%s%s"), ShapeName,
			bUseNanite ? TEXT("_Nanite") : TEXT(""), ShapeName, bUseNanite ? TEXT("_Nanite") : TEXT(""));
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path);
		if (!Mesh)
		{
			UE_LOG(LogLeafTest, Error, TEXT("Leaf mesh %s missing (run Scripts/create_leaf_assets.py)"), *Path);
			continue;
		}
		UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(this);
		Component->SetStaticMesh(Mesh);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCastShadow(bCastShadows);
		Component->RegisterComponent();
		Component->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
		ShapeComponents.Add(Component);
	}
	GetWorldTimerManager().SetTimer(RetryHandle, this, &ALeafTestPatch::TryScatter, 1.f, true, 2.f);
}

bool ALeafTestPatch::TraceGround(const FVector& Location, FVector& OutPoint, FVector& OutNormal) const
{
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LeafGround), false, this);
	const FVector Start(Location.X, Location.Y, 20000.f);
	const FVector End(Location.X, Location.Y, -5000.f);
	if (!GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		return false;
	}
	OutPoint = Hit.ImpactPoint;
	OutNormal = Hit.ImpactNormal;
	return true;
}

void ALeafTestPatch::TryScatter()
{
	FVector Point, Normal;
	if (!TraceGround(FVector(CentreMetres.X * 100.f, CentreMetres.Y * 100.f, 0.f), Point, Normal))
	{
		return; // The streamed ground has no collision yet.
	}
	GetWorldTimerManager().ClearTimer(RetryHandle);
	UE_LOG(LogLeafTest, Log, TEXT("LEAFTEST ground at the centre is %.2f m high"), Point.Z / 100.f);
	int32 Placed = 0;
	for (int32 Index = 0; Index < LeafCount; ++Index)
	{
		const bool bInHeap = Random.FRand() < HeapShare;
		const float RadiusCm = RadiusMetres * 100.f * (bInHeap ? HeapRadiusFraction : 1.f);
		const float Angle = Random.FRand() * TWO_PI;
		const float Distance = RadiusCm * FMath::Sqrt(Random.FRand());
		const FVector Ground2D(CentreMetres.X * 100.f + FMath::Cos(Angle) * Distance,
			CentreMetres.Y * 100.f + FMath::Sin(Angle) * Distance, 0.f);
		if (!TraceGround(Ground2D, Point, Normal))
		{
			continue;
		}
		AddLeaf(Point, Normal, bInHeap ? Random.FRand() * HeapHeightCm : 0.f);
		++Placed;
	}
	UE_LOG(LogLeafTest, Log, TEXT("LEAFTEST placed %d leaves around (%.1f, %.1f) m, %s"), Placed, CentreMetres.X, CentreMetres.Y,
		bUseNanite ? TEXT("Nanite") : TEXT("instanced static meshes"));
}

void ALeafTestPatch::AddLeaf(const FVector& Point, const FVector& Normal, float HeightOffsetCm)
{
	if (ShapeComponents.IsEmpty())
	{
		return;
	}
	const FQuat Alignment = FQuat::FindBetweenNormals(FVector::UpVector, Normal.GetSafeNormal());
	const FQuat Yaw(FVector::UpVector, Random.FRand() * TWO_PI);
	const FQuat Tilt = FRotator(Random.FRandRange(-MaximumTiltDegrees, MaximumTiltDegrees), 0.f,
		Random.FRandRange(-MaximumTiltDegrees, MaximumTiltDegrees)).Quaternion();
	const float Scale = Random.FRandRange(MinimumScale, MaximumScale);
	const FVector Location = Point + Normal * (SurfaceLiftCm + HeightOffsetCm);
	UInstancedStaticMeshComponent* Component = ShapeComponents[Random.RandRange(0, ShapeComponents.Num() - 1)];
	Component->AddInstance(FTransform(Alignment * Yaw * Tilt, Location, FVector(Scale)), /*bWorldSpace=*/true);
}
