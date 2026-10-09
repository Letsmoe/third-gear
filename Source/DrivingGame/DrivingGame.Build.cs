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
			"Slate",
			"SlateCore",
			"UMG",
			"ApplicationCore",
			"HeadMountedDisplay",
			"XRBase",
			"WheelInput",
			"DeveloperSettings",
			"PhysicsCore",
			"Chaos",
			"ChaosVehicles",
			"ChaosVehiclesCore",
			"MapRuntime",
			"Isobar",
		});
	}
}
