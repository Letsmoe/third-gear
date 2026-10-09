#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RainEffect.generated.h"

class UDynamicMeshComponent;
class UMaterialInterface;

/**
 * Falling rain around the viewer as one mesh of thin streaks, animated entirely in the material (M_RainStreaks).
 *
 * Every streak is a quad whose corner sits in UV0 and whose starting position in the rain box sits in UV1 and UV2
 * (box fractions, plus a random number deciding at which rain intensity the streak shows). The material moves each
 * streak down and with the wind over time, wraps it into a box around the camera and turns it to face the viewer,
 * so the CPU never touches a drop. The actor only follows the camera so the mesh is never culled.
 */
UCLASS()
class DRIVINGGAME_API ARainEffect : public AActor
{
	GENERATED_BODY()

public:
	ARainEffect();

	virtual void Tick(float DeltaSeconds) override;

	/** Builds the streak mesh with the given material and number of streaks; call once after spawning. */
	void Initialise(UMaterialInterface* Material, int32 StreakCount = 12000);

private:
	UPROPERTY()
	TObjectPtr<UDynamicMeshComponent> Streaks;
};
