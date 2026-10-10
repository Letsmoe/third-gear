#include "WorldStructures.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/StrongObjectPtr.h"
#include "WorldTileActor.h"
#include "WorldTileData.h"

DEFINE_LOG_CATEGORY_STATIC(LogWorldStructures, Log, All);

namespace
{
constexpr double CentimetresPerMetre = 100.0;
/** Groups added per spawn step of a tile; a group is one component with all instances of a piece. */
constexpr int32 GroupsPerStep = 4;

/** /Game/World/Structures/Meshes/<family>/SM_<piece>: the family is the piece name up to its first underscore. */
FString StructureMeshPath(const FString& Piece)
{
	FString Family;
	FString Rest;
	if (!Piece.Split(TEXT("_"), &Family, &Rest))
	{
		Family = Piece;
	}
	return FString::Printf(TEXT("/Game/World/Structures/Meshes/%s/SM_%s.SM_%s"), *Family, *Piece, *Piece);
}

/** A variant material instance by slot and variant, cached; null when the slot has no such variant. */
UMaterialInterface* FindVariantMaterialCached(const FName& Slot, const FString& Variant)
{
	static TMap<FString, TStrongObjectPtr<UMaterialInterface>> Cache;
	const FString Name = FString::Printf(TEXT("MI_Struct_%s_%s"), *Slot.ToString(), *Variant);
	if (const TStrongObjectPtr<UMaterialInterface>* Found = Cache.Find(Name))
	{
		return Found->Get();
	}
	const FString Path = FString::Printf(TEXT("/Game/World/Structures/Materials/%s.%s"), *Name, *Name);
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	Cache.Add(Name, TStrongObjectPtr<UMaterialInterface>(Material));
	return Material;
}

FWorldStructureGroup& FindOrAddGroup(TMap<FString, int32>& GroupIndex, FWorldStructureInstances& Out, const FString& Piece,
	const FString& Variant, bool bWire)
{
	const FString Key = Piece + TEXT("|") + Variant;
	if (const int32* Found = GroupIndex.Find(Key))
	{
		return Out.Groups[*Found];
	}
	GroupIndex.Add(Key, Out.Groups.Num());
	FWorldStructureGroup& Group = Out.Groups.AddDefaulted_GetRef();
	Group.Piece = Piece;
	Group.Variant = Variant;
	Group.bWire = bWire;
	return Group;
}

FVector LocationCm(const FWorldStructure& Structure)
{
	return FVector(Structure.Position) * CentimetresPerMetre;
}
}

void BuildStructureInstances(const FWorldTileData& Data, FWorldStructureInstances& Out)
{
	TMap<FString, int32> GroupIndex;
	for (const FWorldStructure& Structure : Data.Structures)
	{
		if (!Data.Names.IsValidIndex(Structure.Mesh))
		{
			continue;
		}
		const FString& Piece = Data.Names[Structure.Mesh];
		const FString Variant = Data.Names.IsValidIndex(Structure.Variant) ? Data.Names[Structure.Variant] : FString();
		switch (Structure.Kind)
		{
		case EWorldStructureKind::Rotor:
		{
			FWorldStructureRotor& Rotor = Out.Rotors.AddDefaulted_GetRef();
			Rotor.Piece = Piece;
			Rotor.Location = LocationCm(Structure);
			// Yaw about Z, then the shaft tilted about the rotor's own X (front end, local +Y, up).
			Rotor.Shaft = FQuat(FVector::UpVector, FMath::DegreesToRadians(Structure.Rotation.Yaw))
				* FQuat(FVector::ForwardVector, FMath::DegreesToRadians(Structure.Rotation.Roll));
			Rotor.Rpm = Structure.Param0;
			Rotor.StartDegrees = Structure.Param1;
			break;
		}
		case EWorldStructureKind::Wire:
		{
			FWorldStructureGroup& Group = FindOrAddGroup(GroupIndex, Out, Piece, Variant, true);
			const FRotator Rotation(Structure.Rotation.Pitch, Structure.Rotation.Yaw, 0.f);
			Group.Transforms.Add(FTransform(Rotation, LocationCm(Structure), FVector(Structure.Scale.X, 1.f, 1.f)));
			Group.Sags.Add(Structure.Param0);
			break;
		}
		default:
		{
			FWorldStructureGroup& Group = FindOrAddGroup(GroupIndex, Out, Piece, Variant, false);
			const FRotator Rotation(Structure.Rotation.Pitch, Structure.Rotation.Yaw, Structure.Rotation.Roll);
			Group.Transforms.Add(FTransform(Rotation, LocationCm(Structure), FVector(Structure.Scale)));
			break;
		}
		}
	}
}

UStaticMesh* FindStructureMeshCached(const FString& Piece)
{
	static TMap<FString, TStrongObjectPtr<UStaticMesh>> Cache;
	if (const TStrongObjectPtr<UStaticMesh>* Found = Cache.Find(Piece))
	{
		return Found->Get();
	}
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *StructureMeshPath(Piece), nullptr, LOAD_NoWarn);
	if (!Mesh)
	{
		UE_LOG(LogWorldStructures, Warning, TEXT("Structure mesh %s missing; run Scripts/import_structures.py"), *Piece);
	}
	Cache.Add(Piece, TStrongObjectPtr<UStaticMesh>(Mesh));
	return Mesh;
}

void ApplyStructureVariant(UMeshComponent& Component, UStaticMesh& Mesh, const FString& Variant)
{
	if (Variant.IsEmpty())
	{
		return;
	}
	const TArray<FStaticMaterial>& Slots = Mesh.GetStaticMaterials();
	for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
	{
		if (UMaterialInterface* Material = FindVariantMaterialCached(Slots[Slot].MaterialSlotName, Variant))
		{
			Component.SetMaterial(Slot, Material);
		}
	}
}

UInstancedStaticMeshComponent* AddStructureGroup(AActor& Owner, USceneComponent& Parent, const FWorldStructureGroup& Group,
	bool bCollision)
{
	UStaticMesh* Mesh = FindStructureMeshCached(Group.Piece);
	if (!Mesh || Group.Transforms.IsEmpty())
	{
		return nullptr;
	}
	UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(&Owner);
	Component->SetStaticMesh(Mesh);
	Component->SetMobility(EComponentMobility::Static);
	Component->SetCanEverAffectNavigation(false);
	ApplyStructureVariant(*Component, *Mesh, Group.Variant);
	if (Group.bWire || !bCollision)
	{
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	else
	{
		Component->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	}
	if (Group.bWire)
	{
		// Thin wires add nothing to the shadows but cost a page in every shadow map cascade they cross.
		Component->SetCastShadow(false);
		Component->NumCustomDataFloats = 1;
	}
	Component->SetupAttachment(&Parent);
	Component->RegisterComponent();
	Owner.AddInstanceComponent(Component);
	Component->AddInstances(Group.Transforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
	for (int32 Index = 0; Index < Group.Sags.Num(); ++Index)
	{
		Component->SetCustomDataValue(Index, 0, Group.Sags[Index], /*bMarkRenderStateDirty=*/Index == Group.Sags.Num() - 1);
	}
	return Component;
}

bool AddStructureGroupsStep(AWorldTileActor& Actor, const FWorldStructureInstances& Structures, int32 Step)
{
	const int32 First = Step * GroupsPerStep;
	const int32 Last = FMath::Min(First + GroupsPerStep, Structures.Groups.Num());
	for (int32 Index = First; Index < Last; ++Index)
	{
		AddStructureGroup(Actor, *Actor.GetRootComponent(), Structures.Groups[Index], /*bCollision=*/true);
	}
	return Last >= Structures.Groups.Num();
}
