#include "WheelInputSettings.h"

const TArray<FName>& UWheelInputSettings::GetUserEditableProperties()
{
	static const TArray<FName> Names = {
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, WheelRangeDegrees),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, SteeringAxis),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, bInvertSteering),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, ThrottleAxis),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, BrakeAxis),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, ClutchAxis),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, bInvertThrottle),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, bInvertBrake),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, bInvertClutch),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, PedalDeadzone),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, BrakeFullTravel),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, GearButtonIndices),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, ReverseButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, StartEngineButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, HandbrakeButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, RecenterViewButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, ResetCarButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, LowBeamButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, HighBeamButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, IndicatorLeftButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, IndicatorRightButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, HazardButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, RadioToggleButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, RadioNextButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, RadioPreviousButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, MenuButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, MenuConfirmButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, MenuBackButtonIndex),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, DPadXAxis),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, DPadYAxis),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, bInvertForce),
		GET_MEMBER_NAME_CHECKED(UWheelInputSettings, ForceGain),
	};
	return Names;
}
