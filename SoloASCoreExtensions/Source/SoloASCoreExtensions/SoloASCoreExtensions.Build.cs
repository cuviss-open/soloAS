// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
using System;
using System.IO;
using System.Linq;
using System.Text;
using UnrealBuildTool;

public sealed class SoloASCoreExtensions : ModuleRules
{
    public SoloASCoreExtensions(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrecompileForTargets = PrecompileTargetsType.Any;
        bUsePrecompiled = Directory.GetFiles(ModuleDirectory, "*.cpp", SearchOption.AllDirectories).Length == 0;

        using (var SourceBytes = new MemoryStream())
        {
            foreach (var FilePath in Directory.GetFiles(ModuleDirectory, "*", SearchOption.AllDirectories)
                .Where(Path => (Path.EndsWith(".cpp") || Path.EndsWith(".h") || Path.EndsWith(".Build.cs"))
                    && !Path.Replace('\\', '/').Contains("/Tests/"))
                .OrderBy(Path => Path, StringComparer.Ordinal))
            {
                var RelativePath = Path.GetRelativePath(ModuleDirectory, FilePath).Replace('\\', '/');
                var NameBytes = Encoding.UTF8.GetBytes(RelativePath);
                SourceBytes.Write(NameBytes, 0, NameBytes.Length);
                SourceBytes.WriteByte(0);
                var FileBytes = File.ReadAllBytes(FilePath);
                SourceBytes.Write(FileBytes, 0, FileBytes.Length);
                SourceBytes.WriteByte(0);
                ExternalDependencies.Add(FilePath);
            }
            var Digest = EpicGames.Core.Digest.Compute<EpicGames.Core.Sha256>(SourceBytes.ToArray()).ToString().ToLowerInvariant();
            PrivateDefinitions.Add("SOLOAS_CE_BINDING_SOURCE_ID=\"" + Digest + "\"");
        }
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));

        PublicDependencyModuleNames.AddRange(new[] { "AngelscriptCode", "Core", "CoreUObject", "StructUtils" });

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "SoloASBridge",
                "Engine",
                "Projects",
                "Json",
                "AssetRegistry",
            }
        );
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new[] { "ToolMenus", "Slate", "SlateCore" });
        }
        if (Target.bBuildEditor && Target.LinkType != TargetLinkType.Monolithic
            && (Target.Platform == UnrealTargetPlatform.Win64 || Target.Platform == UnrealTargetPlatform.Mac))
        {
            PrivateDependencyModuleNames.AddRange(new[] { "SoloASBridgeEditor", "UnrealEd", "BlueprintGraph" });
        }
    }
}
