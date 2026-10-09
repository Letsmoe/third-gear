#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AITrafficCar.generated.h"

class UBoxComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;
class USceneComponent;
class UTrafficRuleComponent;

/**
 * A City Sample vehicle as AI traffic uses it: the static meshes (modelled in car space, wheels around their own
 * centres) and the sizes derived from them. Loaded once per model; the geometry is read from the meshes' bounds, so a
 * model needs no hand-entered numbers.
 */
struct MAPRUNTIME_API FTrafficVehicleModel
{
	/** Content folder under /Game/CitySampleVehicles, e.g. vehicle07_Car. */
	FString Folder;
	/** Asset name parts: SM_<Type>_<Tag>_No_Wheel and so on, e.g. vehCar and vehicle07. */
	FString Type;
	FString Tag;
	/** How often this model is picked relative to the others. */
	float Weight = 1.f;
	/** Fixed paint (a taxi is ivory), else a colour typical for German roads is picked. */
	bool bFixedPaint = false;
	FLinearColor FixedPaint = FLinearColor::White;

	// Filled by Load()
	bool bLoaded = false;
	UStaticMesh* Body = nullptr;
	UStaticMesh* Glass = nullptr;
	UStaticMesh* Wheels[4] = {};
	UStaticMesh* BrakePads[2] = {};
	/** Wheel centres in car space, cm: front left, front right, rear left, rear right. */
	FVector WheelCentersCm[4] = {};
	float WheelRadiusCm = 31.f;
	FBox BodyBoundsCm = FBox(ForceInit);
	/** Slot index of the paint and the lights in the body mesh's materials, or INDEX_NONE. */
	int32 PaintSlot = INDEX_NONE;
	int32 LightSlot = INDEX_NONE;
	/** Clean paint instance of this model from /Game/Vehicles/TrafficPaint (Scripts/create_traffic_paint.py), or null for the stock one. */
	UMaterialInterface* TrafficPaint = nullptr;

	// Derived, metres
	float LengthM = 4.4f;
	float WidthM = 1.85f;
	float HeightM = 1.5f;
	float WheelbaseM = 2.7f;
	/** Distance from the front axle to the front bumper and from the rear axle to the rear bumper. */
	float FrontOverhangM = 0.8f;
	float RearOverhangM = 0.9f;

	/** Loads the meshes and measures them; false when an asset is missing. Safe to call repeatedly. */
	bool Load();

	/** Adds the loaded meshes to Out, so the owner can keep them from being garbage collected between cars. */
	void CollectAssets(TArray<TObjectPtr<UObject>>& Out) const;

	/** Car-space x of the point halfway between the axles, cm: the origin of the traffic car actor. */
	float AxleMidpointXCm() const { return (WheelCentersCm[0].X + WheelCentersCm[2].X) * 0.5f; }

	/** Car-space z of the ground under the wheels, cm. */
	float GroundZCm() const { return WheelCentersCm[0].Z - WheelRadiusCm; }
};

/**
 * The look and the collision of one AI car: body, glass, four wheels that steer and roll, brake pads, a box the
 * player can hit and a rule checker. The actor's origin is on the ground halfway between the axles, x forward.
 * UAITrafficSubsystem moves it; it has no tick of its own.
 */
UCLASS(NotPlaceable, Transient)
class MAPRUNTIME_API AAITrafficCar : public AActor
{
	GENERATED_BODY()

public:
	AAITrafficCar();

	/** Builds the meshes of a model and paints it. Call once, right after spawning. */
	void Initialize(const FTrafficVehicleModel& Model, const FLinearColor& PaintColor);

	/** Presses the four wheels' tracks into the snow (USnowTrackSubsystem). */
	void StampSnowTracks();

	/** Places the car: origin on the ground (cm), orientation, front wheel steering angle and how far the wheels rolled (radians). */
	void ApplyPose(const FVector& GroundLocationCm, const FRotator& Rotation, float SteerRadians, float WheelRollRadians);

	/** Brake lights, headlights (with the tail lights), indicator lamps and the blink phase. TurnSignal is -1 left, 0 off, +1 right. */
	void SetLights(bool bBrake, bool bHeadlights, int32 TurnSignal, bool bBlinkOn);

	UTrafficRuleComponent* GetRuleChecker() const { return RuleChecker; }

	/** Starts the rule checker; a new car waits a moment so it doesn't judge its first, speedless frame. Does nothing when already on. */
	void EnableRuleChecking();

private:
	void SetLightScalar(UMaterialInstanceDynamic* Material, FName Parameter, float Value) const;

	UPROPERTY()
	TObjectPtr<USceneComponent> VisualRoot;
	UPROPERTY()
	TObjectPtr<UBoxComponent> CollisionBox;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> BodyComponent;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> GlassComponent;
	UPROPERTY()
	TObjectPtr<USceneComponent> SteerPivots[2];

	/** Wheel centres relative to the actor origin (on the ground, mid axles), cm. */
	FVector WheelOffsetsCm[4] = {};
	UPROPERTY()
	TObjectPtr<USceneComponent> SpinPivots[4];
	UPROPERTY()
	TObjectPtr<UTrafficRuleComponent> RuleChecker;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> LightMaterial;

	bool bBrakeOn = false;
	bool bHeadlightsOn = false;
	int32 TurnLampsOn = 0;
	bool bLightsInitialised = false;
	bool bRuleCheckingOn = false;
};
