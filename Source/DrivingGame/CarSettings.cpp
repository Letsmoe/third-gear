#include "CarSettings.h"

#include "Animation/AnimInstance.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

const TArray<FName>& UCarSettings::GetUserEditableProperties()
{
	static const TArray<FName> Names = {
		GET_MEMBER_NAME_CHECKED(UCarSettings, FfbFullScaleNm),
		GET_MEMBER_NAME_CHECKED(UCarSettings, FfbMaxForce),
	};
	return Names;
}

UCarSettings::UCarSettings()
{
	// Placeholder model: the UE vehicle template sports car (copied to /Game/Vehicles/SportsCar).
	SkeletalMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(TEXT("/Game/Vehicles/SportsCar/SKM_SportsCar.SKM_SportsCar")));
	AnimClass = TSoftClassPtr<UAnimInstance>(FSoftObjectPath(TEXT("/Game/Vehicles/SportsCar/ABP_SportsCar.ABP_SportsCar_C")));
	auto Attach = [this](const TCHAR* Path, FName Socket, float Yaw)
	{
		FCarAttachedMesh& Entry = AttachedMeshes.AddDefaulted_GetRef();
		Entry.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(Path));
		Entry.Socket = Socket;
		Entry.Rotation = FRotator(0.f, Yaw, 0.f);
	};
	const TCHAR* Wheel = TEXT("/Game/Vehicles/SportsCar/SM_SportsCar_Wheel.SM_SportsCar_Wheel");
	Attach(TEXT("/Game/Vehicles/SportsCar/SM_SportsCar.SM_SportsCar"), NAME_None, 0.f);
	Attach(TEXT("/Game/Vehicles/SportsCar/SM_SportsCar_Glass.SM_SportsCar_Glass"), NAME_None, 0.f);
	Attach(Wheel, TEXT("Phys_Wheel_FL"), -90.f); // wheel bones point their Y axis down; turn the rim outwards
	Attach(Wheel, TEXT("Phys_Wheel_FR"), 90.f);
	Attach(Wheel, TEXT("Phys_Wheel_BL"), -90.f);
	Attach(Wheel, TEXT("Phys_Wheel_BR"), 90.f);
	DashboardTextMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Vehicles/Materials/M_DashboardText.M_DashboardText")));
	MenuPanelMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/UI/M_MenuPanel.M_MenuPanel")));
	WheelBones = {TEXT("Phys_Wheel_FL"), TEXT("Phys_Wheel_FR"), TEXT("Phys_Wheel_BL"), TEXT("Phys_Wheel_BR")};

	// VW EA211 1.4 TSI 150 PS (CZDA): 250 Nm from 1500 to 3500 rpm, 110 kW from 5000 to 6000 rpm.
	TorqueCurve = {
		{600.0, 100.0}, {1000.0, 150.0}, {1250.0, 200.0}, {1500.0, 250.0}, {3500.0, 250.0}, {4000.0, 245.0},
		{4500.0, 233.0}, {5000.0, 210.0}, {5500.0, 191.0}, {6000.0, 175.0}, {6500.0, 150.0}, {7000.0, 100.0},
	};
	// MQ250-6F as fitted to the Golf VII 1.4 TSI 140/150 PS.
	GearRatios = {3.778f, 2.118f, 1.360f, 1.029f, 0.857f, 0.733f};
}

FCarSimParams UCarSettings::MakeSimParams() const
{
	FCarSimParams P;
	P.MassKg = MassKg;
	P.DragAreaM2 = DragAreaM2;
	P.RollingRadiusM = RollingRadiusM;
	P.TirePeakMu = TirePeakMu;
	P.TireLateralMuScale = TireLateralMuScale;
	P.TireLoadSensitivity = TireLoadSensitivity;
	P.TirePeakSlipRatio = TirePeakSlipRatio;
	P.TirePeakSlipAngleDeg = TirePeakSlipAngleDeg;
	P.TireShapeC = TireShapeC;
	P.RollingResistance = RollingResistance;
	P.PneumaticTrailM = PneumaticTrailM;
	P.CasterTrailM = CasterTrailM;
	P.WheelInertiaFront = WheelInertiaFront;
	P.WheelInertiaRear = WheelInertiaRear;
	P.MaxBrakeTorqueFront = MaxBrakeTorqueFront;
	P.MaxBrakeTorqueRear = MaxBrakeTorqueRear;
	P.BrakePedalExponent = BrakePedalExponent;
	P.HandbrakeTorque = HandbrakeTorque;
	P.bABS = bABS;
	P.TorqueCurve.Reset();
	for (const FVector2D& Point : TorqueCurve)
	{
		P.TorqueCurve.Add(FVector2f(Point));
	}
	P.TorqueCurve.Sort([](const FVector2f& A, const FVector2f& B) { return A.X < B.X; });
	P.IdleRpm = IdleRpm;
	P.RevLimitRpm = RevLimitRpm;
	P.StallRpm = StallRpm;
	P.EngineInertia = EngineInertia;
	P.FrictionTorqueBase = FrictionTorqueBase;
	P.FrictionTorquePerKrpm = FrictionTorquePerKrpm;
	P.ThrottleExponent = ThrottleExponent;
	P.NaturallyAspiratedFraction = NaturallyAspiratedFraction;
	P.TurboLagSeconds = TurboLagSeconds;
	P.IdleMaxTorque = IdleMaxTorque;
	P.ClutchMaxTorque = ClutchMaxTorque;
	P.ClutchEngagedBelow = ClutchEngagedBelow;
	P.ClutchReleasedAbove = ClutchReleasedAbove;
	P.ClutchCurveExponent = ClutchCurveExponent;
	P.GearRatios = GearRatios;
	P.ReverseRatio = ReverseRatio;
	P.FinalDrive = FinalDrive;
	P.DrivetrainEfficiency = DrivetrainEfficiency;
	P.SteeringRatio = SteeringRatio;
	P.MaxRoadWheelAngleDeg = MaxRoadWheelAngleDeg;
	P.ComplianceSteerDegPerKN = ComplianceSteerDegPerKN;
	return P;
}
