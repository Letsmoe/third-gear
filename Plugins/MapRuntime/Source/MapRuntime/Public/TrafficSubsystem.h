#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "TrafficNetwork.h"
#include "TrafficSubsystem.generated.h"

class AWorldTileActor;
class UMaterialParameterCollection;
class UPointLightComponent;
class USpotLightComponent;

/** A street lamp or signal head that may get a real light while the viewer is near. Positions in world cm. */
struct FStreetLightSource
{
	FVector LocationCm = FVector::ZeroVector;
	FVector Direction = FVector::DownVector;
	/** INDEX_NONE for a street lamp, else the approach id of the signal head. */
	int32 ApproachId = INDEX_NONE;
	float LensOffsetsCm[3] = {};
};

/**
 * Traffic signals, speed limits and the lights of street furniture for the generated world.
 *
 * The streamer hands over the region's traffic.json (junctions, approaches, phases, speed limits) when it starts.
 * Other systems ask questions through the query functions: which signal governs the lane ahead and what it shows,
 * what the speed limit is here. The signal cycles are functions of traffic time (game time unless -TrafficTimeOffset=
 * shifts it), so any vehicle can predict a signal ahead.
 *
 * It also lights the street: the lens glow of signal heads follows their aspect, and a small pool of real lights is
 * moved to the lamps and lit lenses nearest the viewer. Lamps are on when the sun is below the horizon; the console
 * variable tg.Night (0 to 1, negative = automatic) overrides this, and tg.NightScene sets up a night test scene.
 */
UCLASS()
class MAPRUNTIME_API UTrafficSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Loads <WorldDir>/traffic.json; called by the world streamer. False when the file is missing. */
	bool LoadRegion(const FString& WorldDir);

	const FTrafficNetwork& GetNetwork() const { return Network; }

	/** The world data folder LoadRegion read, so other systems can load their own files from it; empty before. */
	const FString& GetRegionDirectory() const { return RegionDirectory; }

	/** Seconds of traffic time: what the signal cycles run on. */
	double GetTrafficTime() const;

	/** Shifts traffic time against game time (tests set up a signal state at a chosen moment this way). */
	void SetTimeOffsetSeconds(double Offset) { TimeOffsetSeconds = Offset; }

	/**
	 * The signal a vehicle at Location (world cm) heading Forward is approaching, within MaxDistanceCm ahead
	 * (or BehindCm past the stop line). False when no signalised approach lies ahead.
	 */
	bool FindSignalAhead(const FVector& Location, const FVector& Forward, FApproachQuery& Out, float MaxDistanceCm = 15000.f,
		float BehindCm = 1000.f, int32 PreferApproachId = INDEX_NONE) const;

	/** The speed limit of the road at Location for a vehicle heading Forward; LimitKmh 0 means no limit. */
	FSpeedLimitResult GetSpeedLimit(const FVector& Location, const FVector& Forward) const;

	/** Tile actors with signal heads register to have their lenses updated, and to stop when they go away. */
	void RegisterSignalTile(AWorldTileActor* Tile);
	void UnregisterSignalTile(AWorldTileActor* Tile);

	/** Lights of a tile's lamps and signal heads (world cm), keyed by the tile actor. */
	void AddLightSources(const AActor* Owner, TArray<FStreetLightSource>&& Sources);
	void RemoveLightSources(const AActor* Owner);

	/** 0 in daylight, 1 at night: how far the street lamps are lit. */
	float GetNightFactor() const { return NightFactor; }

	/** The car's headlight pose for the retroreflective signs (world cm); with Candela 0 the signs are not lit. */
	void SetHeadlight(const FVector& LocationCm, const FVector& Direction, float Candela);

private:
	void UpdateNightFactor();
	void ApplyNightScene();
	void UpdateFakeHeadlights();
	void UpdateSignalLenses();
	void UpdateLightPool();
	void EnsureLightPool();
	void PushMaterialParameters();
	bool GetViewerLocation(FVector& OutLocation) const;

	FTrafficNetwork Network;
	FString RegionDirectory;
	double TimeOffsetSeconds = 0.0;
	float NightFactor = 0.f;
	float SecondsSinceSlowUpdate = 1.f;
	float SecondsSinceSignalUpdate = 1.f;
	bool bNightSceneApplied = false;

	TArray<TWeakObjectPtr<AWorldTileActor>> SignalTiles;
	TMap<const AActor*, TArray<FStreetLightSource>> LightSources;

	UPROPERTY(Transient)
	TObjectPtr<AActor> LightHolder;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USpotLightComponent>> LampLights;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UPointLightComponent>> LensLights;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USpotLightComponent>> HeadlightLights;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> Collection;
	/** Which lamp (by owner and index) each pooled light shows, so lights stay put while the viewer moves. */
	TArray<TPair<const AActor*, int32>> LampAssignments;

	FVector HeadlightLocation = FVector::ZeroVector;
	FVector HeadlightDirection = FVector::ForwardVector;
	float HeadlightCandela = 0.f;
};
