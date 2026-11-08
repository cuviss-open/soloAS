using UnrealBuildTool;

public class SoloASDeveloperToolsEditor : ModuleRules
{
    public SoloASDeveloperToolsEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.Add("Core");
        PrivateDependencyModuleNames.AddRange(new[] {
            "CoreUObject", "Engine", "UnrealEd", "ApplicationCore", "ToolMenus", "Slate", "SlateCore", "InputCore",
            "Json", "Projects", "DeveloperSettings", "Settings", "AngelscriptCode", "SoloASDeveloperToolsProviders"
        });
    }
}
