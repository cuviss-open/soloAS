using System.IO;
using UnrealBuildTool;

namespace UnrealBuildTool.Rules
{
	public class AngelscriptEditor : ModuleRules
	{
		public AngelscriptEditor(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
			PrecompileForTargets = PrecompileTargetsType.Any;
			bUsePrecompiled = SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
                                                                                                               
            if (Target.bBuildEditor && Target.LinkType != TargetLinkType.Monolithic
                && (Target.Platform == UnrealTargetPlatform.Win64 || Target.Platform == UnrealTargetPlatform.Mac))
            {
                PrivateDependencyModuleNames.AddRange(new[] { "PythonScriptPlugin", "Python3" });
                PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("PythonScriptPlugin"), "Private"));
            }
                                                                                                             

                                                                                                                     
            PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("CoreUObject"), "Internal"));
                                                                                                                   

			PublicDependencyModuleNames.AddRange(new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
                                                                                                             
				"StateTreeEditorModule",
                                                                                                           
				"UnrealEd",
				"EditorSubsystem",
				"AngelscriptCode",
                                                                                                                     
                "SoloASBridge",
                                                                                                                   
				"BlueprintGraph",
				"Kismet",
				"DirectoryWatcher",
				"Slate",
				"SlateCore",
				"AssetTools",
			});

                                                                                                       
			PrivateDependencyModuleNames.Add("Foliage");
                                                                                                     
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"TypedElementFramework",
				"TypedElementRuntime",
                                                                                                        
				"BSPUtils",
                                                                                                      
				"Projects",
				"GameplayTags",
				"Settings",
				"LevelEditor",
				"PropertyEditor",
				"ContentBrowser",
				"ContentBrowserData",
				"ToolMenus",
                                                                                                       
                "OutputLog",
                                                                                                     
				"ToolWidgets",
                                                                                           
				"UMG",
                                                                                         
				"PlacementMode",
				"StructViewer",
                                                                                                           
				"AIGraph",
				"AIModule",
				"BehaviorTreeEditor",
                                                                                                         
                                                                                                           
				"CurveAssetEditor",
                                                                                                         
                                                                                                                   
				"SoloASBridgeEditor",
                                                                                                                 
			});

                                                                                                  
			if (Target.bBuildEditor)
				PrivateDependencyModuleNames.Add("Persona");
                                                                                                

                                                                                                                
			if (Target.bBuildEditor && Target.LinkType == TargetLinkType.Modular
				&& Target.Platform == UnrealTargetPlatform.Win64)
				PrivateDependencyModuleNames.Add("Detours");
                                                                                                              
		}
	}
}
