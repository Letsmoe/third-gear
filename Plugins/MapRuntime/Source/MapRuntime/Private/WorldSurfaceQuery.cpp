#include "WorldSurfaceQuery.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/HitResult.h"
#include "Materials/MaterialInterface.h"
#include "UDynamicMesh.h"

FName FindWorldSurfaceName(const FHitResult& Hit)
{
	const UDynamicMeshComponent* Component = Cast<UDynamicMeshComponent>(Hit.Component.Get());
	if (!Component || !Component->GetDynamicMesh())
	{
		return NAME_None;
	}
	const UE::Geometry::FDynamicMesh3& Mesh = Component->GetDynamicMesh()->GetMeshRef();
	if (!Mesh.HasAttributes() || !Mesh.Attributes()->HasMaterialID() || !Mesh.IsTriangle(Hit.FaceIndex))
	{
		return NAME_None;
	}
	const int32 Slot = Mesh.Attributes()->GetMaterialID()->GetValue(Hit.FaceIndex);
	const UMaterialInterface* Material = Component->GetMaterial(Slot);
	if (!Material)
	{
		return NAME_None;
	}
	FString Name = Material->GetName();
	Name.RemoveFromStart(TEXT("M_"));
	return FName(*Name);
}
