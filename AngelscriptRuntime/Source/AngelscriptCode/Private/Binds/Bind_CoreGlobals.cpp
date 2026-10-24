#include "AngelscriptBinds.h"

#include "CoreGlobals.h"
#include "UObject/Class.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_CoreGlobals((int32)FAngelscriptBinds::EOrder::Late, []
{
	FAngelscriptBinds::BindGlobalFunction("bool IsRunningCommandlet() no_discard", FUNC_TRIVIAL(IsRunningCommandlet));
	FAngelscriptBinds::BindGlobalFunction("bool IsRunningCookCommandlet() no_discard", FUNC_TRIVIAL(IsRunningCookCommandlet));
	FAngelscriptBinds::BindGlobalFunction("bool IsRunningDLCCookCommandlet() no_discard", FUNC_TRIVIAL(IsRunningDLCCookCommandlet));
	FAngelscriptBinds::BindGlobalFunction("UClass GetRunningCommandletClass() no_discard", FUNC_TRIVIAL(GetRunningCommandletClass));
});
