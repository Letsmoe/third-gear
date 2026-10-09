#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WorldTileActor.generated.h"

class UDynamicMeshComponent;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;
namespace UE::Geometry { class FDynamicMesh3; }

/**
 * The generated content of one world tile at one detail level: ground chunks, markings and buildings as dynamic
 * meshes, plants as instances. AWorldStreamer fills it piece by piece over several frames, so no single frame pays
 * for a whole tile, and turns on ground collision only for the chunks near the car. Never saved.
 */
UCLASS(NotPlaceable, Transient)
class MAPRUNTIME_API AWorldTileActor : public AActor
{
	GENERATED_BODY()

public:
	AWorldTileActor();

	/** Adds one ground chunk; WorldBoundsCm is its rectangle, used to decide when it needs collision. */
	void AddGroundChunk(UE::Geometry::FDynamicMesh3&& Mesh, const FBox2D& WorldBoundsCm, const TArray<UMaterialInterface*>& Materials);

	/** Adds the road markings (never collide). */
	void AddMarkings(UE::Geometry::FDynamicMesh3&& Mesh, const TArray<UMaterialInterface*>& Materials);

	/** Adds the buildings, colliding when bCollision. */
	void AddBuildings(UE::Geometry::FDynamicMesh3&& Mesh, const TArray<UMaterialInterface*>& Materials, bool bCollision, bool bCookNow);

	/** Adds instances of one plant model (transforms relative to the tile). */
	void AddPlants(UStaticMesh* Mesh, const TArray<FTransform>& Transforms, int32 CullDistanceCm);

	/** Invisible cylinders at the trunks so the car can hit trees: base points (tile-relative) and diameters, cm. */
	void AddTrunkColliders(const TArray<FVector>& Bases, const TArray<float>& Diameters);

	/**
	 * The ground chunk without collision nearest to LocationCm and within RadiusCm, or INDEX_NONE.
	 * OutDistanceCm receives its distance.
	 */
	int32 FindChunkNeedingCollision(const FVector2D& LocationCm, double RadiusCm, double& OutDistanceCm) const;

	/** Turns on collision for one ground chunk; with bCookNow it is ready when this returns. */
	void EnableChunkCollision(int32 Chunk, bool bCookNow);

private:
	UDynamicMeshComponent* AddMeshComponent(UE::Geometry::FDynamicMesh3&& Mesh, const TArray<UMaterialInterface*>& Materials,
		bool bCastShadow);

	UInstancedStaticMeshComponent* AddInstances(UStaticMesh* Mesh, const TArray<FTransform>& Transforms, int32 CullDistanceCm);

	void EnableCollision(UDynamicMeshComponent* Component, bool bCookNow);

	void RegisterNew(UPrimitiveComponent* Component);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UDynamicMeshComponent>> GroundChunks;

	TArray<FBox2D> GroundChunkBounds;
	TArray<bool> GroundChunkHasCollision;
};
