using UnrealBuildTool;

// Blueprint graph nodes. UncookedOnly: the nodes expand into plain function calls when a Blueprint compiles,
// so cooked games never load this module, but runtime Blueprints can use the nodes without editor-only warnings.
public class CourierBlueprint : ModuleRules
{
	public CourierBlueprint(ReadOnlyTargetRules Target) : base(Target)
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
			"AssetRegistry",
			"KismetCompiler",
			"UnrealEd"
		});
	}
}
