using UnrealBuildTool;

public class Courier : ModuleRules
{
	public Courier(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HTTP",
			"Json"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"JsonUtilities",
			"WebSockets"
		});
	}
}
