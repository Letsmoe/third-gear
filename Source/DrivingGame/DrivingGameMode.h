#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "DrivingGameMode.generated.h"

UCLASS()
class DRIVINGGAME_API ADrivingGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ADrivingGameMode();

	virtual void BeginPlay() override;
	virtual UClass* GetDefaultPawnClassForController_Implementation(AController* InController) override;
	virtual APawn* SpawnDefaultPawnAtTransform_Implementation(AController* NewPlayer, const FTransform& SpawnTransform) override;

	/**
	 * True when the free-fly camera pawn (ASeatedVRPawn) is used instead of the car:
	 * -FreeCam, or the screenshot mode (-Shots=...). Add -SpawnCar to also park a car at the player start.
	 */
	static bool UseFreeCamera();

private:
	/** Ground point below a player-start transform (player starts sit at eye height above the road). */
	FVector FindGroundBelow(const FVector& Location) const;

	void TakeNextShot();

	TArray<FString> PendingShots;
	int32 ShotIndex = 0;
	FString ShotName = TEXT("shot");
};
