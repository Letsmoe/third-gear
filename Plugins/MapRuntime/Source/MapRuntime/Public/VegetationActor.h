#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VegetationActor.generated.h"

class UInstancedSkinnedMeshComponent;
class UInstancedStaticMeshComponent;
class USkinnedAsset;
class UStaticMesh;
class UTransformProviderData;

/**
 * Holds the trees/shrubs of one map area: one instanced component per vegetation model, plus invisible
 * instanced cylinders as trunk collision (so the car can hit trees without per-leaf collision).
 * Filled by the editor import script (Scripts/import_vegetation.py); everything is saved with the level.
 */
UCLASS()
class MAPRUNTIME_API AVegetationActor : public AActor
{
	GENERATED_BODY()

public:
	AVegetationActor();

	/** Removes all vegetation components created by the Add* functions. */
	UFUNCTION(BlueprintCallable, Category = "Vegetation")
	void ClearVegetation();

	/**
	 * Adds instances of a skinned (wind-animated, Nanite foliage) model. Transforms are in world space.
	 * EndCullDistance in cm (0 = never culled).
	 */
	UFUNCTION(BlueprintCallable, Category = "Vegetation")
	UInstancedSkinnedMeshComponent* AddSkinnedInstances(USkinnedAsset* Mesh, UTransformProviderData* Wind,
		const TArray<FTransform>& Transforms, int32 EndCullDistance = 0);

	/** Adds instances of a static model (bushes, props). Transforms are in world space. */
	UFUNCTION(BlueprintCallable, Category = "Vegetation")
	UInstancedStaticMeshComponent* AddStaticInstances(UStaticMesh* Mesh, const TArray<FTransform>& Transforms,
		bool bCollision, int32 EndCullDistance = 0);

	/**
	 * Invisible collision cylinders: Centers are trunk base points (world, cm), Diameters/Heights in cm.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vegetation")
	UInstancedStaticMeshComponent* AddTrunkColliders(const TArray<FVector>& BasePoints, const TArray<float>& Diameters,
		float Height = 300.f);

private:
	void RegisterNewComponent(UPrimitiveComponent* Component);
};
