#pragma once

#include "CoreMinimal.h"
#include "ChaosVehicleWheel.h"
#include "CarWheels.generated.h"

/**
 * Chaos wheel setups. Only geometry and suspension are used from these (the tyre and drivetrain
 * model is our own, see FCarDrivetrain); values come from UCarSettings.
 */
UCLASS()
class DRIVINGGAME_API UCarWheelFront : public UChaosVehicleWheel
{
	GENERATED_BODY()

public:
	UCarWheelFront();
};

UCLASS()
class DRIVINGGAME_API UCarWheelRear : public UChaosVehicleWheel
{
	GENERATED_BODY()

public:
	UCarWheelRear();
};
