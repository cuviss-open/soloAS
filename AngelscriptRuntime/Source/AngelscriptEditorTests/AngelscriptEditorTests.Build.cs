// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
using System.IO;
using UnrealBuildTool;

public class AngelscriptEditorTests : ModuleRules
{
    public AngelscriptEditorTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("AngelscriptEditor"), "Private"));
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "UnrealEd", "AngelscriptCode", "AngelscriptEditor",
            "SoloASBridge", "SoloASBridgeEditor", "StructUtils", "BlueprintGraph", "Kismet"
        });
    }
}
