using UnrealBuildTool;

public class DrivingGame : ModuleRules
{
	public DrivingGame(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"HeadMountedDisplay",
			"XRBase",
			"WheelInput",
			"DeveloperSettings",
			"PhysicsCore",
			"Chaos",
			"ChaosVehicles",
			"ChaosVehiclesCore",
			"Isobar",
		});
	}
}
