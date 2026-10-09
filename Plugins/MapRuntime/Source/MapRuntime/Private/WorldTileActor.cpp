#include "WorldTileActor.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UDynamicMesh.h"
#include "WorldTileMesher.h"

namespace
{
constexpr int32 ShrubCullDistanceCm = 40000;
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

UDynamicMeshComponent* AWorldTileActor::AddMeshComponent(const TCHAR* Name, UE::Geometry::FDynamicMesh3&& Mesh,
	const TArray<UMaterialInterface*>& Materials, bool bCollision, bool bCookNow, bool bCastShadow)
{
	if (Mesh.TriangleCount() == 0)
	{
		return nullptr;
	}
	UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(this, Name);
	Component->SetTangentsType(EDynamicMeshComponentTangentsMode::ExternallyProvided);
	Component->SetCastShadow(bCastShadow);
	Component->GetDynamicMesh()->SetMesh(MoveTemp(Mesh));
	Component->ConfigureMaterialSet(Materials);
	if (bCollision)
	{
		Component->bUseAsyncCooking = !bCookNow;
		Component->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Component->SetComplexAsSimpleCollisionEnabled(true, /*bImmediateUpdate=*/false);
	}
	else
	{
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	RegisterNew(Component);
	if (bCollision)
	{
		Component->UpdateCollision(/*bOnlyIfPending=*/false);
	}
	return Component;
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

void AWorldTileActor::Populate(FWorldTileMeshes& Meshes, const TArray<UMaterialInterface*>& Materials,
	const TMap<FString, TObjectPtr<UStaticMesh>>& PlantModels, bool bCollision, bool bCookNow)
{
	AddMeshComponent(TEXT("Ground"), MoveTemp(Meshes.GroundMesh), Materials, bCollision, bCookNow, /*bCastShadow=*/true);
	AddMeshComponent(TEXT("Markings"), MoveTemp(Meshes.MarkingsMesh), Materials, false, false, /*bCastShadow=*/false);
	AddMeshComponent(TEXT("Buildings"), MoveTemp(Meshes.BuildingsMesh), Materials, bCollision, bCookNow, /*bCastShadow=*/true);
	for (const FWorldPlantInstances& Group : Meshes.Plants)
	{
		const TObjectPtr<UStaticMesh>* Mesh = PlantModels.Find(Group.Model);
		if (Mesh && *Mesh && !Group.Transforms.IsEmpty())
		{
			AddInstances(*Mesh, Group.Transforms, Group.Model == TEXT("shrub") ? ShrubCullDistanceCm : 0);
		}
	}
	if (bCollision)
	{
		AddTrunkColliders(Meshes.TrunkBases, Meshes.TrunkDiameters);
	}
}
