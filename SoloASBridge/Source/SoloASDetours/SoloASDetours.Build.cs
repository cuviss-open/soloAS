// SPDX-License-Identifier: MPL-2.0
using System.IO;
using UnrealBuildTool;

public class SoloASDetours : ModuleRules
{
    public SoloASDetours(ReadOnlyTargetRules Target) : base(Target)
    {
        Type = ModuleType.External;
                                                                                                                                              
        if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 8
            && Target.bBuildEditor && Target.LinkType == TargetLinkType.Modular
            && Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemIncludePaths.Add(Path.Combine(PluginDirectory, "Source", "ThirdParty", "Detours"));
        }
        // SOLOAS-COMPAT-END
    }
}
