using UnrealBuildTool;

public sealed class SoloASDeveloperToolsToolchainEditor : ModuleRules
{
    public SoloASDeveloperToolsToolchainEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "DeveloperSettings", "DesktopPlatform",
            "UnrealEd", "ToolMenus", "Slate", "SlateCore", "Settings", "Json", "XmlParser", "Projects"
        });
    }
}
