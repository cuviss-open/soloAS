using System.IO;
using UnrealBuildTool;

namespace UnrealBuildTool.Rules
{
	public class AngelscriptGAS : ModuleRules
	{
		public AngelscriptGAS(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
                                                                                            
            // UHT scans module directories independently of C++ include paths.
                                                                                                                                                
            string TagApi = Target.Version.MajorVersion > 5 ||
                (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 7) ? "UE57" : "Legacy";
            // SOLOAS-COMPAT-END
            var TagDirectory = new EpicGames.Core.DirectoryReference(Path.Combine(PluginDirectory, "Versioned", TagApi));
            ConditionalAddModuleDirectory(TagDirectory);
            PublicIncludePaths.Add(Path.Combine(TagDirectory.FullName, "Public"));
                                                                                          
                                                                                                                 
            ShortName = "AG";
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
