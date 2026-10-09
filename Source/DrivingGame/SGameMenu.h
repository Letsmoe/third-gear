#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FMenuController;
class SVerticalBox;

/**
 * Draws the menus of an FMenuController: a dark translucent list on the left with the page title, the rows and a hint
 * line, and for the wheel page a card with the live wheel readout. The same widget is used full screen on the desktop and
 * on a floating panel in VR (bWorldPanel), where there is no screen to blur or dim.
 */
class SGameMenu : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGameMenu) {}
		SLATE_ARGUMENT(FMenuController*, Controller)
		/** True when drawn onto a panel in the world (VR) instead of the screen. */
		SLATE_ARGUMENT(bool, bWorldPanel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual void Tick(const FGeometry& AllottedGeometry, const double CurrentTime, const float DeltaTime) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	TSharedRef<SWidget> BuildLeftPanel();
	TSharedRef<SWidget> BuildTitleBlock();
	TSharedRef<SWidget> BuildStatusBlock();
	TSharedRef<SWidget> BuildReadoutCard();

	/** Rebuilds the rows after the controller switched to another page. */
	void RebuildRows();

	/** Rebuilds the raw-axis list of the readout when the device reports different axes. */
	void RebuildAxisList();

	FMenuController* Controller = nullptr;
	bool bWorldPanel = false;
	int32 BuiltPageSerial = -1;
	uint64 BuiltAxisMask = ~0ull;
	TSharedPtr<SVerticalBox> RowBox;
	TSharedPtr<SVerticalBox> AxisBox;
};
