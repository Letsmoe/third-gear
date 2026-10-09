#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SnowTrackSubsystem.generated.h"

class UTexture2D;
class UTextureRenderTarget2D;

/**
 * Tyre tracks in the snow: a deformation map around the viewer that the snow layer material (M_Snow_Layer) reads.
 *
 * The map is kept on the CPU as a toroidal grid of TextureSize² texels of TexelSizeCm, so scrolling never moves data:
 * when the viewer has moved a ScrollStepTexels step, only the strips that enter the window are cleared, and tracks
 * outside the window are lost. Wheels call StampWheel every frame; the stamp sweeps from the wheel's last position
 * and presses a tyre wide track (compaction), ridges of pushed aside snow along its edges, and the direction of travel
 * (for the tread pattern). Changed regions are uploaded to a transient texture and copied into the render target asset
 * /Game/World/SnowTracks/RT_SnowTracks (made by Scripts/create_snow_track_map.py), which the material samples at
 * frac(world position / WindowSizeCm). Texture channels: R compaction, G ridge, B and A the travel direction
 * (cos and sin, 0.5 = zero).
 *
 * Tracks are stamped only while it snows (SnowCover above a threshold) and fade when the cover grows by new snowfall.
 */
UCLASS()
class MAPRUNTIME_API USnowTrackSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return false; }

	/**
	 * Presses the track of one wheel: from where this key's wheel was last frame to WheelLocationCm. Key identifies the
	 * wheel (any stable pointer). Does nothing when there is no snow or the wheel is outside the window.
	 */
	void StampWheel(const void* Key, const FVector& WheelLocationCm, float TyreWidthCm);

	static constexpr int32 TextureSize = 2048;
	static constexpr float TexelSizeCm = 5.f;
	static constexpr float WindowSizeCm = TextureSize * TexelSizeCm;

private:
	struct FWheelHistory
	{
		FVector2D LastLocationCm = FVector2D::ZeroVector;
		double LastStampTime = 0.0;
	};

	/** Moves the window to the viewer in ScrollStepTexels steps and clears the strips that enter it. */
	void FollowViewer();
	void ClearAll();
	void ClearColumns(int64 FirstWorldColumn, int64 LastWorldColumn);
	void ClearRows(int64 FirstWorldRow, int64 LastWorldRow);
	/** Scales compaction and ridges down when new snow has fallen since the last call. */
	void FadeForNewSnow(float SnowCover);
	void PressSegment(const FVector2D& FromCm, const FVector2D& ToCm, float HalfWidthCm);
	/** Marks world texels [MinX, MaxX] x [MinY, MaxY] as changed, split where the grid wraps. */
	void MarkDirty(int64 MinX, int64 MinY, int64 MaxX, int64 MaxY);
	void UploadAndCopy();
	uint8* TexelAt(int64 WorldX, int64 WorldY);

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> UploadTexture;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> TrackMap;

	TArray<uint8> Pixels;
	TArray<FIntRect> DirtyRects;
	TMap<const void*, FWheelHistory> Wheels;
	FIntPoint CenterTexel = FIntPoint(MAX_int32, MAX_int32);
	float ReferenceCover = 0.f;
	float FadeCheckCountdown = 0.f;
	float SnowCover = 0.f;
	bool bCopyNeeded = false;
};
