#include "AngelscriptManager.h"
#include "AngelscriptType.h"
#include "AngelscriptBinds.h"

#include "HAL/PlatformProcess.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FPlatformProcess((int32)FAngelscriptBinds::EOrder::Late, []
{
	{
		FAngelscriptBinds::FNamespace ns("FPlatformProcess");

		// Paths
		FAngelscriptBinds::BindGlobalFunction("FString UserDir() no_discard", []() { return FString(FPlatformProcess::UserDir()); });
		FAngelscriptBinds::BindGlobalFunction("FString UserSettingsDir() no_discard", []() { return FString(FPlatformProcess::UserSettingsDir()); });
		FAngelscriptBinds::BindGlobalFunction("FString UserTempDir() no_discard", []() { return FString(FPlatformProcess::UserTempDir()); });
		FAngelscriptBinds::BindGlobalFunction("FString ApplicationSettingsDir() no_discard", []() { return FString(FPlatformProcess::ApplicationSettingsDir()); });
		FAngelscriptBinds::BindGlobalFunction("FString ExecutablePath() no_discard", []() { return FString(FPlatformProcess::ExecutablePath()); });
		FAngelscriptBinds::BindGlobalFunction("FString ExecutableName() no_discard", []() { return FString(FPlatformProcess::ExecutableName()); });
		FAngelscriptBinds::BindGlobalFunction("FString CurrentWorkingDirectory() no_discard", []() { return FPlatformProcess::GetCurrentWorkingDirectory(); });

		// URLs
		FAngelscriptBinds::BindGlobalFunction("void LaunchURL(const FString& URL, const FString& Params = FString())", [](const FString& URL, const FString& Params) { FPlatformProcess::LaunchURL(*URL, *Params, nullptr); });
		FAngelscriptBinds::BindGlobalFunction("void LaunchURL(const FString& URL, const FString& Params, FString& OutError)", [](const FString& URL, const FString& Params, FString& Error) { FPlatformProcess::LaunchURL(*URL, *Params, &Error); });
		FAngelscriptBinds::BindGlobalFunction("bool CanLaunchURL(const FString& URL) no_discard", [](const FString& URL) { return FPlatformProcess::CanLaunchURL(*URL); });

		// Identity
		FAngelscriptBinds::BindGlobalFunction("FString ComputerName() no_discard", []() { return FString(FPlatformProcess::ComputerName()); });
		FAngelscriptBinds::BindGlobalFunction("FString UserName() no_discard", []() { return FString(FPlatformProcess::UserName()); });
		FAngelscriptBinds::BindGlobalFunction("FString GameBundleId() no_discard", []() { return FPlatformProcess::GetGameBundleId(); });
	}
});
