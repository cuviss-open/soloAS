// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
using UnrealBuildTool;

public class SoloASDeveloperToolsProviders : ModuleRules
{
    public SoloASDeveloperToolsProviders(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUsePrecompiled = UnrealBuildTool.Rules.SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
        PublicDependencyModuleNames.Add("Core");
        PrivateDependencyModuleNames.Add("Json");
    }
}
