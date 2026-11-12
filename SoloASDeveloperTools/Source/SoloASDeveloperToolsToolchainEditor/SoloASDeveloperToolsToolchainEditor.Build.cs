// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
using UnrealBuildTool;

public sealed class SoloASDeveloperToolsToolchainEditor : ModuleRules
{
    public SoloASDeveloperToolsToolchainEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUsePrecompiled = UnrealBuildTool.Rules.SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "DeveloperSettings", "DesktopPlatform",
            "UnrealEd", "ToolMenus", "Slate", "SlateCore", "Settings", "Json", "XmlParser", "Projects"
        });
    }
}
