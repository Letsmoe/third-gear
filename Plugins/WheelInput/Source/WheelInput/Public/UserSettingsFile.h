#pragma once

#include "CoreMinimal.h"

class UObject;

/**
 * Per-user overrides of config properties, kept in Saved/Config/<platform>/UserSettings.ini so the checked-in
 * Config/Default*.ini files are never rewritten. A property is stored there only while it differs from the value the
 * regular config hierarchy gave it ("the default"); resetting restores that value and removes the key.
 * Sections are the class path names, e.g. [/Script/WheelInput.WheelInputSettings].
 */
struct WHEELINPUT_API FUserSettingsFile
{
	/**
	 * Remembers the current values of the properties as their defaults (first call per class only), then applies the saved
	 * overrides to the object. Call once at startup, before anything has changed the object. An empty property list means
	 * every config property of the class.
	 */
	static void LoadOverrides(UObject* Target, const TArray<FName>& PropertyNames);

	/** Writes the properties that differ from their defaults to the user file and removes the ones that match again. */
	static void SaveOverrides(const UObject* Source, const TArray<FName>& PropertyNames);

	/** Restores the defaults on the object and removes the properties from the user file. */
	static void ResetToDefaults(UObject* Target, const TArray<FName>& PropertyNames);

	/** Absolute path of the user file. */
	static FString GetPath();
};
