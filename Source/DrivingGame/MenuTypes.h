#pragma once

#include "CoreMinimal.h"

/** What the player pressed: keyboard keys, the wheel's D-pad and its assigned buttons all map to these. */
enum class EMenuInput : uint8
{
	Up,
	Down,
	Left,
	Right,
	Confirm,
	Back,
};

/** One line of a menu page. Functions that don't apply to a kind of row stay unbound. */
struct FMenuRow
{
	enum class EKind : uint8
	{
		/** A section title; never selected. */
		Header,
		/** Does something when confirmed (and on D-pad right). */
		Action,
		/** Shows a value that left and right change. */
		Value,
		/** Shows a wheel button or axis; confirming it starts "press the button on the wheel". */
		Assign,
	};

	EKind Kind = EKind::Action;
	FString Label;
	/** Explanation shown at the bottom of the panel while the row is selected. */
	FString Hint;
	/** The value as text, shown at the right of Value and Assign rows. */
	TFunction<FString()> GetValue;
	/** Position of a Value row within its range, 0..1; draws a slider. Unbound = no slider. */
	TFunction<float()> GetFraction;
	/** Left or right (-1 or +1): changes a Value row, clears an Assign row on -1. */
	TFunction<void(int32 Direction)> Adjust;
	/** Confirm key, D-pad right or a mouse click. */
	TFunction<void()> Activate;
	/** True if holding left or right should keep stepping the value. */
	bool bRepeatable = false;

	bool IsSelectable() const { return Kind != EKind::Header; }
};

/** A list of rows with a title, and optionally a live readout of the wheel next to it. */
struct FMenuPage
{
	FString Title;
	FString Subtitle;
	TArray<FMenuRow> Rows;
	/** Shows the live wheel readout (raw axes, pedals, buttons) beside the list. */
	bool bShowWheelReadout = false;
	/** Draws the game title above the list; for the start menu. */
	bool bShowGameTitle = false;
	/** Runs when the page is left, for saving what the player changed. */
	TFunction<void()> OnLeave;
};
