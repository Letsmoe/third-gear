#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CloudSky.generated.h"

class AVolumetricCloud;
class AStaticMeshActor;
class UMaterialInstanceDynamic;
class USkyLightComponent;

/** What the cloud sky needs from the weather each frame. */
struct FCloudSkyInputs
{
	/** 0 clear to 1 closed deck. */
	float CloudCover = 0.f;
	/** 0 to 1, rain intensity; rain clouds are thicker and darker. */
	float RainIntensity = 0.f;
	/** Wind at cloud height in Unreal axes (X east, Y south), metres per second. */
	FVector2f Wind = FVector2f::ZeroVector;
	/** Seconds on a clock that keeps running, for the drift. */
	double Seconds = 0.0;
	/** Unit vector from the viewer towards the sun. */
	FVector SunDirection = FVector::UpVector;
	/** How much of the sun's disk shows through the cloud in front of it, 0 to 1. */
	float SunVisibility = 1.f;
};

/**
 * Clouds in the sky without tracing them for every view.
 *
 * The engine already has the cheap path: a real-time SkyLight capture renders the sky atmosphere and the volumetric
 * clouds into a cubemap, time-sliced over several frames (r.SkyLight.RealTimeReflectionCapture.TimeSlice, a few cube
 * faces per frame, clouds at reduced resolution). That cubemap lights the scene (Lumen and the sky light), so clouds
 * darken the ground consistently. This class only makes it visible:
 *
 *  - a VolumetricCloud that renders into the capture but not into the main view (bRenderInMainPass off), with a cloud
 *    material driven by the weather: cover, rain, wind drift;
 *  - a dome mesh with a sky material (M_SkyDome) that shows the captured cubemap behind everything. It follows the
 *    viewer and is far enough out to stay behind the horizon tiles.
 *
 * The materials come from Scripts/create_cloud_sky.py. Without them the sky stays the plain atmosphere.
 */
UCLASS()
class UCloudSkyRig : public UObject
{
	GENERATED_BODY()

public:
	/** Spawns the cloud layer and the dome, if the materials exist and `tg.CloudSky` is on. Returns whether it did. */
	bool Initialise(UWorld* World);

	/** Moves the dome to the viewer and feeds the weather into the cloud material. */
	void Update(const FCloudSkyInputs& Inputs);

	bool IsActive() const { return Dome.IsValid(); }

	/**
	 * Debug override of a cloud material parameter, applied after the weather every frame (console command
	 * `CloudSky.Param <name> <1 to 4 numbers>`; a single number is a scalar, more are a colour or vector).
	 */
	void SetDebugParameter(FName Name, const FLinearColor& Value, bool bScalar);

	/** Writes the cloud material's vector and scalar defaults to the log (console command `CloudSky.Dump`). */
	void LogParameters() const;

private:
	/** Capture resolution and cloud cost settings of the sky light's real-time capture. */
	void ConfigureCapture();

	struct FDebugParameter
	{
		FLinearColor Value;
		bool bScalar = true;
	};
	TMap<FName, FDebugParameter> DebugParameters;

	TWeakObjectPtr<AVolumetricCloud> CloudActor;
	TWeakObjectPtr<AStaticMeshActor> Dome;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> CloudMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> DomeMaterial;
};

/**
 * Measurement for the cloud sky's budget: with `-GpuFrameLog=<seconds per window>` it logs the GPU frame time of every
 * window (average, median, 99th percentile, maximum) so the cost of the capture's refreshes can be seen next to the
 * steady state. Call once per frame.
 */
namespace CloudSkyFrameLog
{
	void Record(UWorld* World);
}
