#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Map3DTileBuilder.h"
#include "Subsystems/WorldSubsystem.h"
#include "Map3DSubsystem.generated.h"

struct FMapPalette;
class FPreviewScene;
struct FSlateBrush;
class FSlateWindowElementList;
class UMap3DMeshComponent;
class UMaterialInstanceDynamic;
class UMinimapSubsystem;
class USceneCaptureComponent2D;
class UTexture2D;
class UTextureRenderTarget2D;
struct FMap3DMeshData;

/**
 * A perspective camera over the map's flat ground: where it looks, which way, how steeply, and how far away. Positions are
 * metres in the map frame (x east, y south, z up). Used both to place the scene capture and to turn picture positions back
 * into ground positions for clicks and drags.
 */
struct FMap3DCamera
{
	FVector2d TargetMeters = FVector2d::ZeroVector;
	/** The direction the camera looks along on the ground, radians from +x (east) towards +y (south). */
	float YawRadians = 0.f;
	/** 0 looks straight down, larger angles tilt towards the horizon. */
	float TiltRadians = 0.f;
	float DistanceMeters = 500.f;
	float HorizontalFovDegrees = 40.f;
	FIntPoint ImageSize = FIntPoint(1, 1);

	/** Half the tangent of the horizontal field of view: the picture's half width in units of the depth. */
	double HalfWidthSlope() const;

	FVector GetLocationMeters() const;
	FVector GetForward() const;
	FVector GetRight() const;
	FVector GetUp() const;

	/** The ground position (z = 0) under a picture position (-1..1 on both axes, y up); false at or above the horizon. */
	bool GroundFromNdc(const FVector2f& Ndc, FVector2d& OutMeters) const;

	/** Where a world point lands in the picture, in the same coordinates; false behind the camera. */
	bool NdcFromWorld(const FVector& PointMeters, FVector2f& OutNdc) const;
};

/**
 * The 3D view of the map, in the style of a navigation app: flat roads, green and water, extruded buildings with roofs, trees,
 * traffic lights and the route as a ribbon, seen at a tilt over the car or the full map's centre.
 *
 * The geometry lives in a private preview world with its own renderer scene, so the game's view, Lumen and the streamed
 * world never see it. A scene capture renders it into a render target that the map widgets draw like any image. The tiles
 * come from the .tgtile files (their own reader, off the game thread), one dynamic mesh per 250 m tile with colours taken
 * from a palette texture, so night and day are a palette swap. Only tiles in the camera's view are kept.
 *
 * T toggles the minimap between 2D and 3D, and in the full map between flat and tilted. In the full map, right-drag or
 * Ctrl+left-drag rotates and tilts. Test switches: -Map3D (minimap starts in 3D), -MapTilt=<degrees> and
 * -MapRotation=<degrees clockwise from north> for the full map, -MapMetersPerPixel=<m> for its zoom, -MapSpin (turns it continuously), -Map3DStats and -Map3DDump.
 */
UCLASS()
class DRIVINGGAME_API UMap3DSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableWhenPaused() const override { return true; }

	/** Whether the minimap currently shows the 3D picture. */
	bool IsMinimapActive() const { return bMinimapActive; }

	/** Whether the full map currently shows the 3D picture. */
	bool IsFullMapActive() const { return bFullMapActive; }

	/** Whether the 2D maps' palette is the dark night set, which this view follows. */
	bool IsDarkPalette() const { return bDarkPalette; }

	/** T: flips the minimap between 2D and 3D, or the full map between flat and tilted. */
	void ToggleMode(bool bFullMap);

	/** Right-drag in the full map: horizontal motion rotates, vertical motion tilts. Pixels moved. */
	void OrbitFullMap(const FVector2f& CursorDeltaPixels);

	/** The ground position under a cursor in the full map (pixels from the widget's centre), world metres. */
	FVector2f FullMapScreenToWorld(const FVector2f& OffsetFromCentrePixels) const;

	/** How far the full map's centre has to move for the ground under the cursor to follow a drag of this many pixels. */
	FVector2f FullMapDragToCentreShift(const FVector2f& CursorDeltaPixels) const;

	/** The clockwise angle from up on the screen to north in the full map's picture, radians. */
	float GetFullMapNorthAngleRadians() const;

	/**
	 * Draws the 3D picture over a widget's whole area; false when it is not showing (then the caller paints the 2D map).
	 * The full map's widget size is remembered for clicks and drags.
	 */
	bool PaintMinimap(FSlateWindowElementList& Elements, const FGeometry& Geometry, int32& LayerId);
	bool PaintFullMap(FSlateWindowElementList& Elements, const FGeometry& Geometry, int32& LayerId);

private:
	/** One 250 m tile of the scene. The preview scene keeps its component alive. */
	struct FTile
	{
		TObjectPtr<UMap3DMeshComponent> Component;
		EMap3DDetail Detail = EMap3DDetail::Far;
		double LastUsedSeconds = 0.0;
		double UploadedSeconds = 0.0;
		bool bMeshDiscarded = false;
	};

	/** A tile's mesh built on a worker; Mesh is empty for a tile without shapes or without a file. */
	struct FBuiltTile
	{
		TSharedPtr<FMap3DMeshData> Mesh;
		EMap3DDetail Detail = EMap3DDetail::Far;
		int32 TriangleCount = 0;
		/** Time the worker took to read and mesh the tile, milliseconds. */
		float BuildMilliseconds = 0.f;
	};

	struct FPendingTile
	{
		TFuture<TSharedPtr<FBuiltTile>> Result;
		EMap3DDetail Detail = EMap3DDetail::Far;
	};

	/** Works out which of the two pictures is showing from the minimap's and the full map's state and the palette. */
	void UpdateModes();

	/** Applies the one-time and continuous test switches (-MapMetersPerPixel, -MapSpin). */
	void ApplyTestSwitches();

	/** The camera behind and above the car, looking ahead along its heading, sized by the minimap's zoom. */
	FMap3DCamera MakeMinimapCamera() const;

	/** The camera over the full map's centre, with the user's rotation and tilt and the full map's zoom. */
	FMap3DCamera MakeFullMapCamera() const;

	// Scene

	/** Creates the preview world, material, palette, capture component and the permanent meshes; false when the material is missing. */
	bool EnsureScene();

	/**
	 * A mesh component in the preview world with the map material, or the overlay material that is never hidden by buildings.
	 * 
	 */
	UMap3DMeshComponent* AddMeshComponent(const TSharedPtr<FMap3DMeshData>& Mesh, const FTransform& Transform, bool bOverlay);

	/** The land coloured plane under everything. */
	void BuildGround();

	/** The small white car that stands for the player. */
	void BuildCarMarker();

	/** The red pin of the waypoint. */
	void BuildPinMarker();

	/** Rewrites the palette texture when the 2D maps' palette has changed between day and night. */
	void UpdatePalette();
	void WritePalette(const FMapPalette& Source);

	/** The render target of a picture, created or resized to Size. */
	UTextureRenderTarget2D* EnsureRenderTarget(TObjectPtr<UTextureRenderTarget2D>& Target, const FIntPoint& Size);

	/** Draws a finished picture over a widget, with the distance fade on top. */
	void PaintDistanceFade(FSlateWindowElementList& Elements, const FGeometry& Geometry, int32& LayerId) const;
	bool PaintPicture(UTextureRenderTarget2D* Image, TSharedPtr<FSlateBrush>& Brush, FSlateWindowElementList& Elements, const FGeometry& Geometry, int32& LayerId);

	// Tiles

	/** Starts reading the region's tile index (world.json) on a worker, once the streamer knows the folder. */
	void StartIndexLoad();
	void PollIndexLoad();

	/** Loads the tiles the camera sees (nearest first), upgrades the near ones to full detail and drops old ones. */
	void UpdateTiles(const FMap3DCamera& Camera);

	/** The tiles whose centres are within the picture's ground footprint, with their distance from the target. */
	void CollectWantedTiles(const FMap3DCamera& Camera, TArray<TPair<float, FIntPoint>>& OutWanted) const;

	/** Reads and meshes one tile on a worker thread. */
	void StartTileLoad(const FIntPoint& Key, EMap3DDetail Detail);

	/** Turns finished tile meshes into components, within a small time budget per frame. */
	void CollectFinishedTiles();

	/** Frees the CPU copy of tile meshes whose render buffers have been made. */
	void DiscardUploadedMeshes();

	/** Removes the tiles unused for the longest time once there are too many. */
	void EvictTiles();

	// Markers and route

	/** Places the car and the waypoint pin, scaled to the view's distance. */
	void UpdateMarkers(const FMap3DCamera& Camera);

	/** Rebuilds the route ribbon when the route has changed. */
	void UpdateRoute();

	/** Renders the scene from a camera into the render target of the active picture. */
	void CaptureView(const FMap3DCamera& Camera);

	/** Adds the time since PhaseStart to the worst time of a tick phase (0 setup, 1 tiles, 2 route and markers, 3 capture) and restarts the clock. */
	void RecordPhase(int32 Phase, double& PhaseStart);

	/** -Map3DStats: logs the cost of the last few seconds. */
	void LogStats(float DeltaTime);

	/** -Map3DDump: writes the picture as PNG to Saved/Screenshots after a while, for checking without the widgets. */
	void DumpPicture(UTextureRenderTarget2D* Image);

	TWeakObjectPtr<UMinimapSubsystem> Minimap;
	TUniquePtr<FPreviewScene> Scene;
	bool bDisabled = false;
	bool bSceneFailed = false;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Material;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> OverlayMaterial;
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> PaletteTexture;
	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> Capture;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> MinimapTarget;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> FullMapTarget;
	UPROPERTY(Transient)
	TObjectPtr<UMap3DMeshComponent> GroundComponent;
	UPROPERTY(Transient)
	TObjectPtr<UMap3DMeshComponent> CarComponent;
	UPROPERTY(Transient)
	TObjectPtr<UMap3DMeshComponent> PinComponent;
	UPROPERTY(Transient)
	TObjectPtr<UMap3DMeshComponent> RouteComponent;
	TSharedPtr<FSlateBrush> MinimapBrush;
	TSharedPtr<FSlateBrush> FullMapBrush;

	// Modes
	bool bMinimapPreferred3D = false;
	bool bMinimapActive = false;
	bool bFullMapActive = false;
	bool bDarkPalette = false;
	bool bPaletteWritten = false;
	bool bPaletteIsDark = false;
	bool bHasPicture = false;
	float RouteWidthMeters = 7.f;
	float FullMapTiltRadians = 0.f;
	float FullMapYawRadians = 0.f;
	FVector2f FullMapWidgetSize = FVector2f(1920.f, 1080.f);

	// Tiles
	TFuture<TMap<FIntPoint, FString>> IndexLoad;
	bool bIndexLoadStarted = false;
	TMap<FIntPoint, FString> TilePaths;
	TMap<FIntPoint, FTile> Tiles;
	TMap<FIntPoint, FPendingTile> Pending;
	int32 LoadedTriangleCount = 0;

	// Capture bookkeeping
	float SecondsSinceCapture = 1000.f;
	uint32 LastCameraHash = 0;
	bool bCaptureDirty = true;
	int32 RouteVersionDrawn = -1;
	bool bRouteDrawn = false;

	// Statistics for -Map3DStats
	bool bStats = false;
	int32 CapturesSinceDump = 0;
	double StatsTickSeconds = 0.0;
	double StatsCaptureSeconds = 0.0;
	int32 StatsFrames = 0;
	int32 StatsCaptures = 0;
	int32 StatsTilesBuilt = 0;
	float StatsWindowSeconds = 0.f;
	double StatsWorstTickSeconds = 0.0;
	double StatsPhaseWorstSeconds[4] = {};

	// One-time test switches
	bool bTestMetersDone = false;
};
