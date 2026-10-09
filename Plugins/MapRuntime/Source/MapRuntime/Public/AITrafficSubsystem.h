#pragma once

#include "CoreMinimal.h"
#include "AITrafficSimulation.h"
#include "LaneNetwork.h"
#include "Subsystems/WorldSubsystem.h"
#include "AITrafficSubsystem.generated.h"

class AAITrafficCar;
class AWorldStreamer;
class USpotLightComponent;
struct FTrafficVehicleModel;

/** Rule violations of AI cars since the start, for the traffic test (there should be none). */
struct FAITrafficViolationTotals
{
	int32 RedLight = 0;
	int32 Speeding = 0;
	int32 AmberRun = 0;
};

/**
 * AI traffic on the streamed world: keeps a few dozen cars driving on the real streets around the viewer, obeying
 * signals, speed limits, stop and give way signs and right of way. The cars are simulated by FAITrafficSimulation along
 * the lane graph in lanes.json (Tools/osmimport/build_lanes.py), shown as AAITrafficCar actors, spawned out of sight
 * around the viewer and removed again when they are far away.
 *
 * Needs the region's traffic.json (UTrafficSubsystem) and lanes.json; without lanes.json it does nothing. -NoTraffic
 * turns it off, -TrafficCars=<n> or the console variable tg.TrafficCars sets how many cars it aims for.
 * Vehicles that aren't simulated here, like the player's car, register with RegisterExternalVehicle so AI cars see
 * them and give way to them.
 */
UCLASS()
class MAPRUNTIME_API UAITrafficSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** An actor the AI cars must keep clear of and yield to, with its body half sizes in metres. */
	void RegisterExternalVehicle(AActor* Actor, float HalfLengthM = 2.2f, float HalfWidthM = 0.95f);

	/** Whether the lane graph is loaded and cars are being simulated. */
	bool IsRunning() const { return bRunning; }

	int32 GetCarCount() const { return Simulation.GetCars().Num(); }
	const FAITrafficStats& GetStats() const { return Simulation.GetStats(); }
	FAITrafficViolationTotals GetViolationTotals() const;
	const FLaneNetwork& GetLaneNetwork() const { return Lanes; }

	/** Moves the point the traffic is kept around (tests that fly a camera along a route use the camera itself instead). */
	void SetTargetCarCount(int32 Count) { TargetCarCount = Count; }

private:
	bool TryStart();
	bool GetViewer(FVector& OutLocationCm, FVector2D& OutForward) const;
	void CollectAgents(TArray<FSimAgent>& OutAgents);
	void RemoveFarCars(const FVector& ViewerCm, const FVector2D& ViewerForward);
	int32 CountAllowedCars(const FVector& ViewerCm) const;
	void SpawnCars(const FVector& ViewerCm, const FVector2D& ViewerForward);
	bool TrySpawnOne(const FVector& ViewerCm, const FVector2D& ViewerForward);
	int32 PickModelIndex();
	FLinearColor PickPaint(const FTrafficVehicleModel& Model);
	void RemoveCar(int32 CarId);
	void UpdateActors();
	void ReportNewViolations();
	void UpdateHeadlightPool(const FVector& ViewerCm, bool bNight);

	FLaneNetwork Lanes;
	FAITrafficSimulation Simulation;
	bool bStartTried = false;
	bool bRunning = false;
	int32 TargetCarCount = 30;
	float SecondsSinceSpawn = 0.f;
	FRandomStream Random;

	TArray<TSharedPtr<FTrafficVehicleModel>> Models;
	/** The models' meshes: the model structs only hold raw pointers, so they must be referenced from here. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> ModelAssets;
	TMap<int32, int32> CarModel;

	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<AAITrafficCar>> CarActors;

	struct FExternalVehicle
	{
		TWeakObjectPtr<AActor> Actor;
		float HalfLengthM = 2.2f;
		float HalfWidthM = 0.95f;
	};
	TArray<FExternalVehicle> ExternalVehicles;

	/** How many violations of each car were already reported, and the totals of cars that are gone. */
	TMap<int32, int32> ReportedViolations;
	FAITrafficViolationTotals RemovedTotals;
	TWeakObjectPtr<AWorldStreamer> Streamer;

	/** A few real spot lights moved to the headlights of the cars nearest the viewer at night; the rest only glow. */
	UPROPERTY(Transient)
	TObjectPtr<AActor> LightHolder;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USpotLightComponent>> HeadlightPool;
};
