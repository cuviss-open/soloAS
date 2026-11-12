// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
using UnrealBuildTool;

public class SoloASDeveloperToolsEditor : ModuleRules
{
    public SoloASDeveloperToolsEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUsePrecompiled = UnrealBuildTool.Rules.SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
        PublicDependencyModuleNames.Add("Core");
        PrivateDependencyModuleNames.AddRange(new[] {
            "CoreUObject", "Engine", "UnrealEd", "ApplicationCore", "ToolMenus", "Slate", "SlateCore", "InputCore",
            "Json", "Projects", "DeveloperSettings", "Settings", "AngelscriptCode", "SoloASDeveloperToolsProviders", "Sockets", "Networking"
        });
    }
}
