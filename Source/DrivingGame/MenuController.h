#pragma once

#include "CoreMinimal.h"
#include "MenuTypes.h"
#include "WheelInputTypes.h"

/**
 * State and logic of the menus, independent of how they are drawn (see SGameMenu): a stack of pages, the selected
 * row, repeat handling for held keys and D-pad, and the "press the button on the wheel" assignments.
 * The wheel navigates it too: the D-pad moves and changes values, the configured confirm and back buttons act.
 */
class FMenuController
{
public:
	/** Rows the panel shows at once; longer pages scroll. */
	static constexpr int32 VisibleRowCount = 12;

	/** Fires when the player backs out of the first page. */
	FSimpleDelegate OnCloseRequested;

	/** Replaces the whole stack by one page. */
	void SetRootPage(FMenuPage Page);

	/** Opens a page on top of the current one. */
	void PushPage(FMenuPage Page);

	/** Goes back one page; at the first page this asks for the menu to close. */
	void Back();

	/** Per-frame update while a menu is open: remembers the live wheel state, handles wheel navigation and assignments. */
	void Update(float DeltaSeconds, const FWheelInputState& WheelState);

	/** Applies one navigation input from any source. */
	void HandleInput(EMenuInput Input);

	/** Selects a row (mouse hover). */
	void SelectRow(int32 RowIndex);

	/** Selects and activates a row (mouse click). */
	void ActivateRow(int32 RowIndex);

	/** Steps a row's value (mouse click on the value). */
	void AdjustRow(int32 RowIndex, int32 Direction);

	/** Moves the selection by the given number of rows (mouse wheel). */
	void ScrollSelection(int32 RowDelta);

	/** Starts waiting for the next wheel button press; the callback receives its index. */
	void BeginButtonCapture(const FString& Prompt, TFunction<void(int32 ButtonIndex)> OnButton);

	/** Starts waiting for the next pedal or wheel movement; the callback receives the axis code and whether it rested near its maximum. */
	void BeginAxisCapture(const FString& Prompt, TFunction<void(int32 AxisCode, bool bRestedHigh)> OnAxis);

	/** Shows a short message in the status line. */
	void ShowMessage(const FString& Message);

	/** Cancels a running assignment and clears the message. */
	void ResetTransientState();

	/** Changes whenever a different page is shown, so the panel knows when to rebuild its list. */
	int32 GetPageSerial() const { return PageSerial; }

	bool HasPage() const { return !Stack.IsEmpty(); }
	const FMenuPage& GetPage() const { return Stack.Last().Page; }
	int32 GetSelectedRow() const { return Stack.Last().SelectedRow; }
	int32 GetFirstVisibleRow() const { return Stack.Last().FirstVisibleRow; }
	bool IsCapturing() const { return Capture.Kind != ECaptureKind::None; }

	/** The line below the list: the running assignment prompt, else the latest message, else the selected row's hint. */
	FString GetStatusLine() const;

	const FWheelInputState& GetLiveState() const { return LiveState; }

	/** Name of an evdev axis code for calibration, for example "Z (0x02)". */
	static FString DescribeAxis(int32 AxisCode);

	/** "Button 12", or "not assigned". */
	static FString DescribeButton(int32 ButtonIndex);

	/** Indices of the buttons currently held, for example "12, 17", or "none". */
	FString DescribeButtonsDown() const;

	/** "N", "R" or the gear number the H-shifter currently selects. */
	FString DescribeShifterGear() const;

private:
	enum class ECaptureKind : uint8
	{
		None,
		Button,
		Axis,
	};

	/** A running "press the button / move the pedal" assignment. */
	struct FCapture
	{
		ECaptureKind Kind = ECaptureKind::None;
		FString Prompt;
		float SecondsLeft = 0.f;
		/** Buttons that were already held when the assignment started; they don't count until released. */
		int64 IgnoredButtons = 0;
		TArray<float> BaselineAxes;
		TFunction<void(int32 ButtonIndex)> OnButton;
		TFunction<void(int32 AxisCode, bool bRestedHigh)> OnAxis;
	};

	struct FStackEntry
	{
		FMenuPage Page;
		int32 SelectedRow = 0;
		int32 FirstVisibleRow = 0;
	};

	void UpdateCapture(float DeltaSeconds);
	void UpdateButtonCapture();
	void UpdateAxisCapture();
	void CancelCapture(const FString& Message);
	void UpdateWheelNavigation(float DeltaSeconds);
	bool WasButtonPressed(int32 ButtonIndex) const;
	void MoveSelection(int32 Direction);
	void EnsureSelectionVisible();
	void SelectFirstSelectableRow();
	void ActivateSelected();
	void AdjustSelected(int32 Direction);

	TArray<FStackEntry> Stack;
	int32 PageSerial = 0;
	FCapture Capture;
	FString Message;
	float MessageSecondsLeft = 0.f;

	FWheelInputState LiveState;
	int64 PreviousButtons = 0;
	int32 HeldDirection = -1; // EMenuInput of the D-pad direction held, -1 = none
	float HeldSeconds = 0.f;
};
