using UnrealBuildTool;

public class DrivingGameTarget : TargetRules
{
	public DrivingGameTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("DrivingGame");
	}
}
