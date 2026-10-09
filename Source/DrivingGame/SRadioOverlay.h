#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class STextBlock;

/**
 * The radio's readout in the top right corner of the screen: a small translucent rounded panel with the station name
 * and, below it, the current song. Text changes fade out and in. Hidden while the radio is off.
 */
class SRadioOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRadioOverlay) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Shows the panel (bVisible) with the given texts; a different text fades in. An empty song hides the second line. */
	void SetContent(bool bVisible, const FString& StationName, const FString& SongTitle);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
	/** Puts the wanted station and song into the text blocks (the second line collapses when there is no song). */
	void ShowWantedText();

	TSharedPtr<SBox> PanelBox;
	TSharedPtr<STextBlock> StationText;
	TSharedPtr<STextBlock> SongText;

	bool bWantVisible = false;
	FString ShownStation;
	FString ShownSong;
	FString WantedStation;
	FString WantedSong;
	float PanelOpacity = 0.f;
	float TextOpacity = 1.f;
};
