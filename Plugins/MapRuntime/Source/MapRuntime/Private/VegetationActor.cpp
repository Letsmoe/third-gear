#include "VegetationActor.h"

#include "Components/InstancedSkinnedMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
const TCHAR* VegetationTag = TEXT("Vegetation");

// UInstancedSkinnedMeshComponent has no setter for its cull distances.
void SetIntProperty(UObject* Object, FName Name, int32 Value)
{
	if (FIntProperty* Prop = FindFProperty<FIntProperty>(Object->GetClass(), Name))
	{
		Prop->SetPropertyValue_InContainer(Object, Value);
	}
}
}

AVegetationActor::AVegetationActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Static);
}

void AVegetationActor::ClearVegetation()
{
	TArray<UActorComponent*> Components;
	GetComponents(Components);
	for (UActorComponent* Component : Components)
	{
		if (Component->ComponentHasTag(VegetationTag))
		{
			RemoveInstanceComponent(Component);
			Component->DestroyComponent();
		}
	}
}

void AVegetationActor::RegisterNewComponent(UPrimitiveComponent* Component)
{
	Component->ComponentTags.Add(VegetationTag);
	Component->SetMobility(EComponentMobility::Static);
	Component->SetupAttachment(RootComponent);
	AddInstanceComponent(Component); // serialised with the level like a component added in the editor
	Component->RegisterComponent();
}

UInstancedSkinnedMeshComponent* AVegetationActor::AddSkinnedInstances(USkinnedAsset* Mesh, UTransformProviderData* Wind,
	const TArray<FTransform>& Transforms, int32 EndCullDistance)
{
	if (!Mesh || Transforms.IsEmpty())
	{
		return nullptr;
	}
	const FName Name = MakeUniqueObjectName(this, UInstancedSkinnedMeshComponent::StaticClass(),
		*FString::Printf(TEXT("ISKM_%s"), *Mesh->GetName()));
	UInstancedSkinnedMeshComponent* Component = NewObject<UInstancedSkinnedMeshComponent>(this, Name, RF_Transactional);
	Component->SetSkinnedAsset(Mesh);
	if (Wind)
	{
		Component->SetTransformProvider(Wind);
	}
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetCanEverAffectNavigation(false);
	if (EndCullDistance > 0)
	{
		SetIntProperty(Component, TEXT("InstanceStartCullDistance"), FMath::RoundToInt(EndCullDistance * 0.9f));
		SetIntProperty(Component, TEXT("InstanceEndCullDistance"), EndCullDistance);
	}
	RegisterNewComponent(Component);

	TArray<int32> AnimationIndices;
	AnimationIndices.SetNumZeroed(Transforms.Num());
	Component->AddInstances(Transforms, AnimationIndices, /*bShouldReturnIds=*/false, /*bWorldSpace=*/true);
	return Component;
}

UInstancedStaticMeshComponent* AVegetationActor::AddStaticInstances(UStaticMesh* Mesh, const TArray<FTransform>& Transforms,
	bool bCollision, int32 EndCullDistance)
{
	if (!Mesh || Transforms.IsEmpty())
	{
		return nullptr;
	}
	const FName Name = MakeUniqueObjectName(this, UInstancedStaticMeshComponent::StaticClass(),
		*FString::Printf(TEXT("ISM_%s"), *Mesh->GetName()));
	UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(this, Name, RF_Transactional);
	Component->SetStaticMesh(Mesh);
	Component->SetCollisionEnabled(bCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	Component->SetCanEverAffectNavigation(false);
	if (EndCullDistance > 0)
	{
		Component->SetCullDistances(FMath::RoundToInt(EndCullDistance * 0.9f), EndCullDistance);
	}
	RegisterNewComponent(Component);
	Component->AddInstances(Transforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
	return Component;
}

UInstancedStaticMeshComponent* AVegetationActor::AddTrunkColliders(const TArray<FVector>& BasePoints,
	const TArray<float>& Diameters, float Height)
{
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (!Cylinder || BasePoints.IsEmpty() || BasePoints.Num() != Diameters.Num())
	{
		return nullptr;
	}
	// The engine cylinder is 100 cm wide and 100 cm tall, centred on its pivot.
	TArray<FTransform> Transforms;
	Transforms.Reserve(BasePoints.Num());
	for (int32 i = 0; i < BasePoints.Num(); ++i)
	{
		const float D = FMath::Max(Diameters[i], 10.f) / 100.f;
		Transforms.Emplace(FQuat::Identity, BasePoints[i] + FVector(0, 0, Height * 0.5f), FVector(D, D, Height / 100.f));
	}
	UInstancedStaticMeshComponent* Component = AddStaticInstances(Cylinder, Transforms, /*bCollision=*/true);
	if (Component)
	{
		Component->SetVisibility(false);
		Component->SetHiddenInGame(true);
		Component->SetCastShadow(false);
		Component->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	}
	return Component;
}
