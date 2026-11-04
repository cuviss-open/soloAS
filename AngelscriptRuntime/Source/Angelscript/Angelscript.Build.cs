// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
using UnrealBuildTool;

public class Angelscript : ModuleRules
{
    public Angelscript(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrecompileForTargets = PrecompileTargetsType.Any;
        bUsePrecompiled = UnrealBuildTool.Rules.SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
        PrivateDependencyModuleNames.Add("Core");
    }
}
