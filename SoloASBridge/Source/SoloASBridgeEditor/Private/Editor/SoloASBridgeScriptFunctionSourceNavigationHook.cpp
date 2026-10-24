// Copyright Cuviss Studio. All Rights Reserved.

#include "Editor/SoloASBridgeScriptFunctionSourceNavigationHook.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)

#include "BlueprintEditorSettings.h"
#include "Editor/UnrealEdEngine.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "Editor/EditorEngine.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Preferences/UnrealEdOptions.h"
#include "ScopedTransaction.h"
#include "SourceCodeNavigation.h"
#include "UObject/UnrealType.h"
#include "UnrealEdGlobals.h"
#include "Widgets/Notifications/SNotificationList.h"

#include "Editor/SoloASBridgeEditorInlineHook.h"
#include "Editor/SoloASEditorObjectImportProperties.h"
#include "Shared/SoloASScriptBlueprint.h"

#if PLATFORM_MAC
#include <dlfcn.h>
#endif

#define LOCTEXT_NAMESPACE "SoloASBridgeScriptFunctionSourceNavigationHook"

namespace SoloASBridge::Editor
{
namespace
{
using FCanJumpToDefinition = bool (*)(const UK2Node_CallFunction*);
using FJumpToDefinition = void (*)(const UK2Node_CallFunction*);

FEditorInlineHookHandle GCanJumpHook;
FEditorInlineHookHandle GJumpHook;
void* GBlueprintGraphHandle = nullptr;
TAutoConsoleVariable<int32> CVarEditorScriptFunctionSourceNavigation(
	TEXT("soloas.EditorScriptFunctionSourceNavigation"), 1,
	TEXT("Enable AngelScript source navigation from Blueprint call-function nodes on Stock UE."),
	ECVF_Default);
FCanJumpToDefinition OriginalCanJumpToDefinition()
{
	return reinterpret_cast<FCanJumpToDefinition>(GCanJumpHook.Original);
}

FJumpToDefinition OriginalJumpToDefinition()
{
	return reinterpret_cast<FJumpToDefinition>(GJumpHook.Original);
}

void ShowNativeNavigationDisabledNotification()
{
	FNotificationInfo Info(NSLOCTEXT(
		"K2Node_CallFunction", "NavigateToNativeDisabled",
		"Navigation to Native (c++) Functions Disabled"));
	Info.ExpireDuration = 10.0f;
	Info.CheckBoxState = GetDefault<UBlueprintEditorSettings>()->bNavigateToNativeFunctionsFromCallNodes
		? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
	Info.CheckBoxStateChanged = FOnCheckStateChanged::CreateStatic([](ECheckBoxState NewState)
	{
		const FScopedTransaction Transaction(NSLOCTEXT(
			"K2Node_CallFunction", "ChangeNavigateToNativeFunctionsFromCallNodes",
			"Change Navigate To Native Functions From Call Nodes Setting"));
		UBlueprintEditorSettings* Settings = GetMutableDefault<UBlueprintEditorSettings>();
		Settings->Modify();
		Settings->bNavigateToNativeFunctionsFromCallNodes = NewState == ECheckBoxState::Checked;
		Settings->SaveConfig();
	});
	Info.CheckBoxText = NSLOCTEXT(
		"K2Node_CallFunction", "EnableNavigationToNative",
		"Navigate to Native (c++) Functions from Blueprint Call Nodes?");
	FSlateNotificationManager::Get().AddNotification(Info);
}

bool CanJumpToDefinitionHook(const UK2Node_CallFunction* Node)
{
	const FCanJumpToDefinition Original = OriginalCanJumpToDefinition();
	const void* Caller = GetEditorHookCallerAddress();
	if (!GCanJumpHook.IsCallerInTargetModule(Caller)
#if WITH_DEV_AUTOMATION_TESTS
		&& !SoloASBridge::Editor::Tests::IsCallerInRegisteredModule(
			SoloASBridge::Editor::Tests::ECallerRange::ScriptFunctionSourceNavigation, Caller)
#endif
		)
		return Original != nullptr ? Original(Node) : false;
	if (Original == nullptr)
		return false;

	const UFunction* Function = Node != nullptr ? Node->GetTargetFunction() : nullptr;
	if (CVarEditorScriptFunctionSourceNavigation.GetValueOnAnyThread() != 0
		&& Function != nullptr
		&& SoloASBridge::IsRegisteredScriptClass(Function->GetOwnerClass())
		&& GUnrealEd != nullptr && GUnrealEd->GetUnrealEdOptions()->IsCPPAllowed())
	{
		return true;
	}
	return Original(Node);
}

void JumpToDefinitionHook(const UK2Node_CallFunction* Node)
{
	const FJumpToDefinition Original = OriginalJumpToDefinition();
	const void* Caller = GetEditorHookCallerAddress();
	if (!GJumpHook.IsCallerInTargetModule(Caller)
#if WITH_DEV_AUTOMATION_TESTS
		&& !SoloASBridge::Editor::Tests::IsCallerInRegisteredModule(
			SoloASBridge::Editor::Tests::ECallerRange::ScriptFunctionSourceNavigation, Caller)
#endif
		)
	{
		if (Original != nullptr)
			Original(Node);
		return;
	}
	const UFunction* Function = Node != nullptr ? Node->GetTargetFunction() : nullptr;
	if (CVarEditorScriptFunctionSourceNavigation.GetValueOnAnyThread() != 0
		&& Function != nullptr
		&& SoloASBridge::IsRegisteredScriptClass(Function->GetOwnerClass())
		&& GUnrealEd != nullptr && GUnrealEd->GetUnrealEdOptions()->IsCPPAllowed())
	{
		if (!GetDefault<UBlueprintEditorSettings>()->bNavigateToNativeFunctionsFromCallNodes)
		{
			ShowNativeNavigationDisabledNotification();
			return;
		}

		if (FSourceCodeNavigation::CanNavigateToFunction(Function)
			&& FSourceCodeNavigation::NavigateToFunction(Function))
		{
			return;
		}

		FString HeaderPath;
		if (FSourceCodeNavigation::FindClassHeaderPath(Function, HeaderPath)
			&& IFileManager::Get().FileSize(*HeaderPath) != INDEX_NONE)
		{
			FSourceCodeNavigation::OpenSourceFileAsync(
				FPaths::ConvertRelativePathToFull(HeaderPath));
		}
		return;
	}

	if (Original != nullptr)
		Original(Node);
}

#if PLATFORM_MAC
bool IsBlueprintGraphImage(void* Address)
{
	Dl_info Info{};
	return Address != nullptr && dladdr(Address, &Info) != 0
		&& Info.dli_fname != nullptr
		&& FCStringAnsi::Strstr(Info.dli_fname, "UnrealEditor-BlueprintGraph") != nullptr;
}
#endif

void* FindBlueprintGraphSymbol(const TCHAR* WindowsName, const char* MacName)
{
#if PLATFORM_WINDOWS
	return GBlueprintGraphHandle != nullptr
		? FPlatformProcess::GetDllExport(GBlueprintGraphHandle, WindowsName) : nullptr;
#elif PLATFORM_MAC
	void* Address = dlsym(RTLD_DEFAULT, MacName);
	return IsBlueprintGraphImage(Address) ? Address : nullptr;
#endif
}
}

void InitializeScriptFunctionSourceNavigationHook()
{
	if (!GIsEditor || IsRunningCommandlet() || GCanJumpHook.Original != nullptr)
		return;
	if (FModuleManager::Get().LoadModule(TEXT("BlueprintGraph")) == nullptr)
		return;

#if PLATFORM_WINDOWS
	GBlueprintGraphHandle = FPlatformProcess::GetDllHandle(TEXT("UnrealEditor-BlueprintGraph.dll"));
#elif PLATFORM_MAC
	GBlueprintGraphHandle = FPlatformProcess::GetDllHandle(TEXT("libUnrealEditor-BlueprintGraph.dylib"));
#endif
	void* CanJump = FindBlueprintGraphSymbol(
		TEXT("?CanJumpToDefinition@UK2Node_CallFunction@@UEBA_NXZ"),
		"_ZNK20UK2Node_CallFunction19CanJumpToDefinitionEv");
	void* Jump = FindBlueprintGraphSymbol(
		TEXT("?JumpToDefinition@UK2Node_CallFunction@@UEBAXXZ"),
		"_ZNK20UK2Node_CallFunction16JumpToDefinitionEv");
	if (CanJump == nullptr || Jump == nullptr
		|| !InstallEditorInlineHook(GCanJumpHook, CanJump,
			reinterpret_cast<void*>(&CanJumpToDefinitionHook), TEXT("UK2Node_CallFunction::CanJumpToDefinition")))
	{
		UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Blueprint function source-navigation hooks are unavailable; call-node navigation keeps Stock behavior."));
		if (GBlueprintGraphHandle != nullptr)
			FPlatformProcess::FreeDllHandle(GBlueprintGraphHandle);
		GBlueprintGraphHandle = nullptr;
		return;
	}
	if (!InstallEditorInlineHook(GJumpHook, Jump,
		reinterpret_cast<void*>(&JumpToDefinitionHook), TEXT("UK2Node_CallFunction::JumpToDefinition")))
	{
		RemoveEditorInlineHook(GCanJumpHook,
			reinterpret_cast<void*>(&CanJumpToDefinitionHook), TEXT("UK2Node_CallFunction::CanJumpToDefinition"));
		UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Blueprint function source-navigation hooks are unavailable; call-node navigation keeps Stock behavior."));
		FPlatformProcess::FreeDllHandle(GBlueprintGraphHandle);
		GBlueprintGraphHandle = nullptr;
	}
}

void ShutdownScriptFunctionSourceNavigationHook()
{
	const bool bJumpRestored = RemoveEditorInlineHook(GJumpHook,
		reinterpret_cast<void*>(&JumpToDefinitionHook), TEXT("UK2Node_CallFunction::JumpToDefinition"));
	const bool bCanJumpRestored = RemoveEditorInlineHook(GCanJumpHook,
		reinterpret_cast<void*>(&CanJumpToDefinitionHook), TEXT("UK2Node_CallFunction::CanJumpToDefinition"));
	if (bJumpRestored && bCanJumpRestored && GBlueprintGraphHandle != nullptr)
		FPlatformProcess::FreeDllHandle(GBlueprintGraphHandle);
	if (bJumpRestored && bCanJumpRestored)
	{
		GBlueprintGraphHandle = nullptr;
	}
}
}

#undef LOCTEXT_NAMESPACE

#endif
