#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/DeveloperSettings.h"
#include "CarSimTypes.h"
#include "InstrumentCluster.generated.h"

class ACarPawn;
class FWidgetRenderer;
class SInstrumentClusterWidget;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMeshComponent;
class UTextureRenderTarget2D;

/** Where the instrument cluster's face is in the car body and which material shows it. */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Instrument cluster"))
class DRIVINGGAME_API UInstrumentClusterSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Centre of the cluster's display in car mesh space, cm (+X forward, +Y right, +Z up). */
	UPROPERTY(Config, EditAnywhere, Category = "Face")
	FVector FaceCenter = FVector(28.2f, -37.8f, 82.2f);

	/** Direction the display faces, towards the driver. Normalised on use. */
	UPROPERTY(Config, EditAnywhere, Category = "Face")
	FVector FaceNormal = FVector(-0.94f, 0.f, 0.34f);

	UPROPERTY(Config, EditAnywhere, Category = "Face")
	float FaceWidthCm = 17.f;

	UPROPERTY(Config, EditAnywhere, Category = "Face")
	float FaceHeightCm = 6.5f;

	/** Resolution of the drawn display; the aspect ratio follows the face. */
	UPROPERTY(Config, EditAnywhere, Category = "Face")
	int32 PixelWidth = 2048;

	/** Emissive material with the texture parameter "ClusterImage" (Scripts/create_cluster_material.py). */
	UPROPERTY(Config, EditAnywhere, Category = "Face")
	TSoftObjectPtr<UMaterialInterface> ScreenMaterial;

	/** How often the display is redrawn, Hz. */
	UPROPERTY(Config, EditAnywhere, Category = "Face")
	float RedrawHz = 30.f;

	/** Odometer reading at the start of the session, km. */
	UPROPERTY(Config, EditAnywhere, Category = "Content")
	float StartOdometerKm = 31482.f;
};

/** Everything the painted cluster shows, gathered by UInstrumentClusterComponent once per redraw. */
struct FClusterState
{
	/** False with the ignition off: the display is dark. */
	bool bPowered = false;
	float SpeedKmh = 0.f;
	float Rpm = 0.f;
	/** Needle position scale 0..1 of the whole scale during the start-up sweep, negative when not sweeping. */
	float SweepFraction = -1.f;
	int32 Gear = 0;
	float CoolantFraction = 0.f;
	float FuelFraction = 0.6f;
	float OutsideTemperatureCelsius = 15.f;
	float TimeOfDayHours = 12.f;
	float OdometerKm = 0.f;
	float TripKm = 0.f;

	bool bCheckEngine = false;
	bool bBattery = false;
	bool bOilPressure = false;
	bool bHandbrake = false;
	bool bAbs = false;
	bool bAirbag = false;
	bool bLowBeam = false;
	bool bHighBeam = false;
	bool bLeftIndicator = false;
	bool bRightIndicator = false;

	/** One line for the centre display, empty for none (stalled engine, gear grind). */
	FString Message;

	/** Backlight level 0..1: dimmed a little at night. */
	float Backlight = 1.f;
};

/**
 * The Golf VII style instrument cluster: an analogue tachometer and speedometer with needles, a centre display and
 * warning lights, painted with Slate into a render target that an emissive quad shows in the cluster housing of the
 * car body. The warning lights behave as in a real car: bulb check and needle sweep at ignition, oil and battery
 * until the engine runs, handbrake while it is applied, and tell-tales for the beams and indicators.
 */
UCLASS(ClassGroup = (Car), meta = (BlueprintSpawnableComponent))
class DRIVINGGAME_API UInstrumentClusterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UInstrumentClusterComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Feeds the driver's controls and the car state; called by the pawn every frame. */
	void UpdateFromCar(const FCarDriverInput& Input, const FCarTelemetry& Telemetry);

	/** The state last painted, for tests. */
	const FClusterState& GetState() const { return State; }

private:
	/** Creates the quad, the render target and the Slate widget. */
	void CreateDisplay();

	/** Advances the bulb check, the needle sweep, the coolant temperature and the odometer. */
	void AdvanceSimulation(float DeltaTime);

	/** Fills State from the car, the lights and the weather. */
	void GatherState();

	/** Command line switches that force values onto the display, for screenshots. */
	void ApplyTestSwitches(FClusterState& InOutState) const;

	ACarPawn* GetCar() const;

	FCarDriverInput LastInput;
	FCarTelemetry LastTelemetry;
	FClusterState State;

	bool bWasPowered = false;
	float StartupSeconds = 1000.f;
	float CoolantCelsius = 20.f;
	double OdometerKm = 0.0;
	double TripKm = 0.0;
	float SecondsSinceRedraw = 1000.f;
	bool bDumped = false;

	TObjectPtr<UStaticMeshComponent> Face;
	TObjectPtr<UMaterialInstanceDynamic> ScreenMaterial;
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;
	TSharedPtr<FWidgetRenderer> WidgetRenderer;
	TSharedPtr<SInstrumentClusterWidget> Widget;
	FVector2D DrawSize = FVector2D::ZeroVector;
};
