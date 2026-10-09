#include "SGameMenu.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Fonts/SlateFontInfo.h"
#include "MenuController.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBackgroundBlur.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "WheelInputSettings.h"

namespace GameMenuStyle
{
/** A colour given in sRGB 0..255, as designers write it. */
FLinearColor Srgb(uint8 Red, uint8 Green, uint8 Blue, float Alpha = 1.f)
{
	FLinearColor Color = FLinearColor::FromSRGBColor(FColor(Red, Green, Blue));
	Color.A = Alpha;
	return Color;
}

const FLinearColor TextPrimary = Srgb(236, 240, 245);
const FLinearColor TextSecondary = Srgb(158, 168, 180);
const FLinearColor TextDim = Srgb(106, 116, 128);
const FLinearColor Accent = Srgb(86, 168, 255);
const FLinearColor AccentSoft = Srgb(86, 168, 255, 0.22f);
const FLinearColor PanelBackground = Srgb(8, 10, 13, 0.66f);
const FLinearColor MeterTrack = Srgb(255, 255, 255, 0.10f);

constexpr float RowHeight = 48.f;

FSlateFontInfo Font(const TCHAR* Weight, int32 Size, int32 LetterSpacing = 0)
{
	FSlateFontInfo Info = FCoreStyle::GetDefaultFontStyle(Weight, Size);
	Info.LetterSpacing = LetterSpacing;
	return Info;
}

const FSlateBrush* RoundedWhite(float Radius)
{
	static const FSlateRoundedBoxBrush Small(FLinearColor::White, 6.f);
	static const FSlateRoundedBoxBrush Large(FLinearColor::White, 18.f);
	return Radius > 10.f ? &Large : &Small;
}

const FSlateBrush* FlatWhite()
{
	static const FSlateColorBrush Brush(FLinearColor::White);
	return &Brush;
}
}

// ---------------------------------------------------------------------------------------------------------------------
// A thin horizontal bar showing a range of 0..1, used for sliders and the live pedal and axis readings.
// ---------------------------------------------------------------------------------------------------------------------

class SGameMenuMeter : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SGameMenuMeter) : _RangeStart(0.f), _RangeEnd(0.f), _Thickness(6.f) {}
		SLATE_ATTRIBUTE(float, RangeStart)
		SLATE_ATTRIBUTE(float, RangeEnd)
		SLATE_ARGUMENT(float, Thickness)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		RangeStart = InArgs._RangeStart;
		RangeEnd = InArgs._RangeEnd;
		Thickness = InArgs._Thickness;
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		const FVector2f Size = AllottedGeometry.GetLocalSize();
		const float Top = (Size.Y - Thickness) * 0.5f;
		const FSlateBrush* Brush = GameMenuStyle::RoundedWhite(0.f);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(FVector2f(Size.X, Thickness), FSlateLayoutTransform(FVector2f(0.f, Top))),
			Brush, ESlateDrawEffect::None, GameMenuStyle::MeterTrack);

		const float From = FMath::Clamp(FMath::Min(RangeStart.Get(), RangeEnd.Get()), 0.f, 1.f);
		const float To = FMath::Clamp(FMath::Max(RangeStart.Get(), RangeEnd.Get()), 0.f, 1.f);
		const float FillWidth = FMath::Max((To - From) * Size.X, 0.f);
		if (FillWidth > 0.5f)
		{
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1, AllottedGeometry.ToPaintGeometry(FVector2f(FMath::Max(FillWidth, 4.f), Thickness), FSlateLayoutTransform(FVector2f(From * Size.X, Top))),
				Brush, ESlateDrawEffect::None, GameMenuStyle::Accent);
		}
		return LayerId + 1;
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(40.f, Thickness); }

private:
	TAttribute<float> RangeStart;
	TAttribute<float> RangeEnd;
	float Thickness = 6.f;
};

// ---------------------------------------------------------------------------------------------------------------------
// One row of the list. Reads its content from the controller every frame, so values and focus stay live.
// ---------------------------------------------------------------------------------------------------------------------

class SGameMenuRow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGameMenuRow) {}
		SLATE_ARGUMENT(FMenuController*, Controller)
		SLATE_ARGUMENT(int32, RowIndex)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Controller = InArgs._Controller;
		RowIndex = InArgs._RowIndex;
		const FMenuRow* Row = GetRow();
		if (Row && Row->Kind == FMenuRow::EKind::Header)
		{
			ConstructHeader(Row->Label);
			return;
		}
		ConstructSelectableRow();
	}

	virtual void OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		SCompoundWidget::OnMouseEnter(MyGeometry, MouseEvent);
		Controller->SelectRow(RowIndex);
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		const FMenuRow* Row = GetRow();
		if (!Row || !Row->IsSelectable() || MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
		{
			return FReply::Unhandled();
		}
		const FVector2f Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
		const float Fraction = Local.X / FMath::Max(MyGeometry.GetLocalSize().X, 1.f);
		const bool bOnValue = Row->Kind == FMenuRow::EKind::Value && Fraction > 0.5f;
		if (bOnValue)
		{
			// Left of the slider's middle steps down, right of it steps up.
			Controller->AdjustRow(RowIndex, Fraction < 0.76f ? -1 : +1);
			return FReply::Handled();
		}
		Controller->ActivateRow(RowIndex);
		return FReply::Handled();
	}

private:
	const FMenuRow* GetRow() const
	{
		return Controller->HasPage() && Controller->GetPage().Rows.IsValidIndex(RowIndex) ? &Controller->GetPage().Rows[RowIndex] : nullptr;
	}

	bool IsSelected() const { return Controller->HasPage() && Controller->GetSelectedRow() == RowIndex; }

	FText GetValueText() const
	{
		const FMenuRow* Row = GetRow();
		if (!Row || !Row->GetValue)
		{
			return FText::GetEmpty();
		}
		const bool bWaiting = Controller->IsCapturing() && IsSelected();
		return FText::FromString(bWaiting ? FString(TEXT("waiting...")) : Row->GetValue());
	}

	void ConstructHeader(const FString& Label)
	{
		ChildSlot
		.Padding(FMargin(20.f, 22.f, 0.f, 6.f))
		[
			SNew(STextBlock)
			.Text(FText::FromString(Label.ToUpper()))
			.Font(GameMenuStyle::Font(TEXT("Bold"), 13, 3))
			.ColorAndOpacity(GameMenuStyle::TextDim)
		];
	}

	void ConstructSelectableRow()
	{
		using namespace GameMenuStyle;
		const FMenuRow* Row = GetRow();
		const bool bHasValue = Row && (Row->Kind == FMenuRow::EKind::Value || Row->Kind == FMenuRow::EKind::Assign);
		const bool bIsAdjustable = Row && Row->Kind == FMenuRow::EKind::Value;
		const bool bHasSlider = Row && Row->GetFraction;

		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(RoundedWhite(0.f))
			.BorderBackgroundColor_Lambda([this]() { return IsSelected() ? AccentSoft : FLinearColor::Transparent; })
			.Padding(FMargin(0.f))
			[
				SNew(SBox).HeightOverride(RowHeight)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Fill).Padding(FMargin(0.f, 8.f))
					[
						SNew(SBox).WidthOverride(4.f)
						[
							SNew(SBorder)
							.BorderImage(RoundedWhite(0.f))
							.BorderBackgroundColor_Lambda([this]() { return IsSelected() ? Accent : FLinearColor::Transparent; })
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(FMargin(16.f, 0.f))
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return FText::FromString(GetRow() ? GetRow()->Label : FString()); })
						.Font(Font(TEXT("Regular"), 23))
						.ColorAndOpacity_Lambda([this]() { return IsSelected() ? TextPrimary : TextSecondary; })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 6.f, 0.f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("‹")))
						.Font(Font(TEXT("Bold"), 26))
						.ColorAndOpacity(Accent)
						.Visibility_Lambda([this, bIsAdjustable]() { return bIsAdjustable && IsSelected() ? EVisibility::HitTestInvisible : EVisibility::Hidden; })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 12.f, 0.f))
					[
						SNew(SBox).WidthOverride(130.f).Visibility(bHasSlider ? EVisibility::HitTestInvisible : EVisibility::Collapsed)
						[
							SNew(SGameMenuMeter)
							.RangeStart(0.f)
							.RangeEnd_Lambda([this]() { return GetRow() && GetRow()->GetFraction ? GetRow()->GetFraction() : 0.f; })
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SBox).MinDesiredWidth(bHasValue ? 120.f : 0.f).HAlign(HAlign_Right)
						[
							SNew(STextBlock)
							.Text_Lambda([this]() { return GetValueText(); })
							.Font(Font(TEXT("Regular"), 21))
							.ColorAndOpacity_Lambda([this]() { return IsSelected() ? TextPrimary : TextSecondary; })
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.f, 0.f, 18.f, 0.f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("›")))
						.Font(Font(TEXT("Bold"), 26))
						.ColorAndOpacity(Accent)
						.Visibility_Lambda([this, bIsAdjustable]() { return bIsAdjustable && IsSelected() ? EVisibility::HitTestInvisible : EVisibility::Hidden; })
					]
				]
			]
		];
	}

	FMenuController* Controller = nullptr;
	int32 RowIndex = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// The menu
// ---------------------------------------------------------------------------------------------------------------------

void SGameMenu::Construct(const FArguments& InArgs)
{
	using namespace GameMenuStyle;
	Controller = InArgs._Controller;
	bWorldPanel = InArgs._bWorldPanel;

	TSharedRef<SWidget> LeftPanel = BuildLeftPanel();
	TSharedRef<SWidget> ReadoutCard = BuildReadoutCard();

	if (bWorldPanel)
	{
		// Floating panel: one rounded dark slab, the list on the left and the readout on the right.
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(RoundedWhite(18.f))
			.BorderBackgroundColor(Srgb(8, 10, 13, 0.92f))
			.Padding(FMargin(48.f, 40.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f)[LeftPanel]
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(40.f, 0.f, 0.f, 0.f))[ReadoutCard]
			]
		];
		return;
	}

	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).WidthOverride(760.f)
			[
				SNew(SBackgroundBlur)
				.BlurStrength(22.f)
				.bApplyAlphaToBlur(false)
				.Padding(FMargin(0.f))
				[
					SNew(SBorder)
					.BorderImage(FlatWhite())
					.BorderBackgroundColor(PanelBackground)
					.Padding(FMargin(84.f, 64.f, 44.f, 40.f))
					[LeftPanel]
				]
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(40.f, 0.f))
		[ReadoutCard]
	];
}

TSharedRef<SWidget> SGameMenu::BuildTitleBlock()
{
	using namespace GameMenuStyle;
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SVerticalBox)
			.Visibility_Lambda([this]() { return Controller->HasPage() && Controller->GetPage().bShowGameTitle ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("THIRD GEAR"))).Font(Font(TEXT("Bold"), 52, 10)).ColorAndOpacity(TextPrimary)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(3.f, 4.f, 0.f, 36.f))
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("Hamburg driving simulator"))).Font(Font(TEXT("Light"), 22, 2)).ColorAndOpacity(TextSecondary)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 22.f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Bottom)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(Controller->HasPage() ? Controller->GetPage().Title : FString()); })
					.Font(Font(TEXT("Light"), 40))
					.ColorAndOpacity(TextPrimary)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(FMargin(0.f, 0.f, 20.f, 8.f))
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						const bool bScrolls = Controller->HasPage() && Controller->GetPage().Rows.Num() > FMenuController::VisibleRowCount;
						return bScrolls ? FText::FromString(FString::Printf(TEXT("%d / %d"), Controller->GetSelectedRow() + 1, Controller->GetPage().Rows.Num())) : FText::GetEmpty();
					})
					.Font(Font(TEXT("Regular"), 15))
					.ColorAndOpacity(TextDim)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(2.f, 12.f, 0.f, 0.f)).HAlign(HAlign_Left)
			[
				SNew(SBox).WidthOverride(44.f).HeightOverride(3.f)
				[
					SNew(SBorder).BorderImage(FlatWhite()).BorderBackgroundColor(Accent)
				]
			]
		];
}

TSharedRef<SWidget> SGameMenu::BuildStatusBlock()
{
	using namespace GameMenuStyle;
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 18.f))
		[
			SNew(SBox).HeightOverride(64.f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return FText::FromString(Controller->GetStatusLine()); })
				.AutoWrapText(true)
				.Font(Font(TEXT("Regular"), 18))
				.ColorAndOpacity_Lambda([this]() { return Controller->IsCapturing() ? Accent : TextSecondary; })
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("↑ ↓  choose      ← →  change      Enter  select      Esc  back")))
			.Font(Font(TEXT("Regular"), 15))
			.ColorAndOpacity(TextDim)
		];
}

TSharedRef<SWidget> SGameMenu::BuildLeftPanel()
{
	RowBox = SNew(SVerticalBox);
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[BuildTitleBlock()]
		+ SVerticalBox::Slot().FillHeight(1.f)[RowBox.ToSharedRef()]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 12.f, 0.f, 0.f))[BuildStatusBlock()];
}

void SGameMenu::RebuildRows()
{
	RowBox->ClearChildren();
	BuiltPageSerial = Controller->GetPageSerial();
	if (!Controller->HasPage())
	{
		return;
	}
	const int32 RowCount = Controller->GetPage().Rows.Num();
	for (int32 RowIndex = 0; RowIndex < RowCount; ++RowIndex)
	{
		RowBox->AddSlot()
		.AutoHeight()
		[
			SNew(SGameMenuRow).Controller(Controller).RowIndex(RowIndex)
			.Visibility_Lambda([this, RowIndex]()
			{
				const int32 First = Controller->GetFirstVisibleRow();
				return RowIndex >= First && RowIndex < First + FMenuController::VisibleRowCount ? EVisibility::Visible : EVisibility::Collapsed;
			})
		];
	}
}

TSharedRef<SWidget> SGameMenu::BuildReadoutCard()
{
	using namespace GameMenuStyle;
	AxisBox = SNew(SVerticalBox);

	const auto Heading = [](const TCHAR* Text)
	{
		return SNew(STextBlock).Text(FText::FromString(Text)).Font(Font(TEXT("Bold"), 13, 4)).ColorAndOpacity(TextDim);
	};
	const auto LabelledMeter = [this](const FString& Label, TFunction<float()> GetStart, TFunction<float()> GetEnd, TFunction<FString()> GetText)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(120.f)[SNew(STextBlock).Text(FText::FromString(Label)).Font(Font(TEXT("Regular"), 18)).ColorAndOpacity(TextSecondary)]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 14.f, 0.f))
			[
				SNew(SGameMenuMeter).Thickness(8.f).RangeStart_Lambda(MoveTemp(GetStart)).RangeEnd_Lambda(MoveTemp(GetEnd))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(74.f).HAlign(HAlign_Right)
				[
					SNew(STextBlock).Text_Lambda([GetText]() { return FText::FromString(GetText()); }).Font(Font(TEXT("Regular"), 18)).ColorAndOpacity(TextPrimary)
				]
			];
	};
	const auto PedalMeter = [&](const FString& Label, float FWheelInputState::* Member)
	{
		return LabelledMeter(Label, []() { return 0.f; },
			[this, Member]() { return Controller->GetLiveState().*Member; },
			[this, Member]() { return FString::Printf(TEXT("%.0f %%"), Controller->GetLiveState().*Member * 100.f); });
	};

	return SNew(SBox).WidthOverride(560.f)
		.Visibility_Lambda([this]() { return Controller->HasPage() && Controller->GetPage().bShowWheelReadout ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
		[
			SNew(SBorder)
			.BorderImage(RoundedWhite(18.f))
			.BorderBackgroundColor(Srgb(8, 10, 13, bWorldPanel ? 0.55f : 0.78f))
			.Padding(FMargin(32.f, 28.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[Heading(TEXT("LIVE WHEEL INPUT"))]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 6.f, 0.f, 22.f))
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(Controller->GetLiveState().bConnected ? TEXT("Wheel connected") : TEXT("No wheel found")); })
					.Font(Font(TEXT("Light"), 22))
					.ColorAndOpacity_Lambda([this]() { return Controller->GetLiveState().bConnected ? TextPrimary : Srgb(255, 170, 90); })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 10.f))
				[
					LabelledMeter(TEXT("Steering"),
						[this]() { return 0.5f; },
						[this]() { return Controller->GetLiveState().Steering * 0.5f + 0.5f; },
						[this]() { return FString::Printf(TEXT("%.0f°"), Controller->GetLiveState().SteeringDegrees); })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 10.f))[PedalMeter(TEXT("Throttle"), &FWheelInputState::Throttle)]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 10.f))[PedalMeter(TEXT("Brake"), &FWheelInputState::Brake)]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 26.f))[PedalMeter(TEXT("Clutch"), &FWheelInputState::Clutch)]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 10.f))[Heading(TEXT("RAW AXES"))]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 22.f))[AxisBox.ToSharedRef()]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 8.f))[Heading(TEXT("BUTTONS"))]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						const FWheelInputState& State = Controller->GetLiveState();
						return FText::FromString(FString::Printf(TEXT("Held: %s     Gear: %s     D-pad: %d, %d"),
							*Controller->DescribeButtonsDown(), *Controller->DescribeShifterGear(), State.DPadX, State.DPadY));
					})
					.AutoWrapText(true)
					.Font(Font(TEXT("Regular"), 18))
					.ColorAndOpacity(TextPrimary)
				]
			]
		];
}

void SGameMenu::RebuildAxisList()
{
	using namespace GameMenuStyle;
	const TArray<float>& Values = Controller->GetLiveState().AxisValues;
	uint64 Mask = 0;
	for (int32 AxisCode = 0; AxisCode < Values.Num() && AxisCode < 64; ++AxisCode)
	{
		Mask |= Values[AxisCode] >= 0.f ? (1ull << AxisCode) : 0ull;
	}
	if (Mask == BuiltAxisMask)
	{
		return;
	}
	BuiltAxisMask = Mask;
	AxisBox->ClearChildren();
	if (Mask == 0)
	{
		AxisBox->AddSlot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(TEXT("No axes reported"))).Font(Font(TEXT("Regular"), 18)).ColorAndOpacity(TextDim)];
		return;
	}
	for (int32 AxisCode = 0; AxisCode < 64; ++AxisCode)
	{
		if (!((Mask >> AxisCode) & 1ull))
		{
			continue;
		}
		AxisBox->AddSlot().AutoHeight().Padding(FMargin(0.f, 3.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(150.f)
				[
					SNew(STextBlock).Text(FText::FromString(FMenuController::DescribeAxis(AxisCode))).Font(Font(TEXT("Regular"), 16)).ColorAndOpacity(TextSecondary)
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(FMargin(0.f, 0.f, 14.f, 0.f))
			[
				SNew(SGameMenuMeter).Thickness(5.f).RangeStart(0.f)
				.RangeEnd_Lambda([this, AxisCode]() { const TArray<float>& Live = Controller->GetLiveState().AxisValues; return Live.IsValidIndex(AxisCode) ? Live[AxisCode] : 0.f; })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(74.f).HAlign(HAlign_Right)
				[
					SNew(STextBlock)
					.Text_Lambda([this, AxisCode]() { const TArray<float>& Live = Controller->GetLiveState().AxisValues; return FText::FromString(FString::Printf(TEXT("%.3f"), Live.IsValidIndex(AxisCode) ? Live[AxisCode] : 0.f)); })
					.Font(Font(TEXT("Regular"), 16)).ColorAndOpacity(TextPrimary)
				]
			]
		];
	}
}

void SGameMenu::Tick(const FGeometry& AllottedGeometry, const double CurrentTime, const float DeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, CurrentTime, DeltaTime);
	if (BuiltPageSerial != Controller->GetPageSerial())
	{
		RebuildRows();
	}
	if (Controller->HasPage() && Controller->GetPage().bShowWheelReadout)
	{
		RebuildAxisList();
	}
}

FReply SGameMenu::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	Controller->ScrollSelection(MouseEvent.GetWheelDelta() > 0.f ? -1 : 1);
	return FReply::Handled();
}
