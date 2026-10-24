using System.IO;
using UnrealBuildTool;

namespace UnrealBuildTool.Rules
{
	public class AngelscriptLoader : ModuleRules
	{
		public AngelscriptLoader(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
			PrecompileForTargets = PrecompileTargetsType.Any;
			bUsePrecompiled = SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);

			PublicDependencyModuleNames.AddRange(new string[]
			{
				"Core",
				"Engine",
				"AngelscriptCode",
			});

			if (Target.bBuildEditor)
			{
				PublicDependencyModuleNames.AddRange(new string[] 
				{
					"AngelscriptEditor", 
				});
			}
		}
	}
}
