using System.IO;
using EpicGames.Core;
using UnrealBuildTool;

namespace UnrealBuildTool.Rules
{
    public static class SoloASBridgeBuildPath
    {
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

            return Target.Type == TargetType.Game && File.Exists(Path.Combine(
                Module.PluginDirectory, "Intermediate", "Build", Platform,
                Target.Architecture.ToString(), "UnrealGame", Target.Configuration.ToString(),
                Name, Name + ".precompiled"));
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
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrecompileForTargets = PrecompileTargetsType.Any;
        bUsePrecompiled = UnrealBuildTool.Rules.SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
        PrivateDependencyModuleNames.Add("AutoRTFM");
        PrivateIncludePaths.Add(
            Path.Combine(GetModuleDirectory("CoreUObject"), "Internal")
        );

        if (Target.bBuildEditor)
        {
                                                                                                             
            PrivateDependencyModuleNames.Add("UnrealEd");
                                                                                                           
            PrivateDependencyModuleNames.Add("BlueprintGraph");
            if (Target.LinkType == TargetLinkType.Modular && Target.Platform == UnrealTargetPlatform.Win64)
                PrivateDependencyModuleNames.Add("Detours");
        }

        PublicDefinitions.Add("CUVISS_HOOKS_API=SOLOASBRIDGE_API");
        PublicDefinitions.Add("CUVISS_HOOKS_NO_EXCEPTIONS=1");

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
            }
        );
    }
}
