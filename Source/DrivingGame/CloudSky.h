#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CloudSky.generated.h"

class AActor;
class AVolumetricCloud;
class AStaticMeshActor;
class UMaterialInstanceDynamic;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

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
};

/**
 * Clouds in the sky that cost nothing between refreshes.
 *
 * The sky (atmosphere and volumetric clouds) is rendered on demand into a cube of six render targets and a dome with
 * a sky material (M_SkyDome) shows it behind everything. A refresh starts when the weather or the sun has moved
 * enough, the viewer has moved far from where the cube was taken, or every `tg.CloudSkyRefreshSeconds`. It renders one
 * face per frame into the set of targets that is not showing, then blends from the old set to the new one over a few
 * seconds, so nothing pops. In between nothing is traced at all: the main views and the sky light's own capture have
 * the cloud switched off (the viewport's Cloud show flag), only these scene captures show it.
 *
 * The same dome also draws the atmosphere into the sky light's real-time capture (with a sky mesh in the scene the
 * engine no longer does), so the lighting of the scene keeps the sky it always had. Cloud darkening of the light comes
 * from the weather's sun and sky gain, not from the captured clouds.
 *
 * The materials come from Scripts/create_cloud_sky.py. Without them the sky stays the plain atmosphere.
 */
UCLASS()
class UCloudSkyRig : public UObject
{
	GENERATED_BODY()

public:
	/** Spawns the cloud layer, the capture rig and the dome, if the materials exist. Returns whether it did. */
	bool Initialise(UWorld* World);

	/** Feeds the weather into the cloud material, runs the refresh state machine and moves the dome to the viewer. */
	void Update(const FCloudSkyInputs& Inputs, float DeltaSeconds);

	bool IsActive() const { return Dome.IsValid(); }

	/**
	 * Debug override of a cloud material parameter, applied after the weather every frame (console command
	 * `CloudSky.Param <name> <1 to 4 numbers>`; a single number is a scalar, more are a colour or vector).
	 */
	void SetDebugParameter(FName Name, const FLinearColor& Value, bool bScalar);

	/** Writes the cloud material's vector and scalar defaults to the log (console command `CloudSky.Dump`). */
	void LogParameters() const;

	/** Starts a refresh now (console command `CloudSky.Refresh`). */
	void RequestRefresh() { bRefreshRequested = true; }

private:
	enum class ERefreshState : uint8
	{
		/** Showing one set of faces; nothing runs. */
		Idle,
		/** Rendering the faces of the hidden set, one per frame. */
		Capturing,
		/** Fading from the shown set to the freshly rendered one. */
		Blending,
	};

	/** Creates the six capture components and the two sets of render targets, and gives their textures to the dome. */
	void CreateCaptureRig(UWorld* World);

	/** Writes the weather into the cloud material. */
	void ApplyCloudParameters(const FCloudSkyInputs& Inputs);

	/** Whether the weather, the sun or the viewer has moved far enough from the last capture to render a new one. */
	bool NeedsRefresh(const FCloudSkyInputs& Inputs, const FVector& ViewerLocation) const;

	/** Renders the next face of the hidden set; moves to Blending after the sixth. */
	void CaptureNextFace();

	/** Renders one face of one set now, from the capture position. */
	void CaptureFace(int32 SetIndex, int32 FaceIndex);

	/** Advances the fade between the sets. */
	void AdvanceBlend(float DeltaSeconds);

	struct FDebugParameter
	{
		FLinearColor Value;
		bool bScalar = true;
	};
	TMap<FName, FDebugParameter> DebugParameters;

	TWeakObjectPtr<AVolumetricCloud> CloudActor;
	TWeakObjectPtr<AStaticMeshActor> Dome;
	TWeakObjectPtr<AActor> CaptureActor;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> CloudMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> DomeMaterial;

	UPROPERTY()
	TArray<TObjectPtr<USceneCaptureComponent2D>> FaceCaptures;

	/** Two sets of six faces; the dome fades between them. */
	UPROPERTY()
	TArray<TObjectPtr<UTextureRenderTarget2D>> FaceTargets;

	ERefreshState State = ERefreshState::Idle;
	bool bRefreshRequested = false;
	bool bHasCapture = false;
	/** The set the dome shows when Blend settles (0 or 1), and the one being rendered or faded in. */
	int32 ShownSet = 0;
	int32 HiddenSet = 1;
	int32 NextFace = 0;
	/** 0 shows set 0, 1 shows set 1. */
	float Blend = 0.f;
	double SecondsAtLastCapture = 0.0;
	float CapturedCover = 0.f;
	float CapturedRain = 0.f;
	FVector CapturedSunDirection = FVector::UpVector;
	FVector CapturePosition = FVector::ZeroVector;
	/** Last inputs, for a capture started by a console command. */
	FCloudSkyInputs LastInputs;
};

/**
 * Measurement for the cloud sky's budget: with `-GpuFrameLog=<seconds per window>` it logs the GPU frame time of every
 * window (average, median, 99th percentile, maximum) so the cost of the refreshes can be seen next to the steady state.
 * Call once per frame.
 */
namespace CloudSkyFrameLog
{
	void Record(UWorld* World);
}
