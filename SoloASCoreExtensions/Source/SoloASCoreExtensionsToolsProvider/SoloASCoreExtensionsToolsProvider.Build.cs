// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
using UnrealBuildTool;

public sealed class SoloASCoreExtensionsToolsProvider : ModuleRules
{
    public SoloASCoreExtensionsToolsProvider(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "AngelscriptCode", "SoloASBridge", "SoloASCoreExtensions", "Json"
        });
        // SOLOAS-COMPAT-BEGIN(5.7-5.8) Enable editor protection dependencies on both supported versions [test: SoloAS.CoreExtensions.BlueprintInheritance.ParentRecompile] [id: CS.Compatibility.CoreExtensions.BlueprintParentEditor.BuildVersions]
        if (Target.bBuildEditor && Target.LinkType != TargetLinkType.Monolithic
            && Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 7 && Target.Version.MinorVersion < 9)
        {
            PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "Kismet", "Slate", "SlateCore" });
            if (Target.Platform == UnrealTargetPlatform.Win64)
                PrivateDependencyModuleNames.Add(UnrealBuildTool.Rules.SoloASBridgeBuildPath.EditorDetoursModule(Target));
        }
        // SOLOAS-COMPAT-END
    }
}
