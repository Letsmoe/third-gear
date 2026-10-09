#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/DeveloperSettings.h"
#include "CarSimTypes.h"
#include "CarLights.generated.h"

class ACarPawn;
class UMaterialInstanceDynamic;
class USpotLightComponent;
class UStaticMeshComponent;
class UTextureLightProfile;

/** Indicator stalk position. */
UENUM()
enum class ECarIndicator : uint8
{
	Off,
	Left,
	Right,
	Hazard,
};

/**
 * Light positions and strengths of the car, section [/Script/DrivingGame.CarLightSettings] in Config/DefaultGame.ini.
 * Positions are in car mesh space (cm, +X forward, +Y right) and belong to City Sample vehicle07, measured from the
 * triangles of the mesh's light material.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Car lights"))
class DRIVINGGAME_API UCarLightSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Headlamp centre of the right headlamp; the left one is mirrored in Y. */
	UPROPERTY(Config, EditAnywhere, Category = "Positions")
	FVector HeadlampLocation = FVector(186.f, 58.f, 68.f);

	/** Tail lamp cluster, right side; also holds the brake, reverse and rear indicator lamps. */
	UPROPERTY(Config, EditAnywhere, Category = "Positions")
	FVector TailLampLocation = FVector(-214.f, 60.f, 77.f);

	/** Front indicator, right side. */
	UPROPERTY(Config, EditAnywhere, Category = "Positions")
	FVector FrontIndicatorLocation = FVector(184.f, 66.f, 61.f);

	/** The low beam is aimed this far below the horizon, degrees (the legal 1 percent is 0.57). */
	UPROPERTY(Config, EditAnywhere, Category = "Beams")
	float LowBeamAimDownDegrees = 0.57f;

	/** Peak intensity of one low beam headlamp, candela. */
	UPROPERTY(Config, EditAnywhere, Category = "Beams")
	float LowBeamCandela = 25000.f;

	/** Peak intensity of one high beam headlamp, candela. */
	UPROPERTY(Config, EditAnywhere, Category = "Beams")
	float HighBeamCandela = 60000.f;

	UPROPERTY(Config, EditAnywhere, Category = "Beams")
	float LowBeamRangeCm = 15000.f;

	UPROPERTY(Config, EditAnywhere, Category = "Beams")
	float HighBeamRangeCm = 40000.f;

	/** Light profiles of the beams (Tools/texturegen/headlamp_ies.py, imported by Scripts/import_headlight_profiles.py). */
	UPROPERTY(Config, EditAnywhere, Category = "Beams")
	TSoftObjectPtr<UTextureLightProfile> LowBeamProfile;

	UPROPERTY(Config, EditAnywhere, Category = "Beams")
	TSoftObjectPtr<UTextureLightProfile> HighBeamProfile;

	/** Roll of the profile around the lamp axis, degrees; sets which side of the beam the cut-off steps up on. */
	UPROPERTY(Config, EditAnywhere, Category = "Beams")
	float ProfileRollDegrees = 0.f;

	/** Cone angle of the headlamp spot lights; the profile shapes the beam inside it, so it must cover the widest angle of the profile. */
	UPROPERTY(Config, EditAnywhere, Category = "Beams")
	float LowBeamConeDegrees = 50.f;

	UPROPERTY(Config, EditAnywhere, Category = "Beams")
	float HighBeamConeDegrees = 25.f;

	/** Glow on the road behind the car, candela per lamp. Only the tail and brake lamp has a real light; reverse lamps and indicators are emissive only. */
	UPROPERTY(Config, EditAnywhere, Category = "Rear")
	float TailCandela = 0.02f;

	UPROPERTY(Config, EditAnywhere, Category = "Rear")
	float BrakeCandela = 0.25f;

	/** Indicator blink rate, Hz (the legal range is 1.0 to 2.0, relays click at about 1.5). */
	UPROPERTY(Config, EditAnywhere, Category = "Indicators")
	float IndicatorHz = 1.5f;

	/** The steering wheel has to be turned this far in the indicated direction before returning counts as the end of the turn, degrees. */
	UPROPERTY(Config, EditAnywhere, Category = "Indicators")
	float CancelTurnDegrees = 70.f;

	/** Steering wheel angle below which the indicator cancels itself after a turn, degrees. */
	UPROPERTY(Config, EditAnywhere, Category = "Indicators")
	float CancelReturnDegrees = 20.f;
};

/**
 * All lights of the player car: low and high beam (spot lights with IES light profiles that draw the asymmetric
 * European beam with its cut-off line), tail, brake and reverse lamps, indicators and hazards. Each switches an
 * emissive parameter on the body's light material; the beams and one small tail light per side are real lights
 * that fall on the road and surroundings.
 * The dashboard tell-tales and the indicator relay click read their state from here.
 */
UCLASS(ClassGroup = (Car), meta = (BlueprintSpawnableComponent))
class DRIVINGGAME_API UCarLightsComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCarLightsComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Feeds the driver's controls and the car state; called by the pawn every frame. */
	void UpdateFromCar(const FCarDriverInput& Input, const FCarTelemetry& Telemetry);

	void ToggleLowBeam();
	void ToggleHighBeam();
	void ToggleIndicator(ECarIndicator Direction);

	bool IsLowBeamOn() const { return bLowBeam || bHighBeam; }
	bool IsHighBeamOn() const { return bHighBeam; }
	ECarIndicator GetIndicator() const { return Indicator; }
	bool IsLeftLampOn() const { return bLeftLampOn; }
	bool IsRightLampOn() const { return bRightLampOn; }

	/** Called with true and false at every blink of the indicator lamps, for the relay click. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FIndicatorLampDelegate, bool);
	FIndicatorLampDelegate OnIndicatorLamp;

private:
	void CreateLights();
	USpotLightComponent* AddLamp(const TCHAR* Name, const FVector& Location, float YawDegrees, const FLinearColor& Colour, float RadiusCm);
	USpotLightComponent* AddHeadlamp(const TCHAR* Name, const FVector& Location, float AimDownDegrees, float RangeCm, float ConeDegrees, UTextureLightProfile* Profile);

	/** Moves the blink phase and decides when the lamps are on; cancels the indicator after a completed turn. */
	void UpdateIndicator(float DeltaTime, float SteeringWheelDeg);
	void SetLampPhase(bool bOn);

	/** Writes the light state into the body material and the real lights. */
	/** Lets the night exposure follow the beams with the adaptation time of the eye. */
	void UpdateEyeAdaptation(float DeltaTime);

	/** Shows or hides the test wall of tg.Headlights.Wall. */
	void UpdateTestWall();

	void ApplyState(float DeltaTime, const FCarDriverInput& Input, const FCarTelemetry& Telemetry);

	ACarPawn* GetCar() const;

	ECarIndicator Indicator = ECarIndicator::Off;
	bool bLowBeam = false;
	bool bHighBeam = false;
	bool bLeftLampOn = false;
	bool bRightLampOn = false;
	float BlinkClockSeconds = 0.f;
	float SteeringPeakDegrees = 0.f;
	bool bLastLampPhase = false;
	bool bForceBrakeLights = false;   // test switches from the command line (-BrakeLights, -ReverseLight)
	bool bForceReverseLight = false;

	// Smoothed brightness 0..1 of the bulbs (filaments take a moment to glow and fade).
	float BrakeGlow = 0.f;
	float LeftGlow = 0.f;
	float RightGlow = 0.f;
	float BeamGlow = 0.f;
	float ReverseGlow = 0.f;
	float HighBeamGlow = 0.f;
	float AdaptationStops = 0.f;

	FCarDriverInput LastInput;
	FCarTelemetry LastTelemetry;

	TObjectPtr<UMaterialInstanceDynamic> LightMaterial;
	TArray<TObjectPtr<USpotLightComponent>> LowBeamLights;
	TArray<TObjectPtr<USpotLightComponent>> HighBeamLights;
	TArray<TObjectPtr<USpotLightComponent>> TailLights;
	TObjectPtr<UStaticMeshComponent> TestWall;
};
