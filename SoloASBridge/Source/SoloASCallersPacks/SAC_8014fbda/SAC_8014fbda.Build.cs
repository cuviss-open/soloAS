using UnrealBuildTool;

public sealed class SAC_8014fbda : SoloASNativeCallerRules
{
                                                                                                                                       
    public SAC_8014fbda(ReadOnlyTargetRules Target)
        : base(Target, "ChaosMover", "ChaosMover", "Plugins/Experimental/ChaosMover/Source/ChaosMover", "Runtime") {
        if (Target.Version.MinorVersion == 6)
            PrivateIncludePaths.Add(System.IO.Path.Combine(GetModuleDirectory("ChaosMover"), "Private"));
    }
    // SOLOAS-COMPAT-END
}
