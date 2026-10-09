#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * Decides how a level starts: in the start menu, straight in the car, or in the free camera. A launch without any of the
 * test switches shows the start menu; the menus open levels with the URL options Drive, FreeCam and Menu to say what they want.
 */
namespace GameFlow
{
/** Map the menus load; the world around the player is generated into it at runtime. */
extern const TCHAR* const StreamedMapPath;

/** True for launches that must skip the start menu: unattended runs and the test and screenshot switches, or -NoMenu. */
bool IsBatchLaunch();

/** True if this level should show the start menu with a slowly turning camera instead of a car. */
bool IsStartMenuLevel(const UWorld* World);

/** True if this level uses the free-fly camera pawn instead of the car (start menu, free camera, or a test switch). */
bool IsFreeCameraLevel(const UWorld* World);

/** The page named by -MenuPage= (Main, Pause, Settings, Wheel, Graphics or Audio), or an empty string. For screenshots. */
FString GetDebugMenuPage();
}
