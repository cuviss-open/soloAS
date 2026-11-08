using UnrealBuildTool;

public class SoloASDeveloperToolsProviders : ModuleRules
{
    public SoloASDeveloperToolsProviders(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.Add("Core");
        PrivateDependencyModuleNames.Add("Json");
    }
}
