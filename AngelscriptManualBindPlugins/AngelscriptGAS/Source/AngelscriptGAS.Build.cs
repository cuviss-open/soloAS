using System.IO;
using UnrealBuildTool;

namespace UnrealBuildTool.Rules
{
	public class AngelscriptGAS : ModuleRules
	{
		public AngelscriptGAS(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
                                                                                                                 
            PrecompileForTargets = PrecompileTargetsType.Any;
            bUsePrecompiled = SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
            PrivateDependencyModuleNames.Add("SoloASBridge");
                                                                                                               

			PublicIncludePaths.AddRange(new string[]
			{
				ModuleDirectory + "/Public",
				ModuleDirectory + "/Public/FunctionLibraries",
			});

			PublicDependencyModuleNames.AddRange(new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"PhysicsCore",

				"AngelscriptCode",

				"GameplayAbilities",
				"GameplayTags",
				"GameplayTasks"
			});
		}
	}
}
