#include "WorldTileActor.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UDynamicMesh.h"
#include "WorldTileMesher.h"

namespace
{
constexpr float TrunkColliderHeightCm = 400.f;

}

AWorldTileActor::AWorldTileActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Static);
}

void AWorldTileActor::RegisterNew(UPrimitiveComponent* Component)
{
	Component->SetMobility(EComponentMobility::Static);
	Component->SetupAttachment(RootComponent);
	Component->SetCanEverAffectNavigation(false);
	Component->RegisterComponent();
	AddInstanceComponent(Component);
}

UDynamicMeshComponent* AWorldTileActor::AddMeshComponent(UE::Geometry::FDynamicMesh3&& Mesh, const TArray<UMaterialInterface*>& Materials,
	bool bCastShadow)
{
	if (Mesh.TriangleCount() == 0)
	{
		return nullptr;
	}
	UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(this);
	Component->SetTangentsType(EDynamicMeshComponentTangentsMode::ExternallyProvided);
	// The mesh never changes once built. The static draw path caches its draw commands, which is cheaper and is what
	// Lumen's surface cache captures from; on the dynamic path every tile was missing from Lumen's bounce light.
	Component->SetMeshDrawPath(EDynamicMeshDrawPath::StaticDraw);
	Component->SetCastShadow(bCastShadow);
	Component->GetDynamicMesh()->SetMesh(MoveTemp(Mesh));
	Component->ConfigureMaterialSet(Materials);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RegisterNew(Component);
	return Component;
}

void AWorldTileActor::EnableCollision(UDynamicMeshComponent* Component, bool bCookNow)
{
	Component->bUseAsyncCooking = !bCookNow;
	Component->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	Component->SetComplexAsSimpleCollisionEnabled(true, /*bImmediateUpdate=*/false);
	Component->UpdateCollision(/*bOnlyIfPending=*/false);
}

void AWorldTileActor::AddGroundChunk(UE::Geometry::FDynamicMesh3&& Mesh, const FBox2D& WorldBoundsCm, const TArray<UMaterialInterface*>& Materials)
{
	if (UDynamicMeshComponent* Component = AddMeshComponent(MoveTemp(Mesh), Materials, /*bCastShadow=*/true))
	{
		GroundChunks.Add(Component);
		GroundChunkBounds.Add(WorldBoundsCm);
		GroundChunkHasCollision.Add(false);
	}
}

void AWorldTileActor::AddMarkings(UE::Geometry::FDynamicMesh3&& Mesh, const TArray<UMaterialInterface*>& Materials)
{
	AddMeshComponent(MoveTemp(Mesh), Materials, /*bCastShadow=*/false);
}

void AWorldTileActor::AddBuildings(UE::Geometry::FDynamicMesh3&& Mesh, const TArray<UMaterialInterface*>& Materials, bool bCollision, bool bCookNow)
{
	UDynamicMeshComponent* Component = AddMeshComponent(MoveTemp(Mesh), Materials, /*bCastShadow=*/true);
	if (Component && bCollision)
	{
		EnableCollision(Component, bCookNow);
	}
}

UInstancedStaticMeshComponent* AWorldTileActor::AddInstances(UStaticMesh* Mesh, const TArray<FTransform>& Transforms, int32 CullDistanceCm)
{
	UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(this);
	Component->SetStaticMesh(Mesh);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (CullDistanceCm > 0)
	{
		Component->SetCullDistances(FMath::RoundToInt(CullDistanceCm * 0.9f), CullDistanceCm);
	}
	RegisterNew(Component);
	Component->AddInstances(Transforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
	return Component;
}

void AWorldTileActor::AddPlants(UStaticMesh* Mesh, const TArray<FTransform>& Transforms, int32 CullDistanceCm)
{
	if (Mesh && !Transforms.IsEmpty())
	{
		AddInstances(Mesh, Transforms, CullDistanceCm);
	}
}

void AWorldTileActor::AddTrunkColliders(const TArray<FVector>& Bases, const TArray<float>& Diameters)
{
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (!Cylinder || Bases.IsEmpty())
	{
		return;
	}
	// The engine cylinder is 100 cm wide and tall, centred on its pivot.
	TArray<FTransform> Transforms;
	for (int32 Index = 0; Index < Bases.Num(); ++Index)
	{
		const float Width = FMath::Max(Diameters[Index], 10.f) / 100.f;
		Transforms.Emplace(FQuat::Identity, Bases[Index] + FVector(0, 0, TrunkColliderHeightCm * 0.5f),
			FVector(Width, Width, TrunkColliderHeightCm / 100.f));
	}
	UInstancedStaticMeshComponent* Component = AddInstances(Cylinder, Transforms, 0);
	Component->SetVisibility(false);
	Component->SetCastShadow(false);
	Component->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
}

int32 AWorldTileActor::FindChunkNeedingCollision(const FVector2D& LocationCm, double RadiusCm, double& OutDistanceCm) const
{
	int32 Best = INDEX_NONE;
	OutDistanceCm = RadiusCm;
	for (int32 Chunk = 0; Chunk < GroundChunks.Num(); ++Chunk)
	{
		const double Distance = DistanceToBox2D(GroundChunkBounds[Chunk], LocationCm);
		if (!GroundChunkHasCollision[Chunk] && Distance <= OutDistanceCm)
		{
			Best = Chunk;
			OutDistanceCm = Distance;
		}
	}
	return Best;
}

void AWorldTileActor::EnableChunkCollision(int32 Chunk, bool bCookNow)
{
	if (GroundChunks.IsValidIndex(Chunk) && !GroundChunkHasCollision[Chunk])
	{
		EnableCollision(GroundChunks[Chunk], bCookNow);
		GroundChunkHasCollision[Chunk] = true;
	}
}
