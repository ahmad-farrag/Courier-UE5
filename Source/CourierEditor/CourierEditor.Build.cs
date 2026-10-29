using UnrealBuildTool;

public class CourierEditor : ModuleRules
{
	public CourierEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"BlueprintGraph",
			"Courier"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"AssetDefinition",
			"AssetRegistry",
			"AssetTools",
			"DesktopPlatform",
			"HTTP",
			"InputCore",
			"Json",
			"JsonUtilities",
			"KismetCompiler",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UnrealEd",
			"WebSockets",
			"WorkspaceMenuStructure"
		});
	}
}
