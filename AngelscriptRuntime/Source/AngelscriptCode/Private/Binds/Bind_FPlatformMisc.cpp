#include "AngelscriptManager.h"
#include "AngelscriptType.h"
#include "AngelscriptBinds.h"

#include "HAL/PlatformMisc.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FPlatformMisc((int32)FAngelscriptBinds::EOrder::Late, []
{
	{
		FAngelscriptBinds::FNamespace ns("FPlatformMisc");
		FAngelscriptBinds::BindGlobalFunction("void RequestExit(bool Force)", [](bool Force)
		{
			FPlatformMisc::RequestExit(Force, nullptr);
		});
		FAngelscriptBinds::BindGlobalFunction("void RequestExit(bool Force, const FString& CallSite)", [](bool Force, const FString& CallSite)
		{
			FPlatformMisc::RequestExit(Force, *CallSite);
		});
		FAngelscriptBinds::BindGlobalFunction(
			"FString GetEnvironmentVariable(const FString& VariableName) no_discard",
			[](const FString& VariableName) {
				return FPlatformMisc::GetEnvironmentVariable(*VariableName);
			}
		);
	}
});
