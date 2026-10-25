#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

typedef TArray<FName> FAngelscriptDebugBreakOptions;
typedef TMap<FName, FString> FAngelscriptDebugBreakFilters;
DECLARE_DELEGATE_RetVal(class ULevel*, FAngelscriptGetDynamicSpawnLevel);
DECLARE_DELEGATE_RetVal_TwoParams(bool, FAngelscriptVerifySpawnActor, struct FActorSpawnParameters&, bool);
DECLARE_DELEGATE_RetVal_TwoParams(bool, FAngelscriptDebugCheckBreakOptions, const FAngelscriptDebugBreakOptions&, UObject*);
DECLARE_DELEGATE_OneParam(FAngelscriptGetDebugBreakFilters, FAngelscriptDebugBreakFilters&);
DECLARE_DELEGATE_TwoParams(FAngelscriptDebugObjectSuffix, UObject*, FString&);
DECLARE_DELEGATE_OneParam(FAngelscriptComponentCreated, class UActorComponent*);
DECLARE_DELEGATE_ThreeParams(FAngelscriptClassAnalyzeDelegate, FString&, TSharedPtr<struct FAngelscriptClassDesc>, bool&);
DECLARE_MULTICAST_DELEGATE_OneParam(FAngelscriptPostCompileClassCollection, const TArray<TSharedRef<struct FAngelscriptModuleDesc>>&);
DECLARE_MULTICAST_DELEGATE_OneParam(FAngelscriptPreGenerateClasses, const TArray<TSharedRef<struct FAngelscriptModuleDesc>>&);
DECLARE_MULTICAST_DELEGATE_OneParam(FAngelscriptPostSetupClasses, const TArray<TSharedRef<struct FAngelscriptModuleDesc>>&);
DECLARE_MULTICAST_DELEGATE_OneParam(FAngelscriptCompilePostLayoutTypes, const TArray<TSharedRef<struct FAngelscriptModuleDesc>>&);
DECLARE_MULTICAST_DELEGATE(FAngelscriptCompilationDelegate);
DECLARE_MULTICAST_DELEGATE_TwoParams(FAngelscriptLiteralAssetCreated, UObject*, const FString&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FAngelscriptDebugListAssets, TArray<FString>, class UASClass*);
DECLARE_MULTICAST_DELEGATE_OneParam(FAngelscriptEditorCreateBlueprint, class UASClass*);
DECLARE_DELEGATE_RetVal_OneParam(FString, FAngelscriptEditorGetCreateBlueprintDefaultAssetPath, class UASClass*);

                                                                                                       
class FAngelscriptCodeModule : public FDefaultModuleImpl
                                                                                                     
{
public:
                                                                                                                
	virtual ANGELSCRIPTCODE_API void StartupModule() override;

	virtual ANGELSCRIPTCODE_API void ShutdownModule() override;
                                                                                                              
                                                                                                   

	virtual bool SupportsDynamicReloading() override { return false; }
                                                                                                 

	static TFunction<bool(class asCScriptFunction*)> ShouldFunctionEmitReturnInfo;
                                                                                                                    
	static ANGELSCRIPTCODE_API FAngelscriptGetDynamicSpawnLevel& GetDynamicSpawnLevel();
                                                                                                                  
	static FAngelscriptVerifySpawnActor& GetVerifySpawnActor();
	static FAngelscriptDebugCheckBreakOptions& GetDebugCheckBreakOptions();
	static FAngelscriptGetDebugBreakFilters& GetDebugBreakFilters();
	static FAngelscriptDebugObjectSuffix& GetDebugObjectSuffix();
	static FAngelscriptComponentCreated& GetComponentCreated();
                                                                                                                  
    /**
     * Compilation multicast delegates use UE's synchronous Broadcast directly.
     * On the supported UE 5.8 implementation, an unchanged invocation list is
     * visited in reverse registration order (last added first). Removal and
     * compaction may reorder entries; do not depend on historical registration
     * order after mutation. Callbacks added during Broadcast wait until the next
     * broadcast. This applies to PreCompile, PostCompile, ReloadHadErrors,
     * OnInitialCompileFinished and PostCompileClassCollection.
     *
     * PreCompile runs before compilation. PostCompile runs after a successful
     * module swap; ReloadHadErrors reports failure instead. Class collection is
     * announced by the outer compile flow after CompileModules returns success.
     * OnInitialCompileFinished runs once initial setup has completed.
     */
                                                                                                                
                                                                                                             
	static ANGELSCRIPTCODE_API FAngelscriptCompilationDelegate& GetPreCompile();
                                                                                                           
                                                                                                              
	static ANGELSCRIPTCODE_API FAngelscriptCompilationDelegate& GetPostCompile();
                                                                                                            
                                                                                                                  
	static ANGELSCRIPTCODE_API FAngelscriptCompilationDelegate& GetReloadHadErrors();
                                                                                                                
                                                                                                                           
	static ANGELSCRIPTCODE_API FAngelscriptCompilationDelegate& GetOnInitialCompileFinished();
                                                                                                                         
	static FAngelscriptClassAnalyzeDelegate& GetClassAnalyze();
	static FAngelscriptPreGenerateClasses& GetPreGenerateClasses();
	static FAngelscriptPostSetupClasses& GetPostSetupClasses();
                                                                                                                             
	static ANGELSCRIPTCODE_API FAngelscriptPostCompileClassCollection& GetPostCompileClassCollection();
                                                                                                                           
	static FAngelscriptLiteralAssetCreated& GetOnLiteralAssetCreated();
	static FAngelscriptLiteralAssetCreated& GetPostLiteralAssetSetup();
	static FAngelscriptCompilePostLayoutTypes& GetCompilePostLayoutTypes();
                                                                                                                  
	static ANGELSCRIPTCODE_API FAngelscriptDebugListAssets& GetDebugListAssets();
                                                                                                                
                                                                                                                        
	static ANGELSCRIPTCODE_API FAngelscriptEditorCreateBlueprint& GetEditorCreateBlueprint();
                                                                                                                      
                                                                                                                                           
	static ANGELSCRIPTCODE_API FAngelscriptEditorGetCreateBlueprintDefaultAssetPath& GetEditorGetCreateBlueprintDefaultAssetPath();
                                                                                                                                         

                                                                                                                     
	static ANGELSCRIPTCODE_API void InitializeAngelscript();
                                                                                                                   

private:

                                                                                           
	void HandleWorldInitializedActors(
		const struct FActorsInitializedParams& Params
	);

	FDelegateHandle WorldInitializedActorsHandle;
                                                                                         
};
