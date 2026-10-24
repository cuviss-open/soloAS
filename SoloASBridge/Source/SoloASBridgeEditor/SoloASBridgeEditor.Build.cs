using System.IO;
using UnrealBuildTool;

public sealed class SoloASBridgeEditor : ModuleRules
{
    public SoloASBridgeEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrecompileForTargets = PrecompileTargetsType.Any;
        bUsePrecompiled = UnrealBuildTool.Rules.SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);

        // Resolve/install only after the existing UMG module loads; do not add a load dependency.
        PrivateIncludePathModuleNames.Add("UMG");

        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject" });
        PrivateDependencyModuleNames.AddRange(
            new[] { "UnrealEd", "Engine", "BlueprintGraph", "KismetCompiler",
                "Kismet", "DataValidation", "StateTreeEditorModule",
                "PropertyEditor", "Slate", "SlateCore", "ClassViewer", "SoloASBridge",
                "SceneOutliner"
            }
        );

        if (Target.bBuildEditor && Target.LinkType == TargetLinkType.Modular
            && Target.Platform == UnrealTargetPlatform.Win64)
        {
            PrivateDependencyModuleNames.Add("Detours");
        }
    }
}
