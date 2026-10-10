#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WorldStructuresActor.generated.h"

class UStaticMeshComponent;

/**
 * The region's tall structures from structures.tgtile (WorldBuilder's FarStructures.h): wind turbines, pylons and
 * their conductors, and the landmarks. AWorldStreamer spawns it once at the start and keeps it, so they show from
 * as far as they can be seen, like the horizon. The turbine rotors turn; everything else stands still. Never saved.
 */
UCLASS(NotPlaceable, Transient)
class MAPRUNTIME_API AWorldStructuresActor : public AActor
{
	GENERATED_BODY()

public:
	AWorldStructuresActor();

	/** Reads the file and spawns its structures; false when the file is missing or broken (logged). */
	bool Load(const FString& Path);

	virtual void Tick(float DeltaSeconds) override;

private:
	struct FRotorState
	{
		FQuat Shaft = FQuat::Identity;
		float DegreesPerSecond = 0.f;
		float StartDegrees = 0.f;
	};

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Rotors;
	TArray<FRotorState> RotorStates;
};
