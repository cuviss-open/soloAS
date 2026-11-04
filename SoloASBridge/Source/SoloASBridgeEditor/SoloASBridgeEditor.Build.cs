using System.IO;
using UnrealBuildTool;

public sealed class SoloASBridgeEditor : ModuleRules
{
    public SoloASBridgeEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
                                                                                                                                             
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private", "Compat",
            $"UE{Target.Version.MajorVersion}{Target.Version.MinorVersion}"));
        // SOLOAS-COMPAT-END
        PrecompileForTargets = PrecompileTargetsType.Any;
        bUsePrecompiled = UnrealBuildTool.Rules.SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);

        // Resolve/install only after the existing UMG module loads; do not add a load dependency.
        PrivateIncludePathModuleNames.Add("UMG");

        // category=A; basis=UE-AS fork StateTreeNodeClassCache::CacheClasses script-node discovery; StateTree remains optional.
        bool WithStateTree = UnrealBuildTool.Rules.SoloASBridgeBuildPath.IsStateTreeEnabled(Target, EngineDirectory);
        PrivateDefinitions.Add("SOLOAS_WITH_STATETREE=" + (WithStateTree ? "1" : "0"));
        if (WithStateTree) PrivateDependencyModuleNames.Add("StateTreeEditorModule");

        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject" });
        PrivateDependencyModuleNames.AddRange(
            new[] { "UnrealEd", "Engine", "BlueprintGraph", "KismetCompiler",
                "Kismet", "DataValidation",
                "PropertyEditor", "Slate", "SlateCore", "ClassViewer", "SoloASBridge",
                "SceneOutliner", "ToolMenus"
            }
        );

                                                                                                  
        PrivateDependencyModuleNames.AddRange(new[] { "ToolMenus", "Projects", "HTTP", "Json" });
                                                                                                

        if (Target.bBuildEditor && Target.LinkType == TargetLinkType.Modular
            && Target.Platform == UnrealTargetPlatform.Win64)
        {
            PrivateDependencyModuleNames.Add(UnrealBuildTool.Rules.SoloASBridgeBuildPath.EditorDetoursModule(Target));
        }
    }
}
