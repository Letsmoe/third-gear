using UnrealBuildTool;

public class DrivingGameEditorTarget : TargetRules
{
	public DrivingGameEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "DrivingGame", "DrivingGameEditor" });
	}
}
