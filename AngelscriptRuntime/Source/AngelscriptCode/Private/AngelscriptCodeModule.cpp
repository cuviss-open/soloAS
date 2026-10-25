#include "AngelscriptCodeModule.h"
#include "AngelscriptManager.h"
                                                                                                 
#include "AngelscriptCallableDispatchAdapter.h"
                                                                                               
                                                                                                          
#include "AngelscriptExecutionQuiescence.h"
                                                                                                        
                                                                                              
#include "ClassGenerator/ASClass.h"
#include "ClassGenerator/AngelscriptReplicationShadow.h"
#include "Shared/SoloASScriptBlueprint.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
                                                                                            

IMPLEMENT_MODULE(FAngelscriptCodeModule, AngelscriptCode);

TFunction<bool(class asCScriptFunction*)> FAngelscriptCodeModule::ShouldFunctionEmitReturnInfo;

void FAngelscriptCodeModule::StartupModule()
{
                                                                                            
    SoloASBridge::SetScriptReplicationListProvider([](const UClass* Class, TArray<FLifetimeProperty>& Out)
    {
        return UASClass::TryGetLifetimeScriptReplicationList(Cast<UASClass>(Class), Out);
    });
    AngelscriptReplicationShadow::Startup();
                                                                                          
                                                                                            
	RegisterAngelscriptRuntimeGcLifecycle();
                                                                                          
                                                                                             
	checkf(
		!WorldInitializedActorsHandle.IsValid(),
		TEXT("Actor binding delegate was already registered.")
	);
	WorldInitializedActorsHandle =
		FWorldDelegates::OnWorldInitializedActors.AddRaw(
			this,
			&FAngelscriptCodeModule::HandleWorldInitializedActors
		);
                                                                                           
}

void FAngelscriptCodeModule::ShutdownModule()
{
                                                                                                          
	BeginAngelscriptExecutionShutdown();
	if (FAngelscriptManager::IsInitialized())
	{
		FAngelscriptManager::Get().StopHotReloadThread();
	}
	FAngelscriptExecutionWriteScope ExecutionWriteScope;
                                                                                                        
                                                                                             
    AngelscriptReplicationShadow::Shutdown();
    SoloASBridge::SetScriptReplicationListProvider({});
                                                                                           
                                                                                              
	if (WorldInitializedActorsHandle.IsValid())
	{
		FWorldDelegates::OnWorldInitializedActors.Remove(WorldInitializedActorsHandle);
		WorldInitializedActorsHandle.Reset();
	}
	UASClass::ResetActorBindingState();
                                                                                            
                                                                                             
	ShutdownAngelscriptRuntimeGcLifecycle();
                                                                                           
                                                                                                  
	ShutdownAngelscriptCallableDispatch();
                                                                                                
                                                                                                   
	ClearAngelscriptRuntimeGeneratedFunctionTraits();
                                                                                                 
                                                                                            
	ClearAngelscriptClassScriptTypes();
                                                                                          
                                                                                                        
	(void)ExecutionWriteScope;
                                                                                                      
}

                                                                                         
void FAngelscriptCodeModule::HandleWorldInitializedActors(
	const FActorsInitializedParams& Params
)
{
	checkf(
		IsInGameThread(),
		TEXT("Actor BindComponents world scan requires the game thread.")
	);
	if (!IsInGameThread() || Params.World == nullptr)
	{
		return;
	}

	// CONTROLLED_DEGRADATION:
	// NATIVE_PRE_SUPER_BEGINPLAY_EQUIVALENCE_NOT_CLAIMED.
	for (TActorIterator<AActor> ActorIt(Params.World); ActorIt; ++ActorIt)
	{
		UASClass::EnsureBindComponentsLinked(*ActorIt);
	}
}
                                                                                       
FAngelscriptGetDynamicSpawnLevel& FAngelscriptCodeModule::GetDynamicSpawnLevel()
{
	static FAngelscriptGetDynamicSpawnLevel Delegate;
	return Delegate;
}

FAngelscriptVerifySpawnActor& FAngelscriptCodeModule::GetVerifySpawnActor()
{
	static FAngelscriptVerifySpawnActor Delegate;
	return Delegate;
}

FAngelscriptDebugCheckBreakOptions& FAngelscriptCodeModule::GetDebugCheckBreakOptions()
{
	static FAngelscriptDebugCheckBreakOptions Delegate;
	return Delegate;
}

FAngelscriptGetDebugBreakFilters& FAngelscriptCodeModule::GetDebugBreakFilters()
{
	static FAngelscriptGetDebugBreakFilters Delegate;
	return Delegate;
}

FAngelscriptDebugObjectSuffix& FAngelscriptCodeModule::GetDebugObjectSuffix()
{
	static FAngelscriptDebugObjectSuffix Delegate;
	return Delegate;
}

FAngelscriptComponentCreated& FAngelscriptCodeModule::GetComponentCreated()
{
	static FAngelscriptComponentCreated Delegate;
	return Delegate;
}

FAngelscriptCompilationDelegate& FAngelscriptCodeModule::GetPreCompile()
{
	static FAngelscriptCompilationDelegate Delegate;
	return Delegate;
}

FAngelscriptCompilationDelegate& FAngelscriptCodeModule::GetPostCompile()
{
	static FAngelscriptCompilationDelegate Delegate;
	return Delegate;
}

FAngelscriptCompilationDelegate& FAngelscriptCodeModule::GetReloadHadErrors()
{
	static FAngelscriptCompilationDelegate Delegate;
	return Delegate;
}

FAngelscriptCompilationDelegate& FAngelscriptCodeModule::GetOnInitialCompileFinished()
{
	static FAngelscriptCompilationDelegate Delegate;
	return Delegate;
}

FAngelscriptClassAnalyzeDelegate& FAngelscriptCodeModule::GetClassAnalyze()
{
	static FAngelscriptClassAnalyzeDelegate Delegate;
	return Delegate;
}

FAngelscriptPostCompileClassCollection& FAngelscriptCodeModule::GetPostCompileClassCollection()
{
	static FAngelscriptPostCompileClassCollection Delegate;
	return Delegate;
}

FAngelscriptPreGenerateClasses& FAngelscriptCodeModule::GetPreGenerateClasses()
{
	static FAngelscriptPreGenerateClasses Delegate;
	return Delegate;
}

FAngelscriptPostSetupClasses& FAngelscriptCodeModule::GetPostSetupClasses()
{
	static FAngelscriptPostSetupClasses Delegate;
	return Delegate;
}

FAngelscriptLiteralAssetCreated& FAngelscriptCodeModule::GetOnLiteralAssetCreated()
{
	static FAngelscriptLiteralAssetCreated Delegate;
	return Delegate;
}

FAngelscriptLiteralAssetCreated& FAngelscriptCodeModule::GetPostLiteralAssetSetup()
{
	static FAngelscriptLiteralAssetCreated Delegate;
	return Delegate;
}

FAngelscriptDebugListAssets& FAngelscriptCodeModule::GetDebugListAssets()
{
	static FAngelscriptDebugListAssets Delegate;
	return Delegate;
}

FAngelscriptEditorCreateBlueprint& FAngelscriptCodeModule::GetEditorCreateBlueprint()
{
	static FAngelscriptEditorCreateBlueprint Delegate;
	return Delegate;
}

FAngelscriptCompilePostLayoutTypes& FAngelscriptCodeModule::GetCompilePostLayoutTypes()
{
	static FAngelscriptCompilePostLayoutTypes Delegate;
	return Delegate;
}

FAngelscriptEditorGetCreateBlueprintDefaultAssetPath& FAngelscriptCodeModule::GetEditorGetCreateBlueprintDefaultAssetPath()
{
	static FAngelscriptEditorGetCreateBlueprintDefaultAssetPath Delegate;
	return Delegate;
}

void FAngelscriptCodeModule::InitializeAngelscript()
{
	static bool bInitialized = false;
	if (bInitialized)
		return;

	bInitialized = true;
	FModuleManager::Get().LoadModuleChecked(TEXT("AngelscriptCode"));
	FAngelscriptManager::GetOrCreate().Initialize();
}
