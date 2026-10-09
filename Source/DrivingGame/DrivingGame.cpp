#include "CarSettings.h"
#include "DrivingPreferences.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "UserSettingsFile.h"

/** Game module: besides the default behaviour it applies the user's saved overrides (force feedback, preferences) once the engine is up. */
class FDrivingGameModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();
		FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FDrivingGameModule::LoadUserSettings);
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::GetOnPostEngineInit().RemoveAll(this);
		FDefaultGameModuleImpl::ShutdownModule();
	}

private:
	/** Applies Saved/Config/<platform>/UserSettings.ini to the car settings and preferences (the wheel plugin does the same for its own settings). */
	void LoadUserSettings()
	{
		if (IsRunningCommandlet())
		{
			return;
		}
		FUserSettingsFile::LoadOverrides(GetMutableDefault<UCarSettings>(), UCarSettings::GetUserEditableProperties());
		FUserSettingsFile::LoadOverrides(GetMutableDefault<UDrivingPreferences>(), UDrivingPreferences::GetUserEditableProperties());
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FDrivingGameModule, DrivingGame, "DrivingGame");
