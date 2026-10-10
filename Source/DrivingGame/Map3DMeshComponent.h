#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "Map3DMeshBuilder.h"
#include "Map3DMeshComponent.generated.h"

/**
 * A mesh of the 3D map: unlit triangles with a palette slot and brightness per vertex (see FMap3DMeshData). It exists
 * because it is much cheaper than a dynamic mesh component for geometry that never changes: the vertices go straight
 * into render buffers and the component can drop its copy once the scene proxy has them.
 */
UCLASS()
class UMap3DMeshComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	UMap3DMeshComponent();

	/** Sets the geometry; the scene proxy copies it into render buffers when it is created. */
	void SetMeshData(const TSharedPtr<FMap3DMeshData>& InData);

	/** Drops the component's copy of the geometry, once the render buffers exist (the mesh then cannot be rebuilt). */
	void DiscardMeshData() { Data.Reset(); }

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual int32 GetNumMaterials() const override { return 1; }
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	TSharedPtr<FMap3DMeshData> Data;
	FBox LocalBounds = FBox(ForceInit);
};
