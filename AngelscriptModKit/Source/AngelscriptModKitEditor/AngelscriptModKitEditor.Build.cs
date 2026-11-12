// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
using UnrealBuildTool;

public sealed class AngelscriptModKitEditor : ModuleRules
{
	public AngelscriptModKitEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUsePrecompiled = UnrealBuildTool.Rules.SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
		PublicIncludePaths.Add(ModuleDirectory + "/Public");
		PrivateIncludePaths.Add(ModuleDirectory + "/Private");

		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "DeveloperSettings" });

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"UnrealEd",
			"AngelscriptModKit",
			"GameFeatures",
			"AngelscriptCode",
			"Projects",
			"Settings",
			"PropertyEditor",
			"ToolMenus",
			"Slate",
			"SlateCore",
			"InputCore",
			"Json",
			"JsonUtilities"
		});
	}
}
