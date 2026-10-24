using System.IO;
using UnrealBuildTool;

namespace UnrealBuildTool.Rules
{
	public class AngelscriptEnhancedInput : ModuleRules
	{
		public AngelscriptEnhancedInput(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
                                                                                                                           
            PrecompileForTargets = PrecompileTargetsType.Any;
            bUsePrecompiled = SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
            PrivateDependencyModuleNames.Add("SoloASBridge");
                                                                                                                         

			PublicIncludePaths.AddRange(new string[]
			{
				ModuleDirectory + "/Public",
			});

			PublicDependencyModuleNames.AddRange(new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"Slate",

				"AngelscriptCode",

				"EnhancedInput",
			});
		}
	}
}
