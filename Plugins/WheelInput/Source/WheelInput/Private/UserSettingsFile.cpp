#include "UserSettingsFile.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogUserSettings, Log, All);

namespace
{
/** Default values as text, keyed by "<class path>.<property>". Filled by the first LoadOverrides of each class. */
TMap<FString, FString>& GetDefaultValues()
{
	static TMap<FString, FString> DefaultValues;
	return DefaultValues;
}

/** The properties to handle: the listed names, or every config property when the list is empty. */
TArray<FProperty*> ResolveProperties(const UObject* Object, const TArray<FName>& PropertyNames)
{
	TArray<FProperty*> Result;
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		const bool bListed = PropertyNames.IsEmpty() ? It->HasAnyPropertyFlags(CPF_Config) : PropertyNames.Contains(It->GetFName());
		if (bListed)
		{
			Result.Add(*It);
		}
	}
	return Result;
}

FString MakeDefaultKey(const UObject* Object, const FProperty* Property)
{
	return Object->GetClass()->GetPathName() + TEXT(".") + Property->GetName();
}

FString ExportValue(const UObject* Object, const FProperty* Property)
{
	FString Text;
	Property->ExportText_InContainer(0, Text, Object, Object, nullptr, PPF_None);
	return Text;
}

void ImportValue(UObject* Object, const FProperty* Property, const FString& Text)
{
	Property->ImportText_InContainer(*Text, Object, Object, PPF_None);
}
}

FString FUserSettingsFile::GetPath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::GeneratedConfigDir() / TEXT("UserSettings.ini"));
}

void FUserSettingsFile::LoadOverrides(UObject* Target, const TArray<FName>& PropertyNames)
{
	FConfigFile File;
	File.Read(GetPath());
	const FString Section = Target->GetClass()->GetPathName();

	for (const FProperty* Property : ResolveProperties(Target, PropertyNames))
	{
		const FString DefaultKey = MakeDefaultKey(Target, Property);
		if (!GetDefaultValues().Contains(DefaultKey))
		{
			GetDefaultValues().Add(DefaultKey, ExportValue(Target, Property));
		}
		FString SavedText;
		if (File.GetString(*Section, *Property->GetName(), SavedText))
		{
			ImportValue(Target, Property, SavedText);
		}
	}
}

void FUserSettingsFile::SaveOverrides(const UObject* Source, const TArray<FName>& PropertyNames)
{
	FConfigFile File;
	File.Read(GetPath());
	const FString Section = Source->GetClass()->GetPathName();

	for (const FProperty* Property : ResolveProperties(Source, PropertyNames))
	{
		const FString* DefaultText = GetDefaultValues().Find(MakeDefaultKey(Source, Property));
		const FString CurrentText = ExportValue(Source, Property);
		if (DefaultText && *DefaultText == CurrentText)
		{
			File.RemoveKeyFromSection(*Section, *Property->GetName());
			continue;
		}
		File.SetString(*Section, *Property->GetName(), *CurrentText);
	}
	File.Dirty = true;
	if (!File.Write(GetPath()))
	{
		UE_LOG(LogUserSettings, Warning, TEXT("Could not write %s"), *GetPath());
	}
}

void FUserSettingsFile::ResetToDefaults(UObject* Target, const TArray<FName>& PropertyNames)
{
	for (const FProperty* Property : ResolveProperties(Target, PropertyNames))
	{
		if (const FString* DefaultText = GetDefaultValues().Find(MakeDefaultKey(Target, Property)))
		{
			ImportValue(Target, Property, *DefaultText);
		}
	}
	SaveOverrides(Target, PropertyNames); // everything equals its default now, so this removes the keys
}
