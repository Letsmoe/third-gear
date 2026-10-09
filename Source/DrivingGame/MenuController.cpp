#include "MenuController.h"

#include "WheelInputSettings.h"

namespace
{
/** A held D-pad direction or key repeats after this long, then at the intervals below. */
constexpr float RepeatInitialDelaySeconds = 0.45f;
constexpr float ValueRepeatIntervalSeconds = 0.06f;
constexpr float ListRepeatIntervalSeconds = 0.14f;
/** An assignment gives up after this long. */
constexpr float CaptureTimeoutSeconds = 10.f;
constexpr float MessageSeconds = 4.f;
/** An axis must travel this far (0..1) from where it rested to count as "the one the player moved". */
constexpr float AxisCaptureThreshold = 0.3f;
constexpr int32 NoDirection = -1;
}

void FMenuController::SetRootPage(FMenuPage Page)
{
	Stack.Reset();
	PushPage(MoveTemp(Page));
}

void FMenuController::PushPage(FMenuPage Page)
{
	FStackEntry& Entry = Stack.AddDefaulted_GetRef();
	Entry.Page = MoveTemp(Page);
	SelectFirstSelectableRow();
	CancelCapture(FString());
	++PageSerial;
}

void FMenuController::Back()
{
	if (Stack.IsEmpty())
	{
		return;
	}
	if (Stack.Last().Page.OnLeave)
	{
		Stack.Last().Page.OnLeave();
	}
	if (Stack.Num() == 1)
	{
		OnCloseRequested.ExecuteIfBound();
		return;
	}
	Stack.Pop();
	++PageSerial;
}

void FMenuController::SelectFirstSelectableRow()
{
	FStackEntry& Entry = Stack.Last();
	Entry.SelectedRow = 0;
	Entry.FirstVisibleRow = 0;
	for (int32 Index = 0; Index < Entry.Page.Rows.Num(); ++Index)
	{
		if (Entry.Page.Rows[Index].IsSelectable())
		{
			Entry.SelectedRow = Index;
			return;
		}
	}
}

void FMenuController::Update(float DeltaSeconds, const FWheelInputState& WheelState)
{
	LiveState = WheelState;
	if (MessageSecondsLeft > 0.f)
	{
		MessageSecondsLeft -= DeltaSeconds;
	}
	if (IsCapturing())
	{
		UpdateCapture(DeltaSeconds);
	}
	else if (HasPage())
	{
		UpdateWheelNavigation(DeltaSeconds);
	}
	PreviousButtons = WheelState.Buttons;
}

bool FMenuController::WasButtonPressed(int32 ButtonIndex) const
{
	if (ButtonIndex < 0 || ButtonIndex >= 64)
	{
		return false;
	}
	const bool bDownNow = (LiveState.Buttons >> ButtonIndex) & 1;
	const bool bDownBefore = (PreviousButtons >> ButtonIndex) & 1;
	return bDownNow && !bDownBefore;
}

void FMenuController::UpdateWheelNavigation(float DeltaSeconds)
{
	const UWheelInputSettings* WheelSettings = GetDefault<UWheelInputSettings>();
	if (WasButtonPressed(WheelSettings->MenuConfirmButtonIndex))
	{
		HandleInput(EMenuInput::Confirm);
	}
	if (WasButtonPressed(WheelSettings->MenuBackButtonIndex))
	{
		HandleInput(EMenuInput::Back);
	}

	int32 Direction = NoDirection;
	if (LiveState.DPadY != 0)
	{
		Direction = static_cast<int32>(LiveState.DPadY < 0 ? EMenuInput::Up : EMenuInput::Down);
	}
	else if (LiveState.DPadX != 0)
	{
		Direction = static_cast<int32>(LiveState.DPadX < 0 ? EMenuInput::Left : EMenuInput::Right);
	}

	if (Direction != HeldDirection)
	{
		HeldDirection = Direction;
		HeldSeconds = RepeatInitialDelaySeconds;
		if (Direction != NoDirection)
		{
			HandleInput(static_cast<EMenuInput>(Direction));
		}
		return;
	}
	if (Direction == NoDirection)
	{
		return;
	}

	const FMenuPage& Page = GetPage();
	const FMenuRow& Row = Page.Rows[GetSelectedRow()];
	const bool bSidewaysDirection = Direction == static_cast<int32>(EMenuInput::Left) || Direction == static_cast<int32>(EMenuInput::Right);
	if (bSidewaysDirection && !Row.bRepeatable)
	{
		return;
	}
	HeldSeconds -= DeltaSeconds;
	if (HeldSeconds <= 0.f)
	{
		HeldSeconds = bSidewaysDirection ? ValueRepeatIntervalSeconds : ListRepeatIntervalSeconds;
		HandleInput(static_cast<EMenuInput>(Direction));
	}
}

void FMenuController::HandleInput(EMenuInput Input)
{
	if (!HasPage())
	{
		return;
	}
	if (IsCapturing())
	{
		if (Input == EMenuInput::Back)
		{
			CancelCapture(TEXT("Assignment cancelled"));
		}
		return;
	}

	const FMenuRow* Row = GetPage().Rows.IsValidIndex(GetSelectedRow()) ? &GetPage().Rows[GetSelectedRow()] : nullptr;
	switch (Input)
	{
	case EMenuInput::Up:
		MoveSelection(-1);
		break;
	case EMenuInput::Down:
		MoveSelection(+1);
		break;
	case EMenuInput::Left:
		// On a plain action row, "left" means going back, like in a car's infotainment system; the first page has nowhere to go.
		if (Row && Row->Kind == FMenuRow::EKind::Action && Stack.Num() > 1)
		{
			Back();
			break;
		}
		AdjustSelected(-1);
		break;
	case EMenuInput::Right:
		if (Row && Row->Kind == FMenuRow::EKind::Value)
		{
			AdjustSelected(+1);
			break;
		}
		ActivateSelected();
		break;
	case EMenuInput::Confirm:
		if (Row && Row->Kind == FMenuRow::EKind::Value)
		{
			AdjustSelected(+1);
			break;
		}
		ActivateSelected();
		break;
	case EMenuInput::Back:
		Back();
		break;
	}
}

void FMenuController::ActivateSelected()
{
	const FMenuPage& Page = GetPage();
	if (Page.Rows.IsValidIndex(GetSelectedRow()) && Page.Rows[GetSelectedRow()].Activate)
	{
		// Copy first: activating may replace the page and with it the row.
		const TFunction<void()> Activate = Page.Rows[GetSelectedRow()].Activate;
		Activate();
	}
}

void FMenuController::AdjustSelected(int32 Direction)
{
	const FMenuPage& Page = GetPage();
	if (Page.Rows.IsValidIndex(GetSelectedRow()) && Page.Rows[GetSelectedRow()].Adjust)
	{
		const TFunction<void(int32)> Adjust = Page.Rows[GetSelectedRow()].Adjust;
		Adjust(Direction);
	}
}

void FMenuController::SelectRow(int32 RowIndex)
{
	FStackEntry& Entry = Stack.Last();
	if (IsCapturing() || !Entry.Page.Rows.IsValidIndex(RowIndex) || !Entry.Page.Rows[RowIndex].IsSelectable() || Entry.SelectedRow == RowIndex)
	{
		return;
	}
	Entry.SelectedRow = RowIndex;
}

void FMenuController::ActivateRow(int32 RowIndex)
{
	SelectRow(RowIndex);
	if (!IsCapturing() && GetPage().Rows.IsValidIndex(RowIndex) && GetPage().Rows[RowIndex].IsSelectable())
	{
		HandleInput(EMenuInput::Confirm);
	}
}

void FMenuController::AdjustRow(int32 RowIndex, int32 Direction)
{
	SelectRow(RowIndex);
	if (!IsCapturing())
	{
		AdjustSelected(Direction);
	}
}

void FMenuController::ScrollSelection(int32 RowDelta)
{
	if (IsCapturing())
	{
		return;
	}
	for (int32 Step = 0; Step < FMath::Abs(RowDelta); ++Step)
	{
		MoveSelection(RowDelta > 0 ? 1 : -1);
	}
}

void FMenuController::MoveSelection(int32 Direction)
{
	FStackEntry& Entry = Stack.Last();
	const int32 RowCount = Entry.Page.Rows.Num();
	for (int32 Attempt = 1; Attempt <= RowCount; ++Attempt)
	{
		const int32 Candidate = ((Entry.SelectedRow + Direction * Attempt) % RowCount + RowCount) % RowCount;
		if (Entry.Page.Rows[Candidate].IsSelectable())
		{
			Entry.SelectedRow = Candidate;
			break;
		}
	}
	EnsureSelectionVisible();
}

void FMenuController::EnsureSelectionVisible()
{
	FStackEntry& Entry = Stack.Last();
	if (Entry.SelectedRow < Entry.FirstVisibleRow)
	{
		Entry.FirstVisibleRow = Entry.SelectedRow;
	}
	if (Entry.SelectedRow >= Entry.FirstVisibleRow + VisibleRowCount)
	{
		Entry.FirstVisibleRow = Entry.SelectedRow - VisibleRowCount + 1;
	}
	// Show the section header above the selected row when it is the first row of the window.
	const bool bHeaderAbove = Entry.SelectedRow > 0 && Entry.Page.Rows[Entry.SelectedRow - 1].Kind == FMenuRow::EKind::Header;
	if (bHeaderAbove && Entry.FirstVisibleRow == Entry.SelectedRow)
	{
		Entry.FirstVisibleRow = Entry.SelectedRow - 1;
	}
	Entry.FirstVisibleRow = FMath::Clamp(Entry.FirstVisibleRow, 0, FMath::Max(0, Entry.Page.Rows.Num() - VisibleRowCount));
}

void FMenuController::ShowMessage(const FString& NewMessage)
{
	Message = NewMessage;
	MessageSecondsLeft = MessageSeconds;
}

void FMenuController::ResetTransientState()
{
	CancelCapture(FString());
	Message.Reset();
	MessageSecondsLeft = 0.f;
	HeldDirection = NoDirection;
}

FString FMenuController::GetStatusLine() const
{
	if (IsCapturing())
	{
		return FString::Printf(TEXT("%s (%.0f s, Esc to cancel)"), *Capture.Prompt, FMath::CeilToFloat(Capture.SecondsLeft));
	}
	if (MessageSecondsLeft > 0.f && !Message.IsEmpty())
	{
		return Message;
	}
	if (HasPage() && GetPage().Rows.IsValidIndex(GetSelectedRow()))
	{
		return GetPage().Rows[GetSelectedRow()].Hint;
	}
	return FString();
}

void FMenuController::BeginButtonCapture(const FString& Prompt, TFunction<void(int32)> OnButton)
{
	Capture = FCapture();
	Capture.Kind = ECaptureKind::Button;
	Capture.Prompt = Prompt;
	Capture.SecondsLeft = CaptureTimeoutSeconds;
	Capture.IgnoredButtons = LiveState.Buttons;
	Capture.OnButton = MoveTemp(OnButton);
}

void FMenuController::BeginAxisCapture(const FString& Prompt, TFunction<void(int32, bool)> OnAxis)
{
	Capture = FCapture();
	Capture.Kind = ECaptureKind::Axis;
	Capture.Prompt = Prompt;
	Capture.SecondsLeft = CaptureTimeoutSeconds;
	Capture.BaselineAxes = LiveState.AxisValues;
	Capture.OnAxis = MoveTemp(OnAxis);
}

void FMenuController::CancelCapture(const FString& NewMessage)
{
	const bool bWasCapturing = IsCapturing();
	Capture = FCapture();
	if (bWasCapturing && !NewMessage.IsEmpty())
	{
		ShowMessage(NewMessage);
	}
}

void FMenuController::UpdateCapture(float DeltaSeconds)
{
	Capture.SecondsLeft -= DeltaSeconds;
	if (Capture.SecondsLeft <= 0.f)
	{
		CancelCapture(TEXT("Nothing pressed, assignment cancelled"));
		return;
	}
	if (Capture.Kind == ECaptureKind::Button)
	{
		UpdateButtonCapture();
	}
	else
	{
		UpdateAxisCapture();
	}
}

void FMenuController::UpdateButtonCapture()
{
	// Buttons held when the assignment started don't count until they have been released once.
	Capture.IgnoredButtons &= LiveState.Buttons;
	const int64 Candidates = LiveState.Buttons & ~Capture.IgnoredButtons;
	for (int32 ButtonIndex = 0; ButtonIndex < 64; ++ButtonIndex)
	{
		if (!((Candidates >> ButtonIndex) & 1))
		{
			continue;
		}
		const TFunction<void(int32)> OnButton = Capture.OnButton;
		Capture = FCapture();
		OnButton(ButtonIndex);
		return;
	}
}

void FMenuController::UpdateAxisCapture()
{
	const UWheelInputSettings* WheelSettings = GetDefault<UWheelInputSettings>();
	for (int32 AxisCode = 0; AxisCode < LiveState.AxisValues.Num() && AxisCode < Capture.BaselineAxes.Num(); ++AxisCode)
	{
		const bool bIsDPad = AxisCode == WheelSettings->DPadXAxis || AxisCode == WheelSettings->DPadYAxis;
		const float Current = LiveState.AxisValues[AxisCode];
		const float Baseline = Capture.BaselineAxes[AxisCode];
		if (bIsDPad || Current < 0.f || Baseline < 0.f || FMath::Abs(Current - Baseline) < AxisCaptureThreshold)
		{
			continue;
		}
		const TFunction<void(int32, bool)> OnAxis = Capture.OnAxis;
		Capture = FCapture();
		OnAxis(AxisCode, Baseline > 0.5f);
		return;
	}
}

FString FMenuController::DescribeAxis(int32 AxisCode)
{
	static const TMap<int32, const TCHAR*> Names = {
		{0x00, TEXT("X")}, {0x01, TEXT("Y")}, {0x02, TEXT("Z")}, {0x03, TEXT("RX")}, {0x04, TEXT("RY")}, {0x05, TEXT("RZ")},
		{0x06, TEXT("Throttle")}, {0x07, TEXT("Rudder")}, {0x08, TEXT("Wheel")}, {0x09, TEXT("Gas")}, {0x0a, TEXT("Brake")},
		{0x10, TEXT("Hat X")}, {0x11, TEXT("Hat Y")},
	};
	if (const TCHAR* const* Name = Names.Find(AxisCode))
	{
		return FString::Printf(TEXT("%s (0x%02x)"), *FString(*Name), AxisCode);
	}
	return FString::Printf(TEXT("Axis 0x%02x"), AxisCode);
}

FString FMenuController::DescribeButton(int32 ButtonIndex)
{
	return ButtonIndex < 0 ? FString(TEXT("not assigned")) : FString::Printf(TEXT("Button %d"), ButtonIndex);
}

FString FMenuController::DescribeButtonsDown() const
{
	FString Result;
	for (int32 ButtonIndex = 0; ButtonIndex < 64; ++ButtonIndex)
	{
		if (LiveState.IsButtonDown(ButtonIndex))
		{
			Result += (Result.IsEmpty() ? TEXT("") : TEXT(", ")) + FString::FromInt(ButtonIndex);
		}
	}
	return Result.IsEmpty() ? FString(TEXT("none")) : Result;
}

FString FMenuController::DescribeShifterGear() const
{
	if (LiveState.ShifterGear == 0)
	{
		return TEXT("N");
	}
	return LiveState.ShifterGear < 0 ? FString(TEXT("R")) : FString::FromInt(LiveState.ShifterGear);
}
