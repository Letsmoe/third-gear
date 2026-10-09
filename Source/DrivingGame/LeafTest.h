#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LeafTest.generated.h"

class UInstancedStaticMeshComponent;

/**
 * Test patch of fallen leaf cards (-LeafTest, see Scripts/create_leaf_assets.py for the assets). Once the ground under
 * the patch has collision, scatters leaf instances on it by tracing down per leaf: evenly over the patch, plus one
 * heap in the middle where leaves lie on top of each other. Options on the command line:
 *   -LeafTestAt=x,y     centre in metres (default: the player start)
 *   -LeafCount=N        number of leaves (default 3000)
 *   -LeafRadius=m       radius of the patch (default 2.5)
 *   -LeafNanite=0|1     Nanite meshes instead of plain instanced static meshes (default 0)
 *   -LeafShadows=0|1    leaves cast shadows (default 1)
 */
UCLASS()
class DRIVINGGAME_API ALeafTestPatch : public AActor
{
	GENERATED_BODY()

public:
	ALeafTestPatch();

	virtual void BeginPlay() override;

private:
	/** Tries to scatter the leaves; keeps retrying every second until the ground below the centre has collision. */
	void TryScatter();

	/** Finds the ground under a point; false when nothing is hit. */
	bool TraceGround(const FVector& Location, FVector& OutPoint, FVector& OutNormal) const;

	/** Adds one leaf lying on the ground at Point, to one of the shape components. */
	void AddLeaf(const FVector& Point, const FVector& Normal, float HeightOffsetCm);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> ShapeComponents;

	FVector2D CentreMetres = FVector2D::ZeroVector;
	int32 LeafCount = 3000;
	float RadiusMetres = 2.5f;
	bool bUseNanite = false;
	bool bCastShadows = true;
	FRandomStream Random;
	FTimerHandle RetryHandle;
};
