#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/DeveloperSettings.h"
#include "CarMirrors.generated.h"

class ACarPawn;
class UCameraComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneCaptureComponent2D;
class UStaticMesh;
class UStaticMeshComponent;
class UTextureRenderTarget2D;

/** One mirror of the car: where its glass is, how big, how it is aimed and how it is rendered. */
USTRUCT()
struct FCarMirrorDefinition
{
	GENERATED_BODY()

	UPROPERTY(Config, EditAnywhere, Category = "Mirror")
	FName Name;

	/** Centre of the glass in car mesh space, cm. */
	UPROPERTY(Config, EditAnywhere, Category = "Mirror")
	FVector Center = FVector::ZeroVector;

	/** Direction the glass faces (towards the driver), car mesh space. Normalised on use. */
	UPROPERTY(Config, EditAnywhere, Category = "Mirror")
	FVector Normal = FVector(-1.f, 0.f, 0.f);

	/**
	 * Direction the mirror is aimed at, car mesh space (zero = same as Normal). The City Sample interior mirror
	 * glass points straight back, which shows the passenger side to a driver sitting off-centre; the image is
	 * rendered as if the glass were turned to this normal while the glass itself stays in its housing.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Mirror")
	FVector AimNormal = FVector::ZeroVector;

	/** Distance of the quad in front of the mesh's own glass, cm, so the mesh glass does not show through. */
	UPROPERTY(Config, EditAnywhere, Category = "Mirror")
	float LiftCm = 0.6f;

	UPROPERTY(Config, EditAnywhere, Category = "Mirror")
	float WidthCm = 20.f;

	UPROPERTY(Config, EditAnywhere, Category = "Mirror")
	float HeightCm = 10.f;

	/**
	 * Widening of the field of view relative to a flat mirror of the same size. 1 = flat (interior mirror),
	 * about 1.8 for the convex door mirrors, which show a much wider view than a flat one.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Mirror")
	float FieldScale = 1.f;

	/** Corner radius of the glass outline, cm. */
	UPROPERTY(Config, EditAnywhere, Category = "Mirror")
	float CornerRadiusCm = 2.f;

	/** How often the image is refreshed, Hz. Door mirrors sit in the periphery and can run slower than the interior mirror. */
	UPROPERTY(Config, EditAnywhere, Category = "Mirror")
	float UpdateRateHz = 30.f;

	/** Resolution of the capture, pixels. */
	UPROPERTY(Config, EditAnywhere, Category = "Mirror")
	int32 ResolutionX = 512;

	UPROPERTY(Config, EditAnywhere, Category = "Mirror")
	int32 ResolutionY = 192;
};

/**
 * Mirror setup. Section [/Script/DrivingGame.CarMirrorSettings] in Config/DefaultGame.ini overrides the defaults.
 * The glass positions belong to the City Sample vehicle07 body mesh (measured from the mesh's glass triangles).
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Car mirrors"))
class DRIVINGGAME_API UCarMirrorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UCarMirrorSettings();

	UPROPERTY(Config, EditAnywhere, Category = "Mirrors")
	TArray<FCarMirrorDefinition> Mirrors;

	/** Mirror material (Scripts/create_mirror_material.py). */
	UPROPERTY(Config, EditAnywhere, Category = "Mirrors")
	TSoftObjectPtr<UMaterialInterface> GlassMaterial;

	/** Mirrors further than this from the driver's view direction are not refreshed (no mirror is in view), degrees. */
	UPROPERTY(Config, EditAnywhere, Category = "Mirrors")
	float SkipOutsideViewDegrees = 62.f;

	/** Objects further away than this are left out of the mirror images, cm. */
	UPROPERTY(Config, EditAnywhere, Category = "Mirrors")
	float MaxViewDistanceCm = 150000.f;
};

/**
 * Working mirrors: a glass quad over each mirror of the car body that shows a scene capture rendered from the
 * driver's reflected eye through the glass (planar reflection with an off-axis frustum, so the image follows the head).
 * Captures are small, run at a reduced rate with the mirrors taking turns, and leave out the expensive effects.
 * Console variable tg.Mirrors 0 switches them off (the glass then shows black).
 */
UCLASS(ClassGroup = (Car), meta = (BlueprintSpawnableComponent))
class DRIVINGGAME_API UCarMirrorsComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCarMirrorsComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	/** Everything that belongs to one mirror at runtime. */
	struct FMirror
	{
		FCarMirrorDefinition Settings;
		TObjectPtr<UStaticMeshComponent> Glass;
		TObjectPtr<USceneCaptureComponent2D> Capture;
		TObjectPtr<UTextureRenderTarget2D> RenderTarget;
		double LastCaptureTime = -1e9;
	};

	/** Creates the glass quad, render target and capture component for one mirror. */
	void CreateMirror(const FCarMirrorDefinition& Settings, UStaticMesh* QuadMesh, UMaterialInterface* GlassMaterial);

	/** Puts the capture at the driver's eye reflected in the mirror plane and builds its off-axis projection. */
	bool PrepareCapture(FMirror& Mirror, const FVector& EyeLocation) const;

	/** Cuts the capture's cost: no screen-space effects and no Lumen (shadows are switched per capture, tg.Mirrors.Shadows). */
	void ApplyCheapShowFlags(USceneCaptureComponent2D& Capture) const;

	/** Applies the console variables that can change while the game runs: shadows, sky light and resolution. */
	void ApplyPerCaptureSettings(FMirror& Mirror) const;

	/** True when the mirror is within the driver's field of view, so refreshing it is worth the cost. */
	bool IsInView(const FMirror& Mirror, const UCameraComponent& Camera) const;

	ACarPawn* GetCar() const;

	TArray<FMirror> MirrorList;
	int32 NextMirror = 0;
};
