#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "MapTileCache.h"
#include "MinimapIndex.h"
#include "MinimapRoute.h"
#include "Subsystems/WorldSubsystem.h"
#include "MinimapSubsystem.generated.h"

class ACarPawn;
class FMinimapInputProcessor;
class FWidgetRenderer;
class SBox;
class SFullMapWidget;
class SMinimapContent;
class SMinimapWidget;
class UTextureRenderTarget2D;

/**
 * The map of the surrounding roads: a small heading-up minimap in the bottom right corner of the screen, and a full-screen
 * north-up map that opens with M, where a click sets a waypoint. A waypoint is snapped to the nearest road lane and the
 * subsystem plans a route to it along the AI traffic lane graph, which both maps draw in blue and which is planned again
 * when the car leaves it. Plus and minus (and two assignable wheel buttons) zoom the minimap in steps.
 *
 * Needs lanes.json (UAITrafficSubsystem); without it, as with -NoTraffic, no map is shown. -NoMinimap turns it off.
 * Test switches: -MinimapRadius=<m>, -MapWaypoint=<x>,<y> (world metres), -OpenMap (opens the full map without pausing),
 * -MapView=<x>,<y>,<metres per pixel> (where the full map opens).
 */
UCLASS()
class DRIVINGGAME_API UMinimapSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableWhenPaused() const override { return true; }

	/** Handles M and the zoom keys before the game sees them; true if the key was used. */
	bool HandleKeyDown(const FKey& Key, uint32 Character);

	// For the 3D map view (UMap3DSubsystem)
	bool IsFullMapOpen() const { return bFullMapOpen; }
	bool IsMinimapShown() const { return bCarVisible && !bFullMapOpen; }
	const FMinimapRoute& GetRoute() const { return Route; }
	int32 GetRouteVersion() const { return RouteVersion; }

	/** The waypoint's position in world metres; false without one. */
	bool GetWaypointMeters(FVector2f& OutMeters) const;

	// Minimap
	const FMinimapFrame& GetMinimapFrame() const { return MinimapFrame; }
	const TArray<const FMapTile*>& GetMinimapTiles() const { return MinimapTiles; }
	const FVector2f& GetCarPositionMeters() const { return CarPositionM; }

	/** The colours both maps and the 3D view use: the dark set at night (sun below the horizon) or with -MapDark. */
	const FMapPalette& GetPalette() const { return bDarkMap ? FMapPalette::Dark() : FMapPalette::Light(); }

	/** The tile data shared by all map views; the 3D view requests building records from it through SetBuildingRecordsWanted. */
	FMapTileCache& GetMapTileCache() { return MapTiles; }

	/** Adds the time one redraw of a map took; with -MinimapStats the averages are logged every few seconds. */
	void RecordDrawCost(bool bFullMap, double Seconds);

	/** Asks that tiles in view are loaded with their full building records (BuildingRecords of FMapTile) as well. */
	void SetBuildingRecordsWanted(bool bWanted) { bBuildingRecordsWanted = bWanted; }
	float GetMinimapRadiusMeters() const { return DisplayRadiusMeters; }
	float GetMinimapHeadingRadians() const { return CarHeadingRadians; }

	/** The waypoint in the minimap's frame (metres, +X right, +Y ahead); false without one. */
	bool GetMinimapWaypoint(FVector2f& OutView) const;

	/** Route length left, metres; false without a route. */
	bool GetRemainingRouteMeters(float& OutMeters) const;

	// Full map
	/** Brings the full map's road frame up to date for a widget of this size (pixels) and returns it. */
	const FMinimapFrame& PrepareFullMapFrame(const FVector2f& SizePixels);
	const FMinimapFrame& GetFullMapFrame() const { return FullMapFrame; }
	const TArray<const FMapTile*>& GetFullMapTiles() const { return FullMapTiles; }
	const FVector2f& GetFullMapCentreMeters() const { return FullMapCentreM; }
	float GetFullMapMetersPerPixel() const { return FullMapMetersPerPixel; }

	/** Whether the full map is zoomed in far enough to draw buildings. */
	bool AreFullMapBuildingsShown() const;
	float GetFullMapScaleBarMeters() const;
	bool GetFullMapWaypoint(FVector2f& OutView) const;

	/** The car in the full map's frame and the clockwise angle from up it points at. */
	bool GetFullMapCar(FVector2f& OutView, float& OutAngleRadians) const;

	void PanFullMap(const FVector2f& CursorDeltaPixels);

	/** Zooms by mouse wheel notches around the cursor (pixels from the widget's centre). */
	void ZoomFullMap(float WheelDelta, const FVector2f& CursorOffsetPixels);

	/** Sets the waypoint at the road nearest to a cursor position (pixels from the widget's centre). */
	void SetWaypointFromFullMap(const FVector2f& CursorOffsetPixels);

	void ClearWaypoint();

private:
	/** Adds the minimap to the viewport once there is one. */
	void EnsureMinimapWidget();

	/** Starts the map tile cache once the streamer knows where the region's tiles are. */
	void EnsureTileCache();

	/** Paints the minimap into its render target. */
	void RedrawMinimapImage();

	/** -MinimapStats: paints the open full map into an off-screen image and records how long that took, batching included. */
	void MeasureFullMapPaint();

	/** Starts the worker that simplifies the lane graph, once the AI traffic has loaded it. */
	void StartIndexBuild();
	void PollIndexBuild();

	/** The player's car, or null (free camera, start menu). */
	ACarPawn* FindCar() const;

	void ReadCarPose(const ACarPawn& Car);
	void StepZoom(int32 Direction);

	/** Picks the dark palette when the sun is well below the horizon (with a margin so it does not flicker). */
	void UpdatePalette();

	/** The request that keeps the tiles around a view loaded. */
	FMapTileRequest MakeTileRequest(const FVector2f& CentreM, float RadiusM, bool bBuildings) const;
	void AnimateZoom(float DeltaSeconds);
	void PollWheelButtons();
	void UpdateMinimapFrame();
	void ApplyTestSwitches();

	/** -MapView=<x>,<y>,<metres per pixel> puts the newly opened full map there, for screenshots. */
	void ApplyFullMapTestView();

	void OpenFullMap(bool bPauseGame);
	void CloseFullMap(bool bRestoreInput);

	/** Puts the waypoint on the lane nearest to a world position; false if no road is within MaxSnapMeters. */
	bool SetWaypointNear(const FVector2f& WorldM, float MaxSnapMeters);

	void UpdateRoute(float DeltaSeconds);
	void StartRouteSearch();
	void PollRouteSearch();
	void CheckProgressOnRoute();

	TWeakObjectPtr<ACarPawn> Car;
	TSharedPtr<FMinimapInputProcessor> InputProcessor;
	TSharedPtr<SMinimapWidget> MinimapWidget;
	TSharedPtr<SMinimapContent> MinimapContent;
	TSharedPtr<FWidgetRenderer> MinimapRenderer;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> StatsImage;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> MinimapImage;
	FMapTileCache MapTiles;
	bool bTileCacheStarted = false;
	bool bBuildingRecordsWanted = false;
	bool bDarkMap = false;
	TArray<const FMapTile*> MinimapTiles;
	TArray<const FMapTile*> FullMapTiles;
	TSharedPtr<SBox> MinimapContainer;
	TSharedPtr<SFullMapWidget> FullMapWidget;
	bool bDisabled = false;
	bool bCarVisible = false;

	// Road data
	TSharedPtr<FMinimapIndex> Index;
	TFuture<TSharedPtr<FMinimapIndex>> IndexBuild;
	bool bIndexBuildStarted = false;
	double IndexBuildStartSeconds = 0.0;

	// Car pose, world metres (x east, y south)
	FVector2f CarPositionM = FVector2f::ZeroVector;
	FVector2f CarDirection = FVector2f(1.f, 0.f);
	float CarHeadingRadians = 0.f;

	// Minimap
	FMinimapFrame MinimapFrame;
	int32 ZoomStep = 1;
	float TargetRadiusMeters = 200.f;
	float DisplayRadiusMeters = 200.f;
	float SecondsSinceFrame = 1000.f;
	int64 PreviousWheelButtons = 0;

	// Full map
	bool bFullMapOpen = false;
	bool bPausedByMap = false;
	FMinimapFrame FullMapFrame;
	FVector2f FullMapCentreM = FVector2f::ZeroVector;
	float FullMapMetersPerPixel = 1.5f;
	FVector2f FullMapFrameSize = FVector2f::ZeroVector;
	FVector2f FullMapFrameCentre = FVector2f(MAX_flt, MAX_flt);
	float FullMapFrameScale = 0.f;
	int32 FullMapFrameRouteVersion = -1;

	// Waypoint and route
	bool bHasWaypoint = false;
	FVector2f WaypointM = FVector2f::ZeroVector;
	int32 WaypointLane = INDEX_NONE;
	FMinimapRoute Route;
	int32 RouteVersion = 0;
	int32 WaypointGeneration = 0;
	TFuture<TSharedPtr<FMinimapRoute>> RouteSearch;
	int32 RouteSearchGeneration = 0;
	float SecondsSinceProgressCheck = 0.f;
	float OffRouteSeconds = 0.f;
	float RemainingMeters = 0.f;
	float SecondsUntilRetry = 0.f;

	// Draw cost statistics (-MinimapStats)
	double MinimapCostSeconds = 0.0;
	int32 MinimapCostSamples = 0;
	double FullMapCostSeconds = 0.0;
	int32 FullMapCostSamples = 0;
	double FrameSecondsSum = 0.0;
	int32 FrameCount = 0;

	// One-time test switches
	bool bTestWaypointDone = false;
	bool bTestMapDone = false;
};
