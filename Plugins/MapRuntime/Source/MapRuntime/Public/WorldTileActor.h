#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WorldTileActor.generated.h"

class UDynamicMeshComponent;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;
struct FWorldTileMeshes;
namespace UE::Geometry { class FDynamicMesh3; }

/**
 * The generated content of one world tile at one detail level: terrain and surfaces, markings and buildings as
 * dynamic meshes, plants as instances. Spawned and destroyed by AWorldStreamer; never saved.
 */
UCLASS(NotPlaceable, Transient)
class MAPRUNTIME_API AWorldTileActor : public AActor
{
	GENERATED_BODY()

public:
	AWorldTileActor();

	/**
	 * Creates the components from finished meshes (moved out of Meshes). Materials and PlantModels are indexed
	 * like Meshes.MaterialNames and by model name. With bCollision the ground and buildings block the car; with
	 * bCookNow their collision is ready when this returns instead of a few frames later.
	 */
	void Populate(FWorldTileMeshes& Meshes, const TArray<UMaterialInterface*>& Materials,
		const TMap<FString, TObjectPtr<UStaticMesh>>& PlantModels, bool bCollision, bool bCookNow);

private:
	UDynamicMeshComponent* AddMeshComponent(const TCHAR* Name, UE::Geometry::FDynamicMesh3&& Mesh,
		const TArray<UMaterialInterface*>& Materials, bool bCollision, bool bCookNow, bool bCastShadow);

	UInstancedStaticMeshComponent* AddInstances(UStaticMesh* Mesh, const TArray<FTransform>& Transforms, int32 CullDistanceCm);

	void AddTrunkColliders(const TArray<FVector>& Bases, const TArray<float>& Diameters);

	void RegisterNew(UPrimitiveComponent* Component);
};
