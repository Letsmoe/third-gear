#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "SeatedVRPawn.generated.h"

class UCameraComponent;
class USceneComponent;

/**
 * Minimal seated viewpoint. The root marks where the driver's eyes should be; the HMD
 * moves the camera relative to it. Without an HMD, the mouse looks around for desktop testing.
 * Will later be replaced by (or attached to) the vehicle's driver seat.
 */
UCLASS()
class DRIVINGGAME_API ASeatedVRPawn : public APawn
{
	GENERATED_BODY()

public:
	ASeatedVRPawn();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	/** Re-centres the seated tracking origin on the current head pose. */
	UFUNCTION(BlueprintCallable, Category = "VR")
	void Recenter();

protected:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> EyeOrigin;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;

private:
	void LookYaw(float Value);
	void LookPitch(float Value);
	bool IsHMDActive() const;
};
