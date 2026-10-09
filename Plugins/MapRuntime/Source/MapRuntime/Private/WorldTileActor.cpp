#include "WorldTileActor.h"
#include "WorldSnow.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UDynamicMesh.h"
#include "TrafficNetwork.h"
#include "TrafficSubsystem.h"
#include "AITrafficCar.h"
#include "AITrafficSubsystem.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ParkedCars.h"
#include "WorldFurniture.h"
#include "WorldTileMesher.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"

namespace
{
constexpr float TrunkColliderHeightCm = 400.f;

}

AWorldTileActor::AWorldTileActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	PrimaryActorTick.TickInterval = 0.25f;
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

void AWorldTileActor::AddPlants(UStaticMesh* Mesh, const TArray<FTransform>& Transforms, int32 CullDistanceCm,
	const TArray<UMaterialInterface*>& WindMaterials, int32 WindDistanceCm)
{
	if (!Mesh || Transforms.IsEmpty())
	{
		return;
	}
	UInstancedStaticMeshComponent* Component = AddInstances(Mesh, Transforms, CullDistanceCm);
	if (WindMaterials.IsEmpty())
	{
		return;
	}
	for (int32 Slot = 0; Slot < WindMaterials.Num(); ++Slot)
	{
		Component->SetMaterial(Slot, WindMaterials[Slot]);
	}
	Component->SetWorldPositionOffsetDisableDistance(WindDistanceCm);
	// Swaying trees would otherwise be re-rendered into the virtual shadow maps every frame (about 14 ms in the park
	// of bergedorf_core at VR resolution). Their shadows stay at the rest pose, which a few centimetres of sway don't show.
	Component->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
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

UInstancedStaticMeshComponent* AWorldTileActor::AddFurnitureInstances(UStaticMesh* Mesh, const TArray<FTransform>& Transforms, bool bCastShadow,
	bool bCollision, int32 CullDistanceCm)
{
	if (!Mesh || Transforms.IsEmpty())
	{
		return nullptr;
	}
	UInstancedStaticMeshComponent* Component = AddInstances(Mesh, Transforms, CullDistanceCm);
	Component->SetCastShadow(bCastShadow);
	if (bCollision)
	{
		// Creating a physics body per instance is the expensive part: it waits until the viewer is near (EnableNextFurnitureCollision).
		FurnitureColliders.Add(Component);
	}
	return Component;
}

bool AWorldTileActor::EnableNextFurnitureCollision()
{
	if (FurnitureCollidersEnabled >= FurnitureColliders.Num())
	{
		return false;
	}
	FurnitureColliders[FurnitureCollidersEnabled++]->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	return true;
}

bool AWorldTileActor::AddFurnitureStep(const FWorldFurnitureInstances& Furniture, const FFurnitureMeshes& Meshes, int32 Step)
{
	TArray<TFunction<void()>> Parts;
	Parts.Add([&]() { AddFurnitureInstances(Meshes.Lamp, Furniture.Lamps, true, true); });
	Parts.Add([&]() { AddFurnitureInstances(Meshes.SignalPole, Furniture.SignalPoles, true, true); });
	Parts.Add([&]()
	{
		if (!Meshes.SignalHead || Furniture.SignalHeads.IsEmpty())
		{
			return;
		}
		// Lens glow comes from three per-instance floats (red, amber, green) that UpdateSignalHeads keeps current.
		UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(this);
		Component->SetStaticMesh(Meshes.SignalHead);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->NumCustomDataFloats = 3;
		RegisterNew(Component);
		Component->SetMobility(EComponentMobility::Movable);
		Component->AddInstances(Furniture.SignalHeads, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/false);
		SignalHeadComponent = Component;
		HeadApproachIds = Furniture.HeadApproaches;
		HeadShownAspects.Init(255, HeadApproachIds.Num());
	});
	for (const TPair<int32, TArray<FTransform>>& Poles : Furniture.SignPoles)
	{
		Parts.Add([&]()
		{
			UStaticMesh* const* Mesh = Meshes.SignPoles.Find(Poles.Key);
			AddFurnitureInstances(Mesh ? *Mesh : nullptr, Poles.Value, true, true);
		});
	}
	Parts.Add([&]() { AddFurnitureInstances(Meshes.SignClamp, Furniture.SignClamps, false, false); });
	for (const TPair<FString, TArray<FTransform>>& Plates : Furniture.SignPlates)
	{
		Parts.Add([&]()
		{
			UInstancedStaticMeshComponent* Component = AddFurnitureInstances(Meshes.SignPlate, Plates.Value, false, false);
			UMaterialInterface* const* Material = Meshes.SignMaterials.Find(Plates.Key);
			if (Component && Material && *Material)
			{
				Component->SetMaterial(0, *Material);
			}
		});
	}
	if (!Furniture.ParkedCars.IsEmpty())
	{
		Parts.Add([&]() { AddParkedCarColliders(Furniture, Meshes); });
		for (int32 ModelIndex = 0; ModelIndex < Meshes.ParkedModels.Num(); ++ModelIndex)
		{
			Parts.Add([&, ModelIndex]() { AddParkedCarBodies(Furniture, Meshes, ModelIndex); });
			Parts.Add([&, ModelIndex]() { AddParkedCarGlassAndWheels(Furniture, Meshes, ModelIndex); });
		}
	}
	Parts.Add([&]() { RegisterFurnitureLights(Furniture); });
	Parts[Step]();
	return Step + 1 >= Parts.Num();
}

namespace
{
/** Parked cars are drawn out to these distances; beyond them the street's cars are too small to tell from the buildings' shade. */
constexpr int32 ParkedBodyCullCm = 12000;
/** Glass, wheels and the shadow of the body only exist this close: they are small, and each is a draw per car and per shadow view. */
constexpr int32 ParkedDetailCullCm = 4000;

/** tg.ParkedCars.Parts is a bit mask of what parked cars draw (1 body, 2 glass, 4 wheels, 8 body shadow), for measuring what each costs. */
static TAutoConsoleVariable<int32> CVarParkedParts(TEXT("tg.ParkedCars.Parts"), 15, TEXT("Bit mask of the parked car parts that are built: 1 body, 2 glass, 4 wheels, 8 body shadow."));

bool ParkedPartEnabled(int32 Bit)
{
	return (CVarParkedParts.GetValueOnGameThread() & Bit) != 0;
}
}

void AWorldTileActor::AddParkedCarColliders(const FWorldFurnitureInstances& Furniture, const FFurnitureMeshes& Meshes)
{
	TArray<FTransform> Boxes;
	for (const FParkedCarPlacement& Car : Furniture.ParkedCars)
	{
		if (Meshes.ParkedModels.IsValidIndex(Car.ModelIndex) && Meshes.ParkedModels[Car.ModelIndex])
		{
			Boxes.Add(ParkedCars::ColliderTransform(*Meshes.ParkedModels[Car.ModelIndex], Car.Pose));
		}
	}
	if (UInstancedStaticMeshComponent* Component = AddFurnitureInstances(Meshes.ParkedCollider, Boxes, false, true))
	{
		Component->SetVisibility(false);
	}
}

void AWorldTileActor::AddParkedCarBodies(const FWorldFurnitureInstances& Furniture, const FFurnitureMeshes& Meshes, int32 ModelIndex)
{
	const FTrafficVehicleModel* Model = Meshes.ParkedModels[ModelIndex];
	if (!Model || !Model->Body || !ParkedPartEnabled(1))
	{
		return;
	}
	TMap<int32, TArray<FTransform>> ByPaint;
	TArray<FTransform> AllPoses;
	for (const FParkedCarPlacement& Car : Furniture.ParkedCars)
	{
		if (Car.ModelIndex == ModelIndex)
		{
			const FTransform Pose = ParkedCars::MeshTransform(*Model, Car.Pose);
			ByPaint.FindOrAdd(Car.PaintIndex).Add(Pose);
			AllPoses.Add(Pose);
		}
	}
	for (const TPair<int32, TArray<FTransform>>& Paint : ByPaint)
	{
		UInstancedStaticMeshComponent* Component = AddFurnitureInstances(Model->Body, Paint.Value, false, false, ParkedBodyCullCm);
		if (!Component || Model->PaintSlot == INDEX_NONE)
		{
			continue;
		}
		if (Model->TrafficPaint)
		{
			Component->SetMaterial(Model->PaintSlot, Model->TrafficPaint);
		}
		if (UMaterialInstanceDynamic* Instance = Component->CreateDynamicMaterialInstance(Model->PaintSlot))
		{
			Instance->SetVectorParameterValue(TEXT("BaseColor"), UAITrafficSubsystem::GetPaintPaletteColor(Paint.Key));
		}
	}
	if (!ParkedPartEnabled(8))
	{
		return;
	}
	// The body's shadow comes from a second set that draws only into shadow views and only close by.
	if (UInstancedStaticMeshComponent* ShadowOnly = AddFurnitureInstances(Model->Body, AllPoses, true, false, ParkedDetailCullCm))
	{
		ShadowOnly->SetRenderInMainPass(false);
	}
}

void AWorldTileActor::AddParkedCarGlassAndWheels(const FWorldFurnitureInstances& Furniture, const FFurnitureMeshes& Meshes, int32 ModelIndex)
{
	const FTrafficVehicleModel* Model = Meshes.ParkedModels[ModelIndex];
	if (!Model)
	{
		return;
	}
	TArray<FTransform> Poses;
	for (const FParkedCarPlacement& Car : Furniture.ParkedCars)
	{
		if (Car.ModelIndex == ModelIndex)
		{
			Poses.Add(ParkedCars::MeshTransform(*Model, Car.Pose));
		}
	}
	if (ParkedPartEnabled(2))
	{
		AddFurnitureInstances(Model->Glass, Poses, false, false, ParkedDetailCullCm);
	}
	if (!ParkedPartEnabled(4))
	{
		return;
	}
	for (UStaticMesh* Wheel : Model->Wheels)
	{
		AddFurnitureInstances(Wheel, Poses, false, false, ParkedDetailCullCm);
	}
}

void AWorldTileActor::RegisterFurnitureLights(const FWorldFurnitureInstances& Furniture)
{
	UTrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UTrafficSubsystem>();
	if (!Traffic)
	{
		return;
	}
	TArray<FStreetLightSource> Sources;
	for (const FFurnitureLight& Light : Furniture.Lights)
	{
		FStreetLightSource& Source = Sources.AddDefaulted_GetRef();
		Source.LocationCm = Light.LocationCm + GetActorLocation();
		Source.Direction = Light.Direction;
		Source.ApproachId = Light.ApproachId;
		FMemory::Memcpy(Source.LensOffsetsCm, Light.LensOffsetsCm, sizeof(Source.LensOffsetsCm));
	}
	Traffic->AddLightSources(this, MoveTemp(Sources));
	if (SignalHeadComponent)
	{
		Traffic->RegisterSignalTile(this);
		UpdateSignalHeads(Traffic->GetNetwork(), Traffic->GetTrafficTime());
	}
}

void AWorldTileActor::UpdateSignalHeads(const FTrafficNetwork& Network, double TimeSeconds)
{
	if (!SignalHeadComponent)
	{
		return;
	}
	bool bChanged = false;
	for (int32 Index = 0; Index < HeadApproachIds.Num(); ++Index)
	{
		const ESignalAspect Aspect = Network.GetApproachState(HeadApproachIds[Index], TimeSeconds).Aspect;
		if (HeadShownAspects[Index] == uint8(Aspect))
		{
			continue;
		}
		HeadShownAspects[Index] = uint8(Aspect);
		const bool bRed = Aspect == ESignalAspect::Red || Aspect == ESignalAspect::RedAmber;
		const bool bAmber = Aspect == ESignalAspect::Amber || Aspect == ESignalAspect::RedAmber;
		const float Lenses[3] = {bRed ? 1.f : 0.f, bAmber ? 1.f : 0.f, Aspect == ESignalAspect::Green ? 1.f : 0.f};
		SignalHeadComponent->SetCustomData(Index, MakeArrayView(Lenses, 3), /*bMarkRenderStateDirty=*/false);
		bChanged = true;
	}
	if (bChanged)
	{
		SignalHeadComponent->MarkRenderStateDirty();
	}
}

void AWorldTileActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		if (UTrafficSubsystem* Traffic = World->GetSubsystem<UTrafficSubsystem>())
		{
			Traffic->RemoveLightSources(this);
			Traffic->UnregisterSignalTile(this);
		}
	}
	Super::EndPlay(EndPlayReason);
}

float ReadWorldSnowCover(UWorld* World)
{
	static TWeakObjectPtr<UMaterialParameterCollection> Collection;
	if (!Collection.IsValid())
	{
		Collection = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/World/MPC_Weather.MPC_Weather"), nullptr, LOAD_NoWarn);
	}
	UMaterialParameterCollectionInstance* Instance = Collection.IsValid() && World ? World->GetParameterCollectionInstance(Collection.Get()) : nullptr;
	float Cover = 0.f;
	if (Instance)
	{
		Instance->GetScalarParameterValue(FName(TEXT("SnowCover")), Cover);
	}
	return Cover;
}

namespace
{
/** Cover above which the snow layer is drawn. */
constexpr float SnowShownAbove = 0.004f;
/** The snow layer is drawn out to this distance, cm; beyond it the ground material's snow texture shows. */
constexpr float SnowDrawDistanceCm = 18000.f;
}

void AWorldTileActor::AddSnowChunk(UE::Geometry::FDynamicMesh3&& Mesh, const FBox2D& WorldBoundsCm, UMaterialInterface* Material)
{
	if (!Material)
	{
		return;
	}
	UDynamicMeshComponent* Component = AddMeshComponent(MoveTemp(Mesh), {Material}, /*bCastShadow=*/false);
	if (!Component)
	{
		return;
	}
	bSnowShown = ReadWorldSnowCover(GetWorld()) > SnowShownAbove;
	Component->SetCullDistance(SnowDrawDistanceCm);
	Component->SetVisibility(bSnowShown);
	SnowChunks.Add(Component);
	SetActorTickEnabled(true);
}

void AWorldTileActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const bool bShown = ReadWorldSnowCover(GetWorld()) > SnowShownAbove;
	if (bShown == bSnowShown)
	{
		return;
	}
	bSnowShown = bShown;
	for (UDynamicMeshComponent* Component : SnowChunks)
	{
		Component->SetVisibility(bShown);
	}
}
