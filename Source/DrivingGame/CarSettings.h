#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CarSimTypes.h"
#include "CarSettings.generated.h"

class UAnimInstance;
class USkeletalMesh;
class UStaticMesh;
class UMaterialInterface;

/** A static mesh attached to the car's skeletal mesh (body panels, glass, wheels...). */
USTRUCT()
struct FCarAttachedMesh
{
	GENERATED_BODY()

	UPROPERTY(Config, EditAnywhere, Category = "Car")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** Bone or socket to attach to; None = mesh root. */
	UPROPERTY(Config, EditAnywhere, Category = "Car")
	FName Socket;

	UPROPERTY(Config, EditAnywhere, Category = "Car")
	FRotator Rotation = FRotator::ZeroRotator;

	/** Offset from the socket, cm. City Sample wheels are modelled in car space: offset = -(wheel bone position). */
	UPROPERTY(Config, EditAnywhere, Category = "Car")
	FVector Location = FVector::ZeroVector;
};

/**
 * Everything about the player car: which assets it uses and all physics/tuning values.
 * Section [/Script/DrivingGame.CarSettings] in Config/DefaultGame.ini overrides these defaults, so tuning needs no rebuild.
 * Defaults: placeholder mesh = UE vehicle template sports car, physics = VW Golf VII 1.4 TSI 150 PS, 6-speed manual (MQ250).
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Car"))
class DRIVINGGAME_API UCarSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UCarSettings();

	FCarSimParams MakeSimParams() const;

	/** Names of the properties the in-game settings menu edits and saves as per-user overrides (the force feedback feel). */
	static const TArray<FName>& GetUserEditableProperties();

	// ---------------- Assets (swap these to change the car model) ----------------

	/** Skeletal mesh with a physics asset; needs one bone per wheel. */
	UPROPERTY(Config, EditAnywhere, Category = "Assets")
	TSoftObjectPtr<USkeletalMesh> SkeletalMesh;

	/** Animation blueprint that rotates/steers the wheel bones (a Chaos "Wheel Controller" node). */
	UPROPERTY(Config, EditAnywhere, Category = "Assets")
	TSoftClassPtr<UAnimInstance> AnimClass;

	/** Static meshes attached to the skeletal mesh (body, glass, wheels on the wheel bones...). */
	UPROPERTY(Config, EditAnywhere, Category = "Assets")
	TArray<FCarAttachedMesh> AttachedMeshes;

	/** Wheel bones in the order front-left, front-right, rear-left, rear-right. */
	UPROPERTY(Config, EditAnywhere, Category = "Assets")
	TArray<FName> WheelBones;

	/** Radius of the wheel mesh, cm. Used for suspension traces and to sit the wheel on the ground visually. */
	UPROPERTY(Config, EditAnywhere, Category = "Assets")
	float VisualWheelRadiusCm = 39.3f;

	UPROPERTY(Config, EditAnywhere, Category = "Assets")
	float VisualWheelWidthCm = 30.f;

	/** Height of the mesh origin above the ground when the car rests on its wheels, cm (wheel radius - wheel bone height). */
	UPROPERTY(Config, EditAnywhere, Category = "Assets")
	float OriginHeightAboveGroundCm = 14.3f;

	/** Driver's eye position relative to the mesh origin, cm (x forward, y right, z up). Left-hand drive. */
	UPROPERTY(Config, EditAnywhere, Category = "Assets")
	FVector DriverEyeLocation = FVector(5.f, -37.f, 101.f);

	/** Speed/rpm/gear readout in front of the driver, relative to the mesh origin. */
	UPROPERTY(Config, EditAnywhere, Category = "Assets")
	FVector DashboardLocation = FVector(62.f, -37.f, 78.f);

	/** Emissive text material for the readout (Scripts/vehicle_materials.py). */
	UPROPERTY(Config, EditAnywhere, Category = "Assets")
	TSoftObjectPtr<UMaterialInterface> DashboardTextMaterial;

	/** Exposure-independent translucent material for the menu panel floating in front of the driver in VR (Scripts/create_menu_material.py). */
	UPROPERTY(Config, EditAnywhere, Category = "Assets")
	TSoftObjectPtr<UMaterialInterface> MenuPanelMaterial;

	// ---------------- Chassis ----------------

	UPROPERTY(Config, EditAnywhere, Category = "Chassis")
	float MassKg = 1300.f;

	/** Share of the weight on the front axle (Golf VII: ~0.61). */
	UPROPERTY(Config, EditAnywhere, Category = "Chassis")
	float FrontWeightFraction = 0.61f;

	/** Centre of mass height above the ground, cm. */
	UPROPERTY(Config, EditAnywhere, Category = "Chassis")
	float CenterOfMassHeightCm = 55.f;

	/** Desired moments of inertia (roll, pitch, yaw), kg m^2. */
	UPROPERTY(Config, EditAnywhere, Category = "Chassis")
	FVector InertiaKgM2 = FVector(500.f, 1900.f, 2050.f);

	/** Cd * frontal area, m^2 (Golf VII: ~0.30 * 2.2). */
	UPROPERTY(Config, EditAnywhere, Category = "Chassis")
	float DragAreaM2 = 0.67f;

	// ---------------- Suspension ----------------

	/** Wheel rate, N per cm of travel (front ~1.4 Hz, rear ~1.6 Hz with the sprung masses). */
	UPROPERTY(Config, EditAnywhere, Category = "Suspension")
	float SpringRateFrontNPerCm = 300.f;

	UPROPERTY(Config, EditAnywhere, Category = "Suspension")
	float SpringRateRearNPerCm = 260.f;

	/** Fraction of critical damping. */
	UPROPERTY(Config, EditAnywhere, Category = "Suspension")
	float DampingRatio = 0.35f;

	/** Bump travel above the resting position, cm. */
	UPROPERTY(Config, EditAnywhere, Category = "Suspension")
	float SuspensionMaxRaiseCm = 8.f;

	UPROPERTY(Config, EditAnywhere, Category = "Suspension")
	float AntiRollFront = 0.3f;

	UPROPERTY(Config, EditAnywhere, Category = "Suspension")
	float AntiRollRear = 0.2f;

	// ---------------- Tyres (205/55 R16) ----------------

	UPROPERTY(Config, EditAnywhere, Category = "Tyres")
	float RollingRadiusM = 0.31f;

	/** Peak longitudinal friction coefficient (braking/traction) on dry asphalt. */
	UPROPERTY(Config, EditAnywhere, Category = "Tyres")
	float TirePeakMu = 1.1f;

	/** Lateral peak friction relative to longitudinal (road tyres grip a little less sideways). */
	UPROPERTY(Config, EditAnywhere, Category = "Tyres")
	float TireLateralMuScale = 0.9f;

	UPROPERTY(Config, EditAnywhere, Category = "Tyres")
	float TireLoadSensitivity = 0.1f;

	UPROPERTY(Config, EditAnywhere, Category = "Tyres")
	float TirePeakSlipRatio = 0.12f;

	UPROPERTY(Config, EditAnywhere, Category = "Tyres")
	float TirePeakSlipAngleDeg = 8.f;

	UPROPERTY(Config, EditAnywhere, Category = "Tyres")
	float TireShapeC = 1.3f;

	UPROPERTY(Config, EditAnywhere, Category = "Tyres")
	float RollingResistance = 0.011f;

	UPROPERTY(Config, EditAnywhere, Category = "Tyres")
	float PneumaticTrailM = 0.035f;

	UPROPERTY(Config, EditAnywhere, Category = "Tyres")
	float CasterTrailM = 0.02f;

	UPROPERTY(Config, EditAnywhere, Category = "Tyres")
	float WheelInertiaFront = 1.2f;

	UPROPERTY(Config, EditAnywhere, Category = "Tyres")
	float WheelInertiaRear = 1.0f;

	// ---------------- Brakes ----------------

	UPROPERTY(Config, EditAnywhere, Category = "Brakes")
	float MaxBrakeTorqueFront = 2200.f;

	UPROPERTY(Config, EditAnywhere, Category = "Brakes")
	float MaxBrakeTorqueRear = 1000.f;

	UPROPERTY(Config, EditAnywhere, Category = "Brakes")
	float BrakePedalExponent = 1.3f;

	UPROPERTY(Config, EditAnywhere, Category = "Brakes")
	float HandbrakeTorque = 1500.f;

	UPROPERTY(Config, EditAnywhere, Category = "Brakes")
	bool bABS = true;

	// ---------------- Engine (EA211 1.4 TSI, 110 kW, 250 Nm) ----------------

	/** Full-load torque curve: X = rpm, Y = Nm. */
	UPROPERTY(Config, EditAnywhere, Category = "Engine")
	TArray<FVector2D> TorqueCurve;

	UPROPERTY(Config, EditAnywhere, Category = "Engine")
	float IdleRpm = 800.f;

	UPROPERTY(Config, EditAnywhere, Category = "Engine")
	float RevLimitRpm = 6400.f;

	UPROPERTY(Config, EditAnywhere, Category = "Engine")
	float StallRpm = 350.f;

	UPROPERTY(Config, EditAnywhere, Category = "Engine")
	float EngineInertia = 0.15f;

	UPROPERTY(Config, EditAnywhere, Category = "Engine")
	float FrictionTorqueBase = 18.f;

	UPROPERTY(Config, EditAnywhere, Category = "Engine")
	float FrictionTorquePerKrpm = 6.f;

	UPROPERTY(Config, EditAnywhere, Category = "Engine")
	float ThrottleExponent = 1.4f;

	UPROPERTY(Config, EditAnywhere, Category = "Engine")
	float NaturallyAspiratedFraction = 0.55f;

	UPROPERTY(Config, EditAnywhere, Category = "Engine")
	float TurboLagSeconds = 0.35f;

	UPROPERTY(Config, EditAnywhere, Category = "Engine")
	float IdleMaxTorque = 60.f;

	// ---------------- Clutch + gearbox (MQ250-6F) ----------------

	UPROPERTY(Config, EditAnywhere, Category = "Transmission")
	float ClutchMaxTorque = 380.f;

	/** Clutch pedal position (0 released, 1 floored) below which the clutch is fully engaged. */
	UPROPERTY(Config, EditAnywhere, Category = "Transmission")
	float ClutchEngagedBelow = 0.25f;

	/** ... and above which it is fully disengaged. The bite point lies just below this. */
	UPROPERTY(Config, EditAnywhere, Category = "Transmission")
	float ClutchReleasedAbove = 0.8f;

	UPROPERTY(Config, EditAnywhere, Category = "Transmission")
	float ClutchCurveExponent = 2.f;

	UPROPERTY(Config, EditAnywhere, Category = "Transmission")
	TArray<float> GearRatios;

	UPROPERTY(Config, EditAnywhere, Category = "Transmission")
	float ReverseRatio = 3.6f;

	UPROPERTY(Config, EditAnywhere, Category = "Transmission")
	float FinalDrive = 3.647f;

	UPROPERTY(Config, EditAnywhere, Category = "Transmission")
	float DrivetrainEfficiency = 0.92f;

	// ---------------- Steering + force feedback ----------------

	/** Steering wheel angle / road wheel angle (Golf VII: ~13.6, 2.76 turns lock to lock). */
	UPROPERTY(Config, EditAnywhere, Category = "Steering")
	float SteeringRatio = 13.6f;

	UPROPERTY(Config, EditAnywhere, Category = "Steering")
	float MaxRoadWheelAngleDeg = 36.f;

	/** Front compliance steer: road wheel angle lost per kN of front axle side force (rubber bushings); main understeer knob. */
	UPROPERTY(Config, EditAnywhere, Category = "Steering")
	float ComplianceSteerDegPerKN = 0.2f;

	/** Electric power steering: fraction of the rack torque the motor takes over, at standstill / above 100 km/h. */
	UPROPERTY(Config, EditAnywhere, Category = "Steering")
	float PowerSteeringAssistLowSpeed = 0.85f;

	UPROPERTY(Config, EditAnywhere, Category = "Steering")
	float PowerSteeringAssistHighSpeed = 0.75f;

	/** Torque at the driver's hands (Nm) that maps to 100 % of the wheel's motor. */
	UPROPERTY(Config, EditAnywhere, Category = "Steering")
	float FfbFullScaleNm = 6.f;

	/** Hard cap on the force feedback output, 0..1. */
	UPROPERTY(Config, EditAnywhere, Category = "Steering")
	float FfbMaxForce = 0.8f;

	/** Damping (normalised force per rad/s of wheel rotation) when parked and when moving; tyre scrub makes parking heavy. */
	UPROPERTY(Config, EditAnywhere, Category = "Steering")
	float FfbDampingParked = 0.12f;

	UPROPERTY(Config, EditAnywhere, Category = "Steering")
	float FfbDampingMoving = 0.05f;

	/** Coulomb friction (normalised force) in the steering column. */
	UPROPERTY(Config, EditAnywhere, Category = "Steering")
	float FfbFrictionParked = 0.1f;

	UPROPERTY(Config, EditAnywhere, Category = "Steering")
	float FfbFrictionMoving = 0.03f;
};
