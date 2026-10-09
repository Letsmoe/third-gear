#include "RainEffect.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"

namespace RainEffectDetail
{
	/** The rain box around the viewer, cm; must match BoxSize in Scripts/create_rain_material.py. */
	constexpr float BoxHorizontalCm = 2400.f;
	constexpr float BoxVerticalCm = 1200.f;

	/** Adds one streak: a quad with its corners in UV0 and its random start in UV1 and UV2. */
	void AddStreak(UE::Geometry::FDynamicMesh3& Mesh, const FVector3f& Start, float Random)
	{
		using namespace UE::Geometry;
		FDynamicMeshUVOverlay* Corners = Mesh.Attributes()->GetUVLayer(0);
		FDynamicMeshUVOverlay* StartXY = Mesh.Attributes()->GetUVLayer(1);
		FDynamicMeshUVOverlay* StartZ = Mesh.Attributes()->GetUVLayer(2);
		const FVector2f CornerValues[4] = {{-0.5f, 0.f}, {0.5f, 0.f}, {0.5f, 1.f}, {-0.5f, 1.f}};
		int32 Vertices[4];
		int32 CornerElements[4];
		int32 XYElements[4];
		int32 ZElements[4];
		for (int32 Index = 0; Index < 4; ++Index)
		{
			// Positions only matter for bounds; the material places every vertex.
			Vertices[Index] = Mesh.AppendVertex(FVector3d(Start.X * BoxHorizontalCm, Start.Y * BoxHorizontalCm, Start.Z * BoxVerticalCm));
			CornerElements[Index] = Corners->AppendElement(CornerValues[Index]);
			XYElements[Index] = StartXY->AppendElement(FVector2f(Start.X, Start.Y));
			ZElements[Index] = StartZ->AppendElement(FVector2f(Start.Z, Random));
		}
		const int32 Triangles[2][3] = {{0, 1, 2}, {0, 2, 3}};
		for (const int32* Triangle : Triangles)
		{
			const int32 Id = Mesh.AppendTriangle(Vertices[Triangle[0]], Vertices[Triangle[1]], Vertices[Triangle[2]]);
			if (Id < 0)
			{
				continue;
			}
			Corners->SetTriangle(Id, FIndex3i(CornerElements[Triangle[0]], CornerElements[Triangle[1]], CornerElements[Triangle[2]]));
			StartXY->SetTriangle(Id, FIndex3i(XYElements[Triangle[0]], XYElements[Triangle[1]], XYElements[Triangle[2]]));
			StartZ->SetTriangle(Id, FIndex3i(ZElements[Triangle[0]], ZElements[Triangle[1]], ZElements[Triangle[2]]));
		}
	}
}

ARainEffect::ARainEffect()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	Streaks = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("Streaks"));
	RootComponent = Streaks;
	Streaks->SetCastShadow(false);
	Streaks->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Streaks->SetMeshDrawPath(EDynamicMeshDrawPath::StaticDraw);
	Streaks->SetEnableRaytracing(false);
	Streaks->SetAffectDynamicIndirectLighting(false);
	Streaks->SetCanEverAffectNavigation(false);
}

void ARainEffect::Initialise(UMaterialInterface* Material, int32 StreakCount)
{
	using namespace UE::Geometry;
	FDynamicMesh3 Mesh;
	Mesh.EnableAttributes();
	Mesh.Attributes()->SetNumUVLayers(3);
	FRandomStream Random(1712);
	for (int32 Index = 0; Index < StreakCount; ++Index)
	{
		const FVector3f Start(Random.FRand() - 0.5f, Random.FRand() - 0.5f, Random.FRand() - 0.5f);
		RainEffectDetail::AddStreak(Mesh, Start, Random.FRand());
	}
	Streaks->GetDynamicMesh()->SetMesh(MoveTemp(Mesh));
	Streaks->SetMaterial(0, Material);
	// The material moves vertices up to half a box away from where the mesh says they are.
	Streaks->SetBoundsScale(2.f);
}

void ARainEffect::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0);
	if (Camera)
	{
		SetActorLocation(Camera->GetCameraLocation());
	}
}
