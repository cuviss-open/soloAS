using System.IO;
using System.Linq;
using EpicGames.Core;
using UnrealBuildTool;

namespace UnrealBuildTool.Rules
{
    public static class SoloASBridgeBuildPath
    {
        // category=A; basis=UE-AS fork StateTreeNodeClassCache::CacheClasses script-node discovery; StateTree remains optional.
        public static bool IsStateTreeEnabled(ReadOnlyTargetRules Target, string EngineDirectory)
        {
            if (!Target.bBuildEditor || Target.DisablePlugins.Contains("StateTree")) return false;
            ProjectDescriptor Project = Target.ProjectFile == null ? null : ProjectDescriptor.FromFile(Target.ProjectFile);
            var Available = Plugins.ReadAvailablePlugins(new DirectoryReference(EngineDirectory),
                Target.ProjectFile?.Directory, null);
            var Enabled = new System.Collections.Generic.HashSet<string>(System.StringComparer.OrdinalIgnoreCase);
            foreach (var Plugin in Available)
                if (!Target.DisablePlugins.Contains(Plugin.Name)
                    && (Target.EnablePlugins.Contains(Plugin.Name)
                        || Plugins.IsPluginEnabledForTarget(Plugin, Project, Target.Platform, Target.Configuration, Target.Type)))
                    Enabled.Add(Plugin.Name);
            bool Changed;
            do
            {
                Changed = false;
                foreach (var Plugin in Available)
                {
                    if (!Enabled.Contains(Plugin.Name) || Plugin.Descriptor.Plugins == null) continue;
                    foreach (var Dependency in Plugin.Descriptor.Plugins)
                        if (!Dependency.bOptional && Dependency.IsEnabledForPlatform(Target.Platform)
                            && Dependency.IsEnabledForTargetConfiguration(Target.Configuration)
                            && Dependency.IsEnabledForTarget(Target.Type)
                            && !Target.DisablePlugins.Contains(Dependency.Name))
                            Changed |= Enabled.Add(Dependency.Name);
                }
            } while (Changed);
            return Enabled.Contains("StateTree");
        }

        public static string EditorDetoursModule(ReadOnlyTargetRules Target)
        {
                                                                                                                                          
            return Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 8
                ? "SoloASDetours" : "Detours";
            // SOLOAS-COMPAT-END
        }

        public static bool MatchesEngineProfile(
            string ActualVersion, int ActualCompatibleChangelist,
            string MinorVersion, int MinimumPatch, int CompatibleChangelist)
        {
            string[] Parts = ActualVersion.Split(new char[] { '.', '-', '+' }, 4);
            int Patch;
            return Parts.Length == 4 && Parts[0] + "." + Parts[1] == MinorVersion
                && int.TryParse(Parts[2], out Patch) && Patch >= MinimumPatch
                && ActualCompatibleChangelist == CompatibleChangelist;
        }

        public static void ConfigureEngineProfile(ModuleRules Module, string EngineRoot)
        {
            string ProfilePath = Path.Combine(Module.PluginDirectory, "Config", "EngineCompatibility.json");
            string VersionPath = Path.Combine(EngineRoot, "Build", "Build.version");
            Module.ExternalDependencies.Add(ProfilePath);
            Module.ExternalDependencies.Add(VersionPath);
            JsonObject Profile = JsonObject.Read(new FileReference(ProfilePath));
            JsonObject Version = JsonObject.Read(new FileReference(VersionPath));
            string ActualVersion = Version.GetIntegerField("MajorVersion") + "."
                + Version.GetIntegerField("MinorVersion") + "." + Version.GetIntegerField("PatchVersion")
                + "-" + Version.GetIntegerField("Changelist") + "+" + Version.GetStringField("BranchName");
            string MinorVersion = Version.GetIntegerField("MajorVersion") + "." + Version.GetIntegerField("MinorVersion");
            JsonObject Support = Profile.GetObjectField("support").GetObjectField(MinorVersion);
            int MinimumPatch = Support.GetIntegerField("minimumPatch");
            JsonObject Patches = Support.GetObjectField("patches");
            if (Support.TryGetBoolField("supported", out bool Enabled) && !Enabled
                && (!Patches.TryGetObjectField(Version.GetIntegerField("PatchVersion").ToString(), out JsonObject Candidate)
                    || !Candidate.TryGetObjectField("installedBuild", out JsonObject CandidateBuild)))
                throw new BuildException("SOLOASBRIDGE_ENGINE_PROFILE_PLANNED: " + ActualVersion);
            JsonObject Baseline = Patches.GetObjectField(Patches.KeyNames.First()).GetObjectField("installedBuild");
            bool Supported = MatchesEngineProfile(ActualVersion, Version.GetIntegerField("CompatibleChangelist"),
                MinorVersion, MinimumPatch, Baseline.GetIntegerField("compatibleChangelist"));
            if (Support.GetStringField("policy") == "last-patch-only")
                Supported = Supported && Version.GetIntegerField("PatchVersion") == Patches.KeyNames.Select(int.Parse).Max();
            else
                Supported = Supported && Support.GetStringField("policy") == "track-all-patches";
            Supported = Supported && Version.GetStringField("BranchName") == Baseline.GetStringField("branch")
                && Version.GetIntegerField("Changelist") >= Baseline.GetIntegerField("compatibleChangelist");
            if (Patches.TryGetObjectField(Version.GetIntegerField("PatchVersion").ToString(), out JsonObject KnownPatch))
            {
                JsonObject Installed = KnownPatch.GetObjectField("installedBuild");
                Supported = Supported && Version.GetIntegerField("Changelist") == Installed.GetIntegerField("changelist")
                    && Version.GetIntegerField("CompatibleChangelist") == Installed.GetIntegerField("compatibleChangelist")
                    && Version.GetStringField("BranchName") == Installed.GetStringField("branch");
            }
            if (!Supported)
                throw new BuildException("SOLOASBRIDGE_ENGINE_PROFILE_MISMATCH: " + ActualVersion);
            Module.PublicDefinitions.Add("SOLOASBRIDGE_ENGINE_PROFILE_MATCHED=1");
        }

        public static bool ShouldUsePrecompiled(
            ModuleRules Module, ReadOnlyTargetRules Target, bool ReadOnlyDefault)
        {
            if (System.Environment.GetEnvironmentVariable("SOLOAS_BUILD_FROM_SOURCE") == "1")
                return false;
            if (ReadOnlyDefault)
                return true;

            string Descriptor = Directory.GetFiles(Module.PluginDirectory, "*.uplugin")[0];
            if (!PluginDescriptor.FromFile(new FileReference(Descriptor)).bInstalled)
                return false;

            string Name = Module.GetType().Name;
            string Platform = Target.Platform.ToString();
            string Extension = Target.Platform == UnrealTargetPlatform.Win64 ? ".dll"
                : Target.Platform == UnrealTargetPlatform.Mac ? ".dylib" : ".so";
            string Prefix = Target.Platform == UnrealTargetPlatform.Linux ? "libUnrealEditor-" : "UnrealEditor-";
            if (!File.Exists(Path.Combine(Module.PluginDirectory, "Binaries", Platform, Prefix + Name + Extension)))
                return false;
            if (Target.Type == TargetType.Editor)
                return true;

            string DirectoryName = string.IsNullOrEmpty(Module.ShortName) ? Name : Module.ShortName;
            string BuildRoot = Path.Combine(Module.PluginDirectory, "Intermediate", "Build", Platform);
            string Manifest = Path.Combine("UnrealGame", Target.Configuration.ToString(),
                DirectoryName, Name + ".precompiled");
            return Target.Type == TargetType.Game
                && (File.Exists(Path.Combine(BuildRoot, Target.Architecture.ToString(), Manifest))
                    || File.Exists(Path.Combine(BuildRoot, Manifest)));
        }

        public static string RequireConsumerThirdPartyDirectory(
            ModuleRules consumerRules
        )
        {
            string thirdPartyDirectory = Path.GetFullPath(
                Path.Combine(consumerRules.PluginDirectory, "ThirdParty")
            );
            if (!Directory.Exists(thirdPartyDirectory))
            {
                throw new BuildException(
                    "SOLOASBRIDGE_THIRDPARTY_MISSING: "
                        + thirdPartyDirectory
                );
            }
            return thirdPartyDirectory;
        }
    }
}

public sealed class SoloASBridge : ModuleRules
{
    public SoloASBridge(ReadOnlyTargetRules Target) : base(Target)
    {
                                                                                                                                             
        if (Target.LinkType == TargetLinkType.Modular && Target.Platform == UnrealTargetPlatform.Win64
            && Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 6)
        {
            RequireImportanceCopySource(Target.Version.MinorVersion);
            string Binary = Path.Combine(EngineDirectory, "Binaries", "Win64", "UnrealEditor-Engine.dll");
            ExternalDependencies.Add(Binary);
            RejectExportedImportanceCopyConstructor(Binary);
        }
        if (Target.LinkType == TargetLinkType.Modular && Target.Platform == UnrealTargetPlatform.Mac
            && Target.Type == TargetType.Editor && Target.Version.MajorVersion == 5
            && Target.Version.MinorVersion >= 6)
            RequireImportanceCopySource(Target.Version.MinorVersion);
        // SOLOAS-COMPAT-END
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
                                                                                                                                       
        string SelectedCompat = $"UE{Target.Version.MajorVersion}{Target.Version.MinorVersion}";
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private", "Compat", SelectedCompat));
        if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion == 4)
            PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public", "Compat", SelectedCompat));
        // SOLOAS-COMPAT-END
        PrecompileForTargets = PrecompileTargetsType.Any;
        UnrealBuildTool.Rules.SoloASBridgeBuildPath.ConfigureEngineProfile(this, EngineDirectory);
        bUsePrecompiled = UnrealBuildTool.Rules.SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
                                                                                                                                 
        if (Target.Version.MajorVersion > 5 || Target.Version.MinorVersion >= 6)
            PrivateDependencyModuleNames.Add("AutoRTFM");
        // SOLOAS-COMPAT-END
        PrivateDependencyModuleNames.Add("InputCore");
        // A: the project plugin-directory view uses the public Projects service.
        PrivateDependencyModuleNames.Add("Projects");
        PrivateIncludePaths.Add(
            Path.Combine(GetModuleDirectory("CoreUObject"), "Internal")
        );

        if (Target.bBuildEditor)
        {
                                                                                                             
            PrivateDependencyModuleNames.Add("UnrealEd");
                                                                                                           
            PrivateDependencyModuleNames.Add("BlueprintGraph");
            if (Target.LinkType == TargetLinkType.Modular && Target.Platform == UnrealTargetPlatform.Win64)
            {
                PrivateDependencyModuleNames.Add(UnrealBuildTool.Rules.SoloASBridgeBuildPath.EditorDetoursModule(Target));
                // SOLOAS-COMPAT-BEGIN(<5.8) Own the legacy editor backend sources without an undeclared C++ dependency module [test: SoloAS.EditorHookInstallation] [id: CS.File.SoloASBridge.Source.SoloASBridge.SoloASBridge.Build.cs]
                if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 8)
                {
                    ConditionalAddModuleDirectory(new EpicGames.Core.DirectoryReference(
                        Path.Combine(PluginDirectory, "Versioned", "LegacyDetours")));
                    PrivateIncludePaths.Add(Path.Combine(PluginDirectory, "Source"));
                    PrivateIncludePaths.Add(Path.Combine(PluginDirectory, "Source", "ThirdParty", "Detours", "src"));
                    PrivateDefinitions.Add("_CRT_STDIO_ARBITRARY_WIDE_SPECIFIERS=1");
                    bUseUnity = false;
                }
                // SOLOAS-COMPAT-END
            }
        }

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
            }
        );
    }
    private void RequireImportanceCopySource(int Minor)
    {
        string Source = Path.Combine(EngineDirectory, "Source/Runtime/Engine/Private/Kismet/ImportanceSamplingLibrary.cpp");
        ExternalDependencies.Add(Source);
        // Each installed SDK implements exactly: FImportanceTexture(const FImportanceTexture& Other) = default.
        string Expected = Minor switch
        {
            6 => "4efa6c1aa8a53591a8eb09956727211336ab69050e4a708b8eb43b43deff6119",
            7 => "4efa6c1aa8a53591a8eb09956727211336ab69050e4a708b8eb43b43deff6119",
            8 => "4efa6c1aa8a53591a8eb09956727211336ab69050e4a708b8eb43b43deff6119",
            _ => throw new BuildException("FImportanceTexture copy compatibility requires a reviewed SDK source hash.")
        };
        System.Type Sha256 = System.Type.GetType("System.Security.Cryptography.SHA256, System.Security.Cryptography.Algorithms", true);
        byte[] Hash = (byte[])Sha256.GetMethod("HashData", new[] { typeof(byte[]) })
            .Invoke(null, new object[] { System.Array.FindAll(File.ReadAllBytes(Source), Value => Value != 13) });
        if (System.BitConverter.ToString(Hash).Replace("-", "").ToLowerInvariant() != Expected)
            throw new BuildException("SOLOAS_IMPORTANCE_COPY_SOURCE_CHANGED: " + Source);
    }

    private static void RejectExportedImportanceCopyConstructor(string Binary)
    {
        byte[] Image = File.ReadAllBytes(Binary);
        int Pe = System.BitConverter.ToInt32(Image, 0x3c);
        if (System.BitConverter.ToUInt32(Image, Pe) != 0x00004550)
            throw new BuildException("Native constructor compatibility requires a valid PE image: " + Binary);
        int Optional = Pe + 24;
        int Directory = Optional + (System.BitConverter.ToUInt16(Image, Optional) == 0x20b ? 112 : 96);
        uint ExportRva = System.BitConverter.ToUInt32(Image, Directory);
        if (ExportRva == 0) return;
        int Sections = Optional + System.BitConverter.ToUInt16(Image, Pe + 20);
        int Count = System.BitConverter.ToUInt16(Image, Pe + 6);
        int Offset(uint Rva)
        {
            for (int Index = 0; Index < Count; ++Index)
            {
                int Section = Sections + Index * 40;
                uint Start = System.BitConverter.ToUInt32(Image, Section + 12);
                uint Size = System.Math.Max(System.BitConverter.ToUInt32(Image, Section + 8),
                    System.BitConverter.ToUInt32(Image, Section + 16));
                if (Rva >= Start && Rva - Start < Size)
                    return checked((int)(System.BitConverter.ToUInt32(Image, Section + 20) + Rva - Start));
            }
            throw new BuildException("Invalid native export RVA in " + Binary);
        }
        int Exports = Offset(ExportRva);
        int Names = Offset(System.BitConverter.ToUInt32(Image, Exports + 32));
        uint NameCount = System.BitConverter.ToUInt32(Image, Exports + 24);
        for (uint Index = 0; Index < NameCount; ++Index)
        {
            int Start = Offset(System.BitConverter.ToUInt32(Image, Names + checked((int)Index) * 4));
            int End = System.Array.IndexOf(Image, (byte)0, Start);
            if (End < Start) throw new BuildException("Invalid native export name in " + Binary);
            string Name = System.Text.Encoding.ASCII.GetString(Image, Start, End - Start);
            if (Name == "??0FImportanceTexture@@QEAA@AEBU0@@Z")
                throw new BuildException("Engine now exports FImportanceTexture's copy constructor; remove and requalify Bridge compatibility definition.");
        }
    }

}
