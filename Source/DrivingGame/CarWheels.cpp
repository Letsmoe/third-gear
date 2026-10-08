#include "CarWheels.h"

#include "CarSettings.h"

namespace
{
void ConfigureWheel(UChaosVehicleWheel& Wheel, bool bFront)
{
	const UCarSettings* Settings = GetDefault<UCarSettings>();
	Wheel.AxleType = bFront ? EAxleType::Front : EAxleType::Rear;
	Wheel.WheelRadius = Settings->VisualWheelRadiusCm;
	Wheel.WheelWidth = Settings->VisualWheelWidthCm;
	Wheel.bAffectedBySteering = bFront;
	Wheel.bAffectedByEngine = bFront;
	Wheel.bAffectedByBrake = true;
	Wheel.bAffectedByHandbrake = !bFront;
	Wheel.MaxSteerAngle = Settings->MaxRoadWheelAngleDeg + 5.f;
	Wheel.bABSEnabled = false;            // our own ABS
	Wheel.bTractionControlEnabled = false;

	// Suspension: Chaos' force-based spring gives SpringRate (N per cm) * compression (cm). With MaxDrop equal to the
	// static deflection the car rests exactly at the wheel bones' positions.
	const float SpringRate = FMath::Max(10.f, bFront ? Settings->SpringRateFrontNPerCm : Settings->SpringRateRearNPerCm);
	const float AxleShare = bFront ? Settings->FrontWeightFraction : 1.f - Settings->FrontWeightFraction;
	const float StaticLoadN = Settings->MassKg * 9.81f * AxleShare * 0.5f;
	Wheel.SpringRate = SpringRate;
	Wheel.SpringPreload = 0.f;
	Wheel.SuspensionMaxDrop = StaticLoadN / SpringRate;
	Wheel.SuspensionMaxRaise = Settings->SuspensionMaxRaiseCm;
	Wheel.SuspensionDampingRatio = Settings->DampingRatio;
	Wheel.WheelLoadRatio = 1.f;           // real dynamic load (load transfer), not a blend with the static load
	Wheel.RollbarScaling = bFront ? Settings->AntiRollFront : Settings->AntiRollRear;
	Wheel.SuspensionSmoothing = 0;
}
}

UCarWheelFront::UCarWheelFront()
{
	ConfigureWheel(*this, true);
}

UCarWheelRear::UCarWheelRear()
{
	ConfigureWheel(*this, false);
}
