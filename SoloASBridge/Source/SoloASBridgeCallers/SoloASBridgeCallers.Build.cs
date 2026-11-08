// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
using System;
using System.IO;
using System.Collections.Generic;
using System.Linq;
using EpicGames.Core;
using UnrealBuildTool;

public sealed class SoloASBridgeCallers : ModuleRules
{
    public SoloASBridgeCallers(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrecompileForTargets = PrecompileTargetsType.Any;
        bUsePrecompiled = UnrealBuildTool.Rules.SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Projects", "Json", "SoloASBridge" });
        RuntimeDependencies.Add("$(PluginDir)/Config/NativeMetadata/*.json.gz", StagedFileType.NonUFS);
    }
}

public abstract class SoloASNativeCallerRules : ModuleRules
{
    protected SoloASNativeCallerRules(ReadOnlyTargetRules Target, string NativeModule,
        string NativePlugin, string Source, string HostType, bool SourceBuildRequired = false) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrecompileForTargets = PrecompileTargetsType.Any;
        bUsePrecompiled = UnrealBuildTool.Rules.SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "AssetRegistry", "Slate", "SlateCore" });
        // Typed value callers use the native types' exported constructors and inline dependencies.
        PrivateDependencyModuleNames.AddRange(new[] { "GameplayTags", "MovieScene", "Chaos", "SoloASBridge", "Projects" });
                                                                                                                                             
        if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion == 8)
        {
            // These reflected APIs live in private headers in the installed 5.8 SDK.
            // No engine implementation is compiled; the caller still targets the original C++ function.
            if (new[] { "SlateRHIRenderer", "Niagara", "PCG", "UnrealEd", "MediaStream",
                "ModelViewViewModelEditor", "AvalancheModifiers", "DynamicMaterialEditor", "Text3D" }.Contains(NativeModule))
                PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory(NativeModule), "Private"));
            // 5.8's reflected array-mesh API uses an Internal DataInterface header.
            if (NativeModule == "Niagara") PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory(NativeModule), "Internal"));
        }
        // SOLOAS-COMPAT-END
                                                                                                       
        // Synthesis public widget headers need UMG in Game; UnrealEd masks this dependency in Editor.
        if (NativeModule == "Synthesis") PrivateDependencyModuleNames.Add("UMG");
                                                                                                     
        // PhysicsControl's public blueprint library uses this node-reference API on 5.4 and 5.8.
        if (NativeModule == "PhysicsControl") PrivateDependencyModuleNames.Add("AnimGraphRuntime");
        // SOLOAS-COMPAT-BEGIN(==5.4) Guard the exact UE 5.4 quality-level SDK source used by native callers [test: SoloAS.NativeReferenceReturns] [id: CS.Compatibility.Bridge.SoloASBridgeCallers.QualityLevel54.SourceGuard]
        if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion == 4 && NativeModule == "Engine")
        {
            RequireQualityLevelSource("Source/Runtime/Engine/Classes/Engine/SkeletalMesh.h", "87594e1684c2470b89cef6b7998228b467b51a560b67804ae0f689b58ddab0b3");
            RequireQualityLevelSource("Source/Runtime/Engine/Classes/Engine/StaticMesh.h", "d728a76c09e6a93028ddee6ec4a0183a93786153ce454bc6ddac30baed843881");
            RequireQualityLevelSource("Source/Runtime/Engine/Private/PerQualityLevelProperties.cpp", "6fe69f0ff1e8acc8e3da0202669c46ff6dced68bf54c0c9da853c5f859493364");
        }
        // SOLOAS-COMPAT-END
                                                                                                      
        ConfigureNativeCaller(Target, NativeModule, NativePlugin, Source, SourceBuildRequired);
        // UBT carries PublicDefinitions into each module's UHT manifest on UE 5.4-5.8.
        PublicDefinitions.Add("SOLOAS_NATIVE_CALLER_AVAILABLE="
            + (PrivateDependencyModuleNames.Contains(NativeModule) ? "1" : "0"));
        // The public relative-body modifier headers include AnimationModifier.h.
        if (NativeModule == "RelativeBodyAnimUtils" && PrivateDependencyModuleNames.Contains(NativeModule))
            PrivateDependencyModuleNames.Add("AnimationModifiers");
        if (NativeModule == "ChaosClothAssetEngine" && PrivateDependencyModuleNames.Contains(NativeModule))
            PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory(NativeModule), "Private"));
        // CS.Bridge.UHT.CommonUiMacUmgLink: WidgetStateBitfield is exported by UMG;
        // Mach-O's two-level namespace needs the direct dylib dependency.
        if (Target.Platform == UnrealTargetPlatform.Mac && Target.LinkType == TargetLinkType.Modular
            && Target.Type == TargetType.Editor && NativeModule == "CommonUI"
            && PrivateDependencyModuleNames.Contains(NativeModule))
            PrivateDependencyModuleNames.Add("UMG");
// SOLOAS-COMPAT-BEGIN(>=5.6 && <=5.7) Retain exact private native caller headers and Text3D font dependencies on the legacy SDKs [test: SoloAS.NativeReferenceReturns] [id: CS.Compatibility.Bridge.SoloASBridgeCallers.LegacyPrivateHeaderInputs]
        if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 6
            && Target.Version.MinorVersion <= 7 && PrivateDependencyModuleNames.Contains(NativeModule))
        {
            // These reflected APIs live in private headers in the installed 5.6-5.7 SDKs.
            // No engine implementation is compiled; the caller still targets the original C++ function.
            if (new[] { "SlateRHIRenderer", "Niagara", "PCG", "UnrealEd", "MediaStream",
                "ModelViewViewModelEditor", "AvalancheModifiers", "DynamicMaterialEditor", "Text3D" }.Contains(NativeModule))
                PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory(NativeModule), "Private"));
            // The reflected array-mesh API uses an Internal DataInterface header.
            if (NativeModule == "Niagara") PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory(NativeModule), "Internal"));
            // CS.Bridge.UHT.Text3DFontDependencies56And57: the installed private
            // Text3DModule.h exposes ft2build.h; mirror Text3D.Build.cs on UE5.6/5.7.
            if (NativeModule == "Text3D" && Target.Type != TargetType.Server)
            {
                if (Target.bCompileFreeType) PrivateDependencyModuleNames.Add("FreeType2");
                PrivateDependencyModuleNames.Add("HarfBuzz");
            }
        }
// SOLOAS-COMPAT-END
                                                                                                    
    }

    // SOLOAS-COMPAT-BEGIN(==5.4) Guard the exact UE 5.4 quality-level SDK source used by native callers [test: SoloAS.NativeReferenceReturns] [id: CS.Compatibility.Bridge.SoloASBridgeCallers.QualityLevel54.SourceGuard.Method]
    private void RequireQualityLevelSource(string RelativePath, string ExpectedSha256)
    {
        string SourcePath = Path.Combine(EngineDirectory, RelativePath);
        ExternalDependencies.Add(SourcePath);
        if (!File.Exists(SourcePath)) throw new BuildException("SOLOAS_QUALITY_LEVEL_SOURCE_MISSING: " + RelativePath);
        System.Type Sha256 = System.Type.GetType("System.Security.Cryptography.SHA256, System.Security.Cryptography.Algorithms", true);
        byte[] Hash = (byte[])Sha256.GetMethod("HashData", new[] { typeof(byte[]) })
            .Invoke(null, new object[] { System.Array.FindAll(File.ReadAllBytes(SourcePath), Value => Value != 13) });
        string Actual = BitConverter.ToString(Hash).Replace("-", "").ToLowerInvariant();
        if (Actual != ExpectedSha256) throw new BuildException("SOLOAS_QUALITY_LEVEL_SOURCE_CHANGED: requalify UE5.4 native caller: " + RelativePath);
    }
    // SOLOAS-COMPAT-END
    private void ConfigureNativeCaller(ReadOnlyTargetRules Target, string NativeModule,
        string NativePlugin, string Source, bool SourceBuildRequired)
    {
        if (SourceBuildRequired) return;
        // CS.Bridge.UHT.MonolithicArchiveSelection
        if (Target.LinkType == TargetLinkType.Monolithic && !Target.bPrecompile
            && NativePlugin.Length != 0 && !EnabledNativePlugins(Target).Contains(NativePlugin))
            return;
        if (Target.bBuildEditor) PrivateDependencyModuleNames.Add("UnrealEd");
        if (NativePlugin.Length == 0)
        {
            if (Directory.Exists(Path.Combine(EngineDirectory, Source)))
                PrivateDependencyModuleNames.Add(NativeModule);
            return;
        }
        PluginInfo Plugin = Plugins.ReadEnginePlugins(new DirectoryReference(EngineDirectory))
            .FirstOrDefault(plugin => plugin.Name == NativePlugin);
        if (Plugin == null || !Plugin.Descriptor.SupportsTargetPlatform(Target.Platform)) return;
                                                                                                                                                     
        var CompilePredicate = typeof(ModuleDescriptor).GetMethods().Single(method =>
            method.Name == "IsCompiledInConfiguration" && method.GetParameters().Length ==
                (Target.Version.MinorVersion >= 7 ? 7 : 6));
        // SOLOAS-COMPAT-END
        ModuleDescriptor Owner = Plugin.Descriptor.Modules.FirstOrDefault(module => module.Name == NativeModule);
        if (Owner == null) return;
        object[] OwnerArguments = { Target.Platform, Target.Configuration, Target.Name,
            Target.Type, Target.bBuildDeveloperTools, Target.bBuildRequiresCookedData };
                                                                                                                                                  
        if (Target.Version.MinorVersion >= 7)
            OwnerArguments = OwnerArguments.Concat(new object[] { Target.Architectures }).ToArray();
        // SOLOAS-COMPAT-END
        if (!(bool)CompilePredicate.Invoke(Owner, OwnerArguments)) return;
                                                                                                                                                       
        if (Target.Version.MinorVersion <= 5 && NativeModule == "AvalancheModifiers")
            PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory(NativeModule), "Private"));
        // SOLOAS-COMPAT-END
                                                                                                                                                     
        if (Target.Version.MinorVersion == 4 && NativeModule == "CustomizableObject")
            PrivateIncludePaths.Add(GetModuleDirectory(NativeModule));
        // SOLOAS-COMPAT-END
        // CS.Bridge.UHT.CaptureConversionTypeInclude
                                                                                                                                                  
        if (NativeModule == "LiveLinkCapabilities" && Target.Version.MinorVersion >= 8)
            PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("CaptureDataConverter"), "Internal"));
        // SOLOAS-COMPAT-END
        Dictionary<string, PluginInfo> SDK = Plugins.ReadEnginePlugins(new DirectoryReference(EngineDirectory))
            .ToDictionary(plugin => plugin.Name, StringComparer.OrdinalIgnoreCase);
                                                                                                                                                  
        if (Target.Version.MinorVersion == 6)
            PrivateDependencyModuleNames.Add(NativeModule);
        // SOLOAS-COMPAT-END
        Queue<PluginInfo> Pending = new Queue<PluginInfo>();
        HashSet<string> Seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        Pending.Enqueue(Plugin);
        while (Pending.Count != 0)
        {
            PluginInfo Dependency = Pending.Dequeue();
            if (!Seen.Add(Dependency.Name) || !Dependency.Descriptor.SupportsTargetPlatform(Target.Platform)) continue;
            foreach (ModuleDescriptor Module in Dependency.Descriptor.Modules ?? new List<ModuleDescriptor>())
            {
                object[] Arguments = { Target.Platform, Target.Configuration, Target.Name,
                    Target.Type, Target.bBuildDeveloperTools, Target.bBuildRequiresCookedData };
                                                                                                                                                               
                if (Target.Version.MinorVersion >= 7)
                    Arguments = Arguments.Concat(new object[] { Target.Architectures }).ToArray();
                // SOLOAS-COMPAT-END
                                                                                                                           
                if ((bool)CompilePredicate.Invoke(Module, Arguments))
                {
                    if (Target.Version.MinorVersion == 6)
                        PrivateIncludePathModuleNames.Add(Module.Name);
                    else
                        PrivateDependencyModuleNames.Add(Module.Name);
                }
                                                                                                                         
            }
            foreach (PluginReferenceDescriptor Reference in Dependency.Descriptor.Plugins ?? new List<PluginReferenceDescriptor>())
                if (Reference.bEnabled && Reference.IsEnabledForPlatform(Target.Platform)
                    && Reference.IsEnabledForTargetConfiguration(Target.Configuration)
                    && Reference.IsEnabledForTarget(Target.Type) && SDK.TryGetValue(Reference.Name, out PluginInfo Child))
                    Pending.Enqueue(Child);
        }
    }
    private HashSet<string> EnabledNativePlugins(ReadOnlyTargetRules Target)
    {
        ProjectDescriptor Project = Target.ProjectFile == null ? null : ProjectDescriptor.FromFile(Target.ProjectFile);
        List<DirectoryReference> Extra = Project?.AdditionalPluginDirectories ?? new List<DirectoryReference>();
        List<PluginInfo> Available = Plugins.ReadAvailablePlugins(new DirectoryReference(EngineDirectory),
            Target.ProjectFile?.Directory, Extra);
        Dictionary<string, PluginInfo> ByName = Available.GroupBy(plugin => plugin.Name)
            .ToDictionary(group => group.Key, group => group.Last(), StringComparer.OrdinalIgnoreCase);
        HashSet<string> Disabled = new HashSet<string>(Target.DisablePlugins, StringComparer.OrdinalIgnoreCase);
        foreach (PluginReferenceDescriptor Reference in Project?.Plugins ?? Array.Empty<PluginReferenceDescriptor>())
            if (!Reference.bEnabled) Disabled.Add(Reference.Name);
        HashSet<string> Enabled = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (PluginInfo Plugin in Available)
            if (!Disabled.Contains(Plugin.Name) && (Target.EnablePlugins.Contains(Plugin.Name)
                || Plugins.IsPluginEnabledForTarget(Plugin, Project, Target.Platform, Target.Configuration, Target.Type)))
                Enabled.Add(Plugin.Name);
        Queue<string> Pending = new Queue<string>(Enabled);
        while (Pending.Count != 0)
        {
            PluginInfo Plugin = ByName[Pending.Dequeue()];
            foreach (PluginReferenceDescriptor Reference in Plugin.Descriptor.Plugins ?? new List<PluginReferenceDescriptor>())
                if (Reference.bEnabled && !Disabled.Contains(Reference.Name)
                    && Reference.IsEnabledForPlatform(Target.Platform)
                    && Reference.IsEnabledForTargetConfiguration(Target.Configuration)
                    && Reference.IsEnabledForTarget(Target.Type) && ByName.ContainsKey(Reference.Name)
                    && Enabled.Add(Reference.Name))
                    Pending.Enqueue(Reference.Name);
        }
        return Enabled;
    }

}
