using UnrealBuildTool;

public class DrivingGameEditor : ModuleRules
{
	public DrivingGameEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"MeshDescription",
			"StaticMeshDescription",
			"AssetRegistry",
			"PhysicsCore",
		});
	}
}
