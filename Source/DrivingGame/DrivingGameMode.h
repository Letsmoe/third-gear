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
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;

	/**
	 * True when the free-fly camera pawn (ASeatedVRPawn) is used instead of the car:
	 * -FreeCam, the screenshot mode (-Shots=...) or the streaming test (-StreamTest=...). Add -SpawnCar to also
	 * park a car at the player start.
	 */
	static bool UseFreeCamera();

	/** True if this level uses the free-fly camera pawn: UseFreeCamera(), the start menu, or a free camera chosen in the menu. */
	bool UsesFreeCameraPawn() const;

private:
	/** Ground point below a player-start transform (player starts sit at eye height above the road). */
	FVector FindGroundBelow(const FVector& Location) const;

	/** Moves the camera to the next -Shots entry, runs its console commands and loads the world around it. */
	void TakeNextShot();

	/** Waits until everything the view needs is compiled and streamed, then captures it (see TakeNextShot). */
	void SettleAndCapture();

	/** Saves the current view as the next numbered screenshot and moves on to the next shot. */
	void CaptureShot();

	/** Blocks until the shaders the current view asked for are compiled and its textures are streamed in. */
	void WaitForShadersAndTextures() const;

	/** Saves the driver's view (-SeatShot) and quits. */
	void CaptureSeatShot();

	/** The level's world streamer, if the world is generated at runtime. */
	class AWorldStreamer* FindWorldStreamer() const;

	/** Player start placed where the generated world's data says the drive starts. */
	UPROPERTY(Transient)
	TObjectPtr<AActor> GeneratedStart;

	TArray<FString> PendingShots;
	int32 ShotIndex = 0;
	/** Seconds the renderer gets after shaders and textures are ready (Lumen and TSR history), -ShotSettle=. */
	float ShotSettleSeconds = 2.5f;
	FString ShotName = TEXT("shot");
};
