#include "SRadioOverlay.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
constexpr float PanelFadeSeconds = 0.35f;
constexpr float TextFadeOutSeconds = 0.15f;
constexpr float TextFadeInSeconds = 0.3f;
constexpr float PanelMaxOpacity = 1.f;
constexpr float ScreenMargin = 28.f;
constexpr float PanelWidth = 340.f;

/** The panel background: dark, mostly transparent, rounded. */
const FSlateRoundedBoxBrush& PanelBrush()
{
	static const FSlateRoundedBoxBrush Brush(FLinearColor(0.02f, 0.02f, 0.03f, 0.55f), 10.f);
	return Brush;
}
}

void SRadioOverlay::Construct(const FArguments& InArgs)
{
	const FSlateFontInfo StationFont = FCoreStyle::GetDefaultFontStyle("Bold", 15);
	const FSlateFontInfo SongFont = FCoreStyle::GetDefaultFontStyle("Regular", 13);
	ChildSlot
	[
		SNew(SBox)
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.f, ScreenMargin, ScreenMargin, 0.f))
		[
			SAssignNew(PanelBox, SBox)
			.WidthOverride(PanelWidth)
			[
				SNew(SBorder)
				.BorderImage(&PanelBrush())
				.Padding(FMargin(16.f, 10.f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SAssignNew(StationText, STextBlock)
						.Font(StationFont)
						.ColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, 1.f))
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
					[
						SAssignNew(SongText, STextBlock)
						.Font(SongFont)
						.ColorAndOpacity(FLinearColor(0.85f, 0.87f, 0.9f, 1.f))
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
				]
			]
		]
	];
	SetVisibility(EVisibility::HitTestInvisible);
}

void SRadioOverlay::SetContent(bool bVisible, const FString& StationName, const FString& SongTitle)
{
	bWantVisible = bVisible;
	WantedStation = StationName;
	WantedSong = SongTitle;
}

void SRadioOverlay::ShowWantedText()
{
	ShownStation = WantedStation;
	ShownSong = WantedSong;
	StationText->SetText(FText::FromString(ShownStation));
	SongText->SetText(FText::FromString(ShownSong));
	SongText->SetVisibility(ShownSong.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible);
}

void SRadioOverlay::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	PanelOpacity = FMath::FInterpConstantTo(PanelOpacity, bWantVisible ? PanelMaxOpacity : 0.f, InDeltaTime, PanelMaxOpacity / PanelFadeSeconds);

	PanelBox->SetRenderOpacity(PanelOpacity);

	const bool bTextDiffers = WantedStation != ShownStation || WantedSong != ShownSong;
	if (bTextDiffers && PanelOpacity <= 0.f)
	{
		ShowWantedText(); // nobody sees it: change without a fade
		return;
	}
	if (bTextDiffers)
	{
		TextOpacity = FMath::FInterpConstantTo(TextOpacity, 0.f, InDeltaTime, 1.f / TextFadeOutSeconds);
		if (TextOpacity <= 0.f)
		{
			ShowWantedText();
		}
	}
	else
	{
		TextOpacity = FMath::FInterpConstantTo(TextOpacity, 1.f, InDeltaTime, 1.f / TextFadeInSeconds);
	}
	StationText->SetRenderOpacity(TextOpacity);
	SongText->SetRenderOpacity(TextOpacity);
}
