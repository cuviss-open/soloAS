#include "AngelscriptBinds.h"

#include "CoreGlobals.h"
#include "UObject/Class.h"
                                                                                                   
#include "Compat/SoloASRuntimeCompat.h"
#include "AngelscriptDocs.h"
#if WITH_EDITOR
#include "PluginBlueprintLibrary.h"
#endif
                                                                                                 

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_CoreGlobals((int32)FAngelscriptBinds::EOrder::Late, []
{
	FAngelscriptBinds::BindGlobalFunction("bool IsRunningCommandlet() no_discard", FUNC_TRIVIAL(IsRunningCommandlet));
	FAngelscriptBinds::BindGlobalFunction("bool IsRunningCookCommandlet() no_discard", FUNC_TRIVIAL(IsRunningCookCommandlet));
	FAngelscriptBinds::BindGlobalFunction("bool IsRunningDLCCookCommandlet() no_discard", FUNC_TRIVIAL(IsRunningDLCCookCommandlet));
	FAngelscriptBinds::BindGlobalFunction("UClass GetRunningCommandletClass() no_discard", FUNC_TRIVIAL(GetRunningCommandletClass));
});

                                                                                                
// A: retain a real engine-owned reference when the MinimalAPI entry is not exported.
AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_PluginProjectSearchPaths(
    (int32)FAngelscriptBinds::EOrder::Late + 101, []
{
    FAngelscriptBinds::FNamespace Namespace("Plugin");
    if (Namespace.HasFunction("GetAdditionalProjectPluginSearchPaths"))
        return;
    FAngelscriptBinds::BindGlobalFunction("const TArray<FString>& GetAdditionalProjectPluginSearchPaths()",
        FUNC_TRIVIAL(SoloASBridge::Compat::GetAdditionalProjectPluginSearchPaths));
#if WITH_EDITOR
    if (UFunction* Function = UPluginBlueprintLibrary::StaticClass()->FindFunctionByName(
        FName(TEXT("GetAdditionalProjectPluginSearchPaths"))))
    {
        FAngelscriptDocs::AddUnrealDocumentation(FAngelscriptBinds::GetPreviousFunctionId(),
            Function->GetMetaData(TEXT("ToolTip")), Function->GetMetaData(TEXT("Category")), Function);
    }
#endif
});
                                                                                              
