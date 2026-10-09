#include "DrivingPreferences.h"

const TArray<FName>& UDrivingPreferences::GetUserEditableProperties()
{
	static const TArray<FName> Names = {
		GET_MEMBER_NAME_CHECKED(UDrivingPreferences, ScreenPercentage),
		GET_MEMBER_NAME_CHECKED(UDrivingPreferences, ViewDistanceMeters),
		GET_MEMBER_NAME_CHECKED(UDrivingPreferences, MasterVolume),
		GET_MEMBER_NAME_CHECKED(UDrivingPreferences, EngineVolume),
		GET_MEMBER_NAME_CHECKED(UDrivingPreferences, AmbienceVolume),
	};
	return Names;
}

FOnDrivingPreferencesChanged& UDrivingPreferences::OnChanged()
{
	static FOnDrivingPreferencesChanged Delegate;
	return Delegate;
}
