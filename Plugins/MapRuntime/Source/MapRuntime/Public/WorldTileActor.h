#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WorldTileActor.generated.h"

class UDynamicMeshComponent;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;
namespace UE::Geometry { class FDynamicMesh3; }
struct FWorldFurnitureInstances;
class FTrafficNetwork;
enum class ESignalAspect : uint8;

/** The street furniture meshes and sign materials, resolved by the world streamer. */
struct FFurnitureMeshes
{
	UStaticMesh* Lamp = nullptr;
	UStaticMesh* SignalPole = nullptr;
	UStaticMesh* SignalHead = nullptr;
	UStaticMesh* SignPlate = nullptr;
	UStaticMesh* SignClamp = nullptr;
	/** Sign poles by visible height in cm. */
	TMap<int32, UStaticMesh*> SignPoles;
	/** Material per sign graphic name. */
	TMap<FString, UMaterialInterface*> SignMaterials;
};

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

	/**
	 * Adds one chunk of the snow layer (see WorldSnow.h). The snow components are shown only while the weather's
	 * SnowCover is above zero; the actor ticks four times a second to follow it, and only once it has snow.
	 */
	void AddSnowChunk(UE::Geometry::FDynamicMesh3&& Mesh, const FBox2D& WorldBoundsCm, UMaterialInterface* Material);

	virtual void Tick(float DeltaSeconds) override;

	/** Adds the road markings (never collide). */
	void AddMarkings(UE::Geometry::FDynamicMesh3&& Mesh, const TArray<UMaterialInterface*>& Materials);

	/** Adds the buildings, colliding when bCollision; with bVisible false only the collision is there. */
	void AddBuildings(UE::Geometry::FDynamicMesh3&& Mesh, const TArray<UMaterialInterface*>& Materials, bool bCollision, bool bCookNow,
		bool bVisible = true);

	/** Adds the instances of one building kit piece (transforms relative to the tile); null meshes are skipped. */
	void AddKitInstances(UStaticMesh* Mesh, const TArray<FTransform>& Transforms, bool bCastShadow);

	/**
	 * Adds instances of one plant model (transforms relative to the tile). WindMaterials, when not empty, replace the
	 * model's materials with ones that sway in the wind; the sway stops being evaluated beyond WindDistanceCm.
	 */
	void AddPlants(UStaticMesh* Mesh, const TArray<FTransform>& Transforms, int32 CullDistanceCm,
		const TArray<UMaterialInterface*>& WindMaterials, int32 WindDistanceCm);

	/** Invisible cylinders at the trunks so the car can hit trees: base points (tile-relative) and diameters, cm. */
	void AddTrunkColliders(const TArray<FVector>& Bases, const TArray<float>& Diameters);

	/**
	 * Adds one part of the tile's street furniture (lamps, signal poles, signal heads, sign poles, one sign graphic ...),
	 * so spawning it is spread over several steps. Returns true after the last part. Near detail only.
	 */
	bool AddFurnitureStep(const FWorldFurnitureInstances& Furniture, const FFurnitureMeshes& Meshes, int32 Step);

	/** Turns on collision for the next furniture component that doesn't have it yet; false when all have it. */
	bool EnableNextFurnitureCollision();

	/** Sets the lens glow of every signal head to what its signal shows at TimeSeconds of traffic time. */
	void UpdateSignalHeads(const FTrafficNetwork& Network, double TimeSeconds);

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

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

	UInstancedStaticMeshComponent* AddFurnitureInstances(UStaticMesh* Mesh, const TArray<FTransform>& Transforms, bool bCastShadow, bool bCollision);

	/** Furniture components that block the car once the viewer is close, and how many of them already do. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> FurnitureColliders;
	int32 FurnitureCollidersEnabled = 0;

	/** Hands the tile's lamp and signal lights to the traffic subsystem and registers the signal heads. */
	void RegisterFurnitureLights(const FWorldFurnitureInstances& Furniture);

	/** The signal head component and what each of its instances currently shows. */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> SignalHeadComponent;
	TArray<int32> HeadApproachIds;
	TArray<uint8> HeadShownAspects;

	/** Snow chunks, hidden while there is no snow. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UDynamicMeshComponent>> SnowChunks;
	bool bSnowShown = true;

	TArray<FBox2D> GroundChunkBounds;
	TArray<bool> GroundChunkHasCollision;
};
