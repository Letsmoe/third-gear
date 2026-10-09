using UnrealBuildTool;

public class MapRuntime : ModuleRules
{
	public MapRuntime(ReadOnlyTargetRules Target) : base(Target)
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
			"Projects",
			"RenderCore",
			"GeometryCore",
			"GeometryFramework",
			"GeometryAlgorithms",
			"Json",
			"AssetRegistry",
		});
	}
}
