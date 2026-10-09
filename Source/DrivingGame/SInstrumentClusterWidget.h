#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"
#include "InstrumentCluster.h"

/**
 * Paints the instrument cluster: two analogue dials with needles, the centre display and the warning lights.
 * All sizes are fractions of the widget's height, so the picture scales to any resolution. Drawn with Slate (anti-aliased
 * lines, scalable text) into a render target by FWidgetRenderer; see UInstrumentClusterComponent.
 */
class SInstrumentClusterWidget : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SInstrumentClusterWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** What to show on the next paint. */
	void SetState(const FClusterState& NewState) { State = NewState; }

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override { return FVector2D(2048.0, 750.0); }

private:
	FClusterState State;
};
