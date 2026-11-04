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
                                                                                                   

                                                                                                                     
            PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("CoreUObject"), "Internal"));
                                                                                                                   

                                                                                                             
        bool WithStateTree = UnrealBuildTool.Rules.SoloASBridgeBuildPath.IsStateTreeEnabled(Target, EngineDirectory);
        PrivateDefinitions.Add("SOLOAS_WITH_STATETREE=" + (WithStateTree ? "1" : "0"));
        if (WithStateTree) PrivateDependencyModuleNames.Add("StateTreeEditorModule");

                                                                                                           
			PublicDependencyModuleNames.AddRange(new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
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
				"ToolWidgets",
				"PlacementMode",
				"StructViewer",
                                                                                                           
				"AIGraph",
				"AIModule",
				"BehaviorTreeEditor",
                                                                                                         
                                                                                                           
				"CurveAssetEditor",
                                                                                                         
                                                                                                                   
				"SoloASBridgeEditor",
                "StructUtils",
                                                                                                                 
			});

                                                                                                         
			if (Target.bBuildEditor)
				PrivateDependencyModuleNames.Add("Persona");
                                                                                                       

                                                                                                                
			if (Target.bBuildEditor && Target.LinkType == TargetLinkType.Modular
				&& Target.Platform == UnrealTargetPlatform.Win64)
				// SOLOAS-COMPAT-BEGIN(<5.8) UE 5.7 requires the Bridge-owned Detours source module [test: SoloAS.EditorHookInstallation]
				PrivateDependencyModuleNames.Add(SoloASBridgeBuildPath.EditorDetoursModule(Target));
// SOLOAS-COMPAT-END
                                                                                                              
		}
	}
}
