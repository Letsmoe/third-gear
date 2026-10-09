#include "AITrafficCar.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TrafficRuleComponent.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogAITrafficCar, Log, All);

namespace
{
const FName PaintParameter(TEXT("BaseColor"));
const FName BrakeParameter(TEXT("Brake Amt LE"));
const FName HeadlightParameter(TEXT("Headlight Amt LE"));
const FName RunningParameter(TEXT("Run Amt LE"));
const FName TurnLeftParameter(TEXT("Turn Amt Left LE"));
const FName TurnRightParameter(TEXT("Turn Amt Right LE"));

UStaticMesh* LoadModelMesh(const FString& Folder, const FString& AssetName)
{
	const FString Path = FString::Printf(TEXT("/Game/CitySampleVehicles/%s/Mesh/%s.%s"), *Folder, *AssetName, *AssetName);
	return LoadObject<UStaticMesh>(nullptr, *Path);
}

int32 FindMaterialSlot(const UStaticMesh* Mesh, const TCHAR* SlotName)
{
	const TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
	for (int32 Index = 0; Index < Materials.Num(); ++Index)
	{
		if (Materials[Index].MaterialSlotName == FName(SlotName))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}
}

bool FTrafficVehicleModel::Load()
{
	if (bLoaded)
	{
		return true;
	}
	const FString Suffix = FString::Printf(TEXT("%s_%s"), *Type, *Tag);
	Body = LoadModelMesh(Folder, FString::Printf(TEXT("SM_%s_No_Wheel"), *Suffix));
	Glass = LoadModelMesh(Folder, FString::Printf(TEXT("SM_All_Trans_%s"), *Suffix));
	if (!Glass)
	{
		Glass = LoadModelMesh(Folder, FString::Printf(TEXT("SM_%s_Trans"), *Suffix));
	}
	const TCHAR* WheelNames[4] = {TEXT("Front_L"), TEXT("Front_R"), TEXT("Rear_L"), TEXT("Rear_R")};
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Wheels[Index] = LoadModelMesh(Folder, FString::Printf(TEXT("SM_Wheel_%s_%s"), WheelNames[Index], *Suffix));
		if (!Wheels[Index])
		{
			UE_LOG(LogAITrafficCar, Warning, TEXT("%s: wheel mesh %d is missing"), *Folder, Index);
			return false;
		}
		WheelCentersCm[Index] = Wheels[Index]->GetBounds().Origin;
	}
	BrakePads[0] = LoadModelMesh(Folder, FString::Printf(TEXT("SM_Brake_Pad_L_%s"), *Suffix));
	BrakePads[1] = LoadModelMesh(Folder, FString::Printf(TEXT("SM_Brake_Pad_R_%s"), *Suffix));
	if (!Body)
	{
		UE_LOG(LogAITrafficCar, Warning, TEXT("%s: body mesh is missing"), *Folder);
		return false;
	}
	WheelRadiusCm = float(Wheels[0]->GetBounds().BoxExtent.Z);
	BodyBoundsCm = Body->GetBoundingBox();
	PaintSlot = FindMaterialSlot(Body, TEXT("veh_carPaint"));
	LightSlot = FindMaterialSlot(Body, TEXT("veh_light"));

	const float FrontAxleX = WheelCentersCm[0].X;
	const float RearAxleX = WheelCentersCm[2].X;
	WheelbaseM = (FrontAxleX - RearAxleX) * 0.01f;
	FrontOverhangM = (float(BodyBoundsCm.Max.X) - FrontAxleX) * 0.01f;
	RearOverhangM = (RearAxleX - float(BodyBoundsCm.Min.X)) * 0.01f;
	LengthM = WheelbaseM + FrontOverhangM + RearOverhangM;
	WidthM = float(BodyBoundsCm.Max.Y - BodyBoundsCm.Min.Y) * 0.01f;
	HeightM = float(BodyBoundsCm.Max.Z - BodyBoundsCm.Min.Z) * 0.01f;
	bLoaded = true;
	return true;
}

void FTrafficVehicleModel::CollectAssets(TArray<TObjectPtr<UObject>>& Out) const
{
	Out.Add(Body);
	Out.Add(Glass);
	for (UStaticMesh* Wheel : Wheels)
	{
		Out.Add(Wheel);
	}
	for (UStaticMesh* Pad : BrakePads)
	{
		Out.Add(Pad);
	}
}

AAITrafficCar::AAITrafficCar()
{
	PrimaryActorTick.bCanEverTick = false;
	SetCanBeDamaged(false);

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	Root->SetMobility(EComponentMobility::Movable);

	CollisionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("CollisionBox"));
	CollisionBox->SetupAttachment(Root);
	CollisionBox->SetCollisionProfileName(TEXT("Vehicle"));
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	CollisionBox->SetGenerateOverlapEvents(false);
	CollisionBox->SetMobility(EComponentMobility::Movable);
	CollisionBox->SetCanEverAffectNavigation(false);

	VisualRoot = CreateDefaultSubobject<USceneComponent>(TEXT("VisualRoot"));
	VisualRoot->SetupAttachment(Root);

	RuleChecker = CreateDefaultSubobject<UTrafficRuleComponent>(TEXT("RuleChecker"));
	RuleChecker->bAutoActivate = false;
	RuleChecker->PrimaryComponentTick.bStartWithTickEnabled = false;
}

void AAITrafficCar::EnableRuleChecking()
{
	if (bRuleCheckingOn)
	{
		return;
	}
	bRuleCheckingOn = true;
	RuleChecker->ResetHistoryForTest();
	RuleChecker->SetComponentTickEnabled(true);
}

void AAITrafficCar::Initialize(const FTrafficVehicleModel& Model, const FLinearColor& PaintColor)
{
	const float MidX = Model.AxleMidpointXCm();
	const float GroundZ = Model.GroundZCm();
	const FVector OriginShift(MidX, 0.f, GroundZ);   // car-space point that becomes the actor origin

	// Collision box around the body, its centre lifted to the body's middle.
	const FVector BoxCentre = Model.BodyBoundsCm.GetCenter() - OriginShift;
	CollisionBox->SetBoxExtent(Model.BodyBoundsCm.GetExtent() * 0.96f, false);
	CollisionBox->SetRelativeLocation(BoxCentre);

	const auto MakeMesh = [this](const TCHAR* Name, UStaticMesh* Mesh, USceneComponent* Parent, const FVector& Location)
	{
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(this, Name);
		Component->SetupAttachment(Parent);
		Component->SetRelativeLocation(Location);
		Component->SetStaticMesh(Mesh);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
		Component->SetMobility(EComponentMobility::Movable);
		Component->RegisterComponent();
		return Component;
	};

	BodyComponent = MakeMesh(TEXT("Body"), Model.Body, VisualRoot, -OriginShift);
	if (Model.Glass)
	{
		GlassComponent = MakeMesh(TEXT("Glass"), Model.Glass, VisualRoot, -OriginShift);
	}
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FVector Centre = Model.WheelCentersCm[Index];
		const bool bFront = Index < 2;
		USceneComponent* Parent = VisualRoot;
		if (bFront)
		{
			USceneComponent* Steer = NewObject<USceneComponent>(this, *FString::Printf(TEXT("Steer%d"), Index));
			Steer->SetupAttachment(VisualRoot);
			Steer->SetRelativeLocation(Centre - OriginShift);
			Steer->RegisterComponent();
			SteerPivots[Index] = Steer;
			Parent = Steer;
			if (Model.BrakePads[Index])
			{
				MakeMesh(*FString::Printf(TEXT("Pad%d"), Index), Model.BrakePads[Index], Steer, -Centre);
			}
		}
		USceneComponent* Spin = NewObject<USceneComponent>(this, *FString::Printf(TEXT("Spin%d"), Index));
		Spin->SetupAttachment(Parent);
		Spin->SetRelativeLocation(bFront ? FVector::ZeroVector : Centre - OriginShift);
		Spin->RegisterComponent();
		SpinPivots[Index] = Spin;
		MakeMesh(*FString::Printf(TEXT("Wheel%d"), Index), Model.Wheels[Index], Spin, -Centre);
	}

	// Paint and lights get their own material instances so every car can look different.
	if (Model.PaintSlot != INDEX_NONE)
	{
		UMaterialInstanceDynamic* Paint = BodyComponent->CreateDynamicMaterialInstance(Model.PaintSlot);
		if (Paint)
		{
			Paint->SetVectorParameterValue(PaintParameter, PaintColor);
		}
	}
	if (Model.LightSlot != INDEX_NONE)
	{
		LightMaterial = BodyComponent->CreateDynamicMaterialInstance(Model.LightSlot);
	}
	RuleChecker->FrontOffsetCm = Model.FrontOverhangM * 100.f + Model.WheelbaseM * 50.f;
}

void AAITrafficCar::ApplyPose(const FVector& GroundLocationCm, const FRotator& Rotation, float SteerRadians, float WheelRollRadians)
{
	SetActorLocationAndRotation(GroundLocationCm, Rotation, false, nullptr, ETeleportType::None);
	const FRotator SteerRotation(0.f, FMath::RadiansToDegrees(SteerRadians), 0.f);
	const FRotator RollRotation(-FMath::RadiansToDegrees(WheelRollRadians), 0.f, 0.f);
	for (int32 Index = 0; Index < 2; ++Index)
	{
		if (SteerPivots[Index])
		{
			SteerPivots[Index]->SetRelativeRotation(SteerRotation);
		}
	}
	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (SpinPivots[Index])
		{
			SpinPivots[Index]->SetRelativeRotation(RollRotation);
		}
	}
}

void AAITrafficCar::SetLightScalar(UMaterialInstanceDynamic* Material, FName Parameter, float Value) const
{
	if (Material)
	{
		Material->SetScalarParameterValue(Parameter, Value);
	}
}

void AAITrafficCar::SetLights(bool bBrake, bool bHeadlights, int32 TurnSignal, bool bBlinkOn)
{
	if (!LightMaterial)
	{
		return;
	}
	const int32 TurnLamps = (TurnSignal != 0 && bBlinkOn) ? TurnSignal : 0;
	if (bLightsInitialised && bBrake == bBrakeOn && bHeadlights == bHeadlightsOn && TurnLamps == TurnLampsOn)
	{
		return;
	}
	bLightsInitialised = true;
	bBrakeOn = bBrake;
	bHeadlightsOn = bHeadlights;
	TurnLampsOn = TurnLamps;
	SetLightScalar(LightMaterial, BrakeParameter, bBrake ? 1.f : 0.f);
	SetLightScalar(LightMaterial, HeadlightParameter, bHeadlights ? 1.f : 0.f);
	SetLightScalar(LightMaterial, RunningParameter, bHeadlights ? 1.f : 0.f);
	SetLightScalar(LightMaterial, TurnLeftParameter, TurnLamps < 0 ? 1.f : 0.f);
	SetLightScalar(LightMaterial, TurnRightParameter, TurnLamps > 0 ? 1.f : 0.f);
}
