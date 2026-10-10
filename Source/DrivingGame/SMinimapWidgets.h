#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"

struct FSlateBrush;
class UMinimapSubsystem;
class UTextureRenderTarget2D;

/** Size in pixels of the minimap as shown, and of the image it is painted at (twice as large, for smooth edges). */
constexpr float MinimapDisplayPx = 320.f;
constexpr float MinimapContentPx = 640.f;

/**
 * What the minimap shows, painted at MinimapContentPx square into a render target: land cover, water, buildings, roads,
 * the planned route and the car, heading up. SMinimapWidget shows the result with rounded corners and a shadow.
 */
class SMinimapContent : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SMinimapContent) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UMinimapSubsystem* InOwner);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override { return FVector2D(MinimapContentPx, MinimapContentPx); }

private:
	TWeakObjectPtr<UMinimapSubsystem> Owner;
};

/** The minimap in the corner of the screen: the painted image with rounded corners over a soft shadow. */
class SMinimapWidget : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SMinimapWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UTextureRenderTarget2D* InImage);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	TSharedPtr<FSlateBrush> ImageBrush;
	TSharedPtr<FSlateBrush> ShadowBrush;
};

/** The full-screen north-up map that opens with M: drag to pan, wheel to zoom at the cursor, click to set a waypoint. */
class SFullMapWidget : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SFullMapWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UMinimapSubsystem* InOwner);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override { return FVector2D(1920.0, 1080.0); }
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	/** A position in the widget (pixels) as an offset from its centre. */
	FVector2f OffsetFromCentre(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) const;

	/** Turns and tilts the 3D map by a cursor movement (pixels). */
	void OrbitMap(const FVector2f& CursorDeltaPixels);

	TWeakObjectPtr<UMinimapSubsystem> Owner;
	bool bLeftDown = false;
	bool bRightDown = false;
	/** The left button turns the 3D map instead of moving it (Ctrl held at the press). */
	bool bOrbiting = false;
	bool bDragged = false;
	FVector2f PressPosition = FVector2f::ZeroVector;
};
