#include "WorldStructuresActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "WorldStructures.h"
#include "WorldTileData.h"

DEFINE_LOG_CATEGORY_STATIC(LogWorldStructuresActor, Log, All);

AWorldStructuresActor::AWorldStructuresActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Static);
}

bool AWorldStructuresActor::Load(const FString& Path)
{
	const double StartTime = FPlatformTime::Seconds();
	FWorldTileData Data;
	FString Error;
	if (!FWorldTileData::Load(Path, Data, Error, EWorldTileSections::Names | EWorldTileSections::Structures))
	{
		UE_LOG(LogWorldStructuresActor, Warning, TEXT("No tall structures: %s"), *Error);
		return false;
	}
	FWorldStructureInstances Instances;
	BuildStructureInstances(Data, Instances);
	// The records are relative to the file's corner, so the components hang from a root standing there.
	RootComponent->SetMobility(EComponentMobility::Movable);
	RootComponent->SetWorldLocation(FVector(Data.Origin.X * 100.0, Data.Origin.Y * 100.0, 0.0));
	RootComponent->SetMobility(EComponentMobility::Static);
	int32 InstanceCount = 0;
	for (const FWorldStructureGroup& Group : Instances.Groups)
	{
		AddStructureGroup(*this, *RootComponent, Group, /*bCollision=*/true);
		InstanceCount += Group.Transforms.Num();
	}
	for (const FWorldStructureRotor& Rotor : Instances.Rotors)
	{
		UStaticMesh* Mesh = FindStructureMeshCached(Rotor.Piece);
		if (!Mesh)
		{
			continue;
		}
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(this);
		Component->SetStaticMesh(Mesh);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
		Component->SetupAttachment(RootComponent);
		Component->SetRelativeLocation(Rotor.Location);
		Component->SetRelativeRotation(Rotor.Shaft * FQuat(FVector::RightVector, FMath::DegreesToRadians(Rotor.StartDegrees)));
		Component->RegisterComponent();
		AddInstanceComponent(Component);
		Rotors.Add(Component);
		RotorStates.Add({Rotor.Shaft, Rotor.Rpm * 6.f, Rotor.StartDegrees});
	}
	SetActorTickEnabled(!Rotors.IsEmpty());
	UE_LOG(LogWorldStructuresActor, Log, TEXT("Tall structures: %d instances in %d groups, %d rotors from %s in %.2f s"),
		InstanceCount, Instances.Groups.Num(), Rotors.Num(), *Path, FPlatformTime::Seconds() - StartTime);
	return true;
}

void AWorldStructuresActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const double Time = GetWorld()->GetTimeSeconds();
	for (int32 Index = 0; Index < Rotors.Num(); ++Index)
	{
		const FRotorState& State = RotorStates[Index];
		const double Degrees = FMath::Fmod(State.StartDegrees + State.DegreesPerSecond * Time, 360.0);
		Rotors[Index]->SetRelativeRotation(State.Shaft * FQuat(FVector::RightVector, FMath::DegreesToRadians(Degrees)));
	}
}
