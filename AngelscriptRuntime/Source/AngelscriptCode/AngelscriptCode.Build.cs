using System.IO;
using UnrealBuildTool;

namespace UnrealBuildTool.Rules
{
	public class AngelscriptCode : ModuleRules
	{
		public AngelscriptCode(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
			PrecompileForTargets = PrecompileTargetsType.Any;
			bUsePrecompiled = SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
                                                                                          
			SetupIrisSupport(Target);
			PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source", "Runtime", "Net", "Iris", "Private"));
                                                                                        
                                                                                           
			string KismetSystemLibraryHeader = Path.Combine(
				EngineDirectory,
				"Source", "Runtime", "Engine", "Classes", "Kismet", "KismetSystemLibrary.h"
			);
			const string K2SetTimerFNameSignature = "K2_SetTimer(UObject* Object, FName FunctionName,";
			bool HasK2SetTimerFName = File.Exists(KismetSystemLibraryHeader) &&
				File.ReadAllText(KismetSystemLibraryHeader).IndexOf(
					K2SetTimerFNameSignature, System.StringComparison.Ordinal
				) >= 0;
			PrivateDefinitions.Add(
				"WITH_ANGELSCRIPT_K2_SETTIMER_FNAME=" + (HasK2SetTimerFName ? "1" : "0")
			);
                                                                                         
                                                                                        
			if (Target.ProjectFile != null)
			{
				string ScriptDirectory = Path.Combine(
					Target.ProjectFile.Directory.FullName, "Script"
				);
				string[] ScriptCaches = new[]
				{
					"Binds.Cache",
					"Binds.Cache.Headers",
					"Binds.Cache.cuviss-cache-v2",
					"PrecompiledScript.Cache",
					"PrecompiledScript.Cache.cuviss-cache-v2"
				};
				bool HasCompleteScriptCache = true;
				int CompanionCount = 0;
				foreach (string Cache in ScriptCaches)
				{
					FileInfo CacheFile = new FileInfo(
						Path.Combine(ScriptDirectory, Cache)
					);
					bool IsCompanion = Cache.EndsWith(".cuviss-cache-v2");
					if (IsCompanion && CacheFile.Exists)
					{
						++CompanionCount;
					}
					if ((!CacheFile.Exists && !IsCompanion)
						|| (CacheFile.Exists && CacheFile.Length <= 0))
					{
						HasCompleteScriptCache = false;
						break;
					}
				}
				HasCompleteScriptCache &= CompanionCount == 0 || CompanionCount == 2;
				bool IsShippingRuntime =
					Target.Configuration == UnrealTargetConfiguration.Shipping
					&& (Target.Type == TargetType.Game
						|| Target.Type == TargetType.Client
						|| Target.Type == TargetType.Server);

				if (IsShippingRuntime && HasCompleteScriptCache)
				{
					foreach (string Cache in ScriptCaches)
					{
						if (File.Exists(Path.Combine(ScriptDirectory, Cache)))
						{
							RuntimeDependencies.Add(
								"$(ProjectDir)/Script/" + Cache,
								StagedFileType.UFS
							);
						}
					}
				}
				else
				{
					RuntimeDependencies.Add(
						"$(ProjectDir)/Script/...",
						StagedFileType.UFS
					);
				}
			}
                                                                                      
                                                                                                        
				ForceIncludeFiles.Add(Path.Combine(
					ModuleDirectory, "Public", "AngelscriptReflectionAdapter.h"
				));
                                                                                                      
                                                                                                       
				ForceIncludeFiles.Add(Path.Combine(
					ModuleDirectory, "Public", "AngelscriptCallableDispatchAdapter.h"
				));
				ForceIncludeFiles.Add("Runtime/SoloASBridgeCallableDispatch.h");
                                                                                                     
                                                                                                 
				ForceIncludeFiles.Add(Path.Combine(
					ModuleDirectory, "Public", "AngelscriptExecutionQuiescence.h"
				));
				ForceIncludeFiles.Add(Path.Combine(
					ModuleDirectory, "Public", "ClassGenerator", "AngelscriptClassState.h"
				));
                                                                                               
                                                                                                  
				ForceIncludeFiles.Add(Path.Combine(
					ModuleDirectory, "Public", "AngelscriptRuntimeGcLifecycle.h"
				));
				ForceIncludeFiles.Add("Runtime/SoloASBridgeObjectLifecycle.h");
				ForceIncludeFiles.Add("UObject/CoreNet.h");
				ForceIncludeFiles.Add("UObject/ObjectKey.h");
				ForceIncludeFiles.Add("HAL/CriticalSection.h");
				ForceIncludeFiles.Add("Misc/ScopeLock.h");
                                                                                                
                                                                                                    
			PublicDependencyModuleNames.Add("SoloASBridge");
			PublicDependencyModuleNames.Add("StructUtils");
                                                                                                  

			/* Link to libraries used in core angelscript code */
			PublicDependencyModuleNames.AddRange(new string[]
			{
				"ApplicationCore",
				"Core",
				"CoreUObject",
				"Engine",
				"EngineSettings",
				"DeveloperSettings",
				"Json",
				"JsonUtilities",
				"GameplayTags",
			});

			/* Link to libraries used in bindings */
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"AIModule",
				"NavigationSystem",
				"NetCore",
				"Landscape",
				"Networking",
				"Sockets",
				"InputCore",
				"SlateCore",
				"Slate",
				"UMG",
				"TraceLog",
				"AssetRegistry",
				"Projects",
				"PhysicsCore",
				"CoreOnline",
			});

			if (Target.bBuildEditor)
			{
				PublicDependencyModuleNames.AddRange(new string[] 
				{
					"UnrealEd",
					"EditorSubsystem",
				});

				PrivateDependencyModuleNames.AddRange(new string[]
				{
					"UMGEditor",
				});
			}

			/* Link to Angelscript */
			PublicIncludePaths.Add(PluginDirectory + "/ThirdParty/include");
			PublicIncludePaths.Add(PluginDirectory + "/ThirdParty/source");
		}
	}
}
