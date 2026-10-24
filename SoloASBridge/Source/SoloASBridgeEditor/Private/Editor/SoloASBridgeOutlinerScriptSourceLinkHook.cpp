// Copyright Cuviss Studio. All Rights Reserved.

#include "Editor/SoloASBridgeOutlinerScriptSourceLinkHook.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)

#include "EditorClassUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "SourceCodeNavigation.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "Shared/SoloASScriptBlueprint.h"

#include "Editor/SoloASBridgeEditorInlineHook.h"
#include "Editor/SoloASEditorObjectImportProperties.h"

#if PLATFORM_MAC
#include <dlfcn.h>
#endif

namespace SoloASBridge::Editor
{
namespace
{
using FGetClassHyperlink = TSharedPtr<SWidget> (*)(UObject*);

FEditorInlineHookHandle GClassHyperlinkHook;
void* GSceneOutlinerHandle = nullptr;
TAutoConsoleVariable<int32> CVarOutlinerScriptSourceLink(
	TEXT("soloas.EditorOutlinerScriptSourceLink"), 1,
	TEXT("Show and open source links for AngelScript classes in the Scene Outliner on Stock UE."),
	ECVF_Default);
FGetClassHyperlink OriginalGetClassHyperlink()
{
	return reinterpret_cast<FGetClassHyperlink>(GClassHyperlinkHook.Original);
}

TSharedPtr<SWidget> GetClassHyperlinkHook(UObject* Object)
{
	const FGetClassHyperlink Original = OriginalGetClassHyperlink();
	const void* Caller = GetEditorHookCallerAddress();
	if (!GClassHyperlinkHook.IsCallerInTargetModule(Caller)
#if WITH_DEV_AUTOMATION_TESTS
		&& !SoloASBridge::Editor::Tests::IsCallerInRegisteredModule(
			SoloASBridge::Editor::Tests::ECallerRange::OutlinerScriptSourceLink, Caller)
#endif
		)
		return Original != nullptr ? Original(Object) : nullptr;
	if (CVarOutlinerScriptSourceLink.GetValueOnAnyThread() == 0
		|| Object == nullptr || Object->GetClass() == nullptr
		|| !SoloASBridge::IsRegisteredScriptClass(Object->GetClass()))
	{
		return Original != nullptr ? Original(Object) : nullptr;
	}

	const UClass* Class = Object->GetClass();
	if (Class->HasMetaData(TEXT("NoSourceLink")))
		return nullptr;

	FEditorClassUtils::FSourceLinkParams Params;
	Params.Object = Object;
	Params.bUseDefaultFormat = true;
	return FEditorClassUtils::GetSourceLink(Class, Params);
}

void* FindSceneOutlinerSymbol(const TCHAR* WindowsName, const char* MacName)
{
#if PLATFORM_WINDOWS
	return GSceneOutlinerHandle != nullptr
		? FPlatformProcess::GetDllExport(GSceneOutlinerHandle, WindowsName) : nullptr;
#elif PLATFORM_MAC
	void* Address = dlsym(RTLD_DEFAULT, MacName);
	Dl_info Info{};
	return Address != nullptr && dladdr(Address, &Info) != 0
		&& Info.dli_fname != nullptr
		&& FCStringAnsi::Strstr(Info.dli_fname, "UnrealEditor-SceneOutliner") != nullptr
		? Address : nullptr;
#endif
}
}

void InitializeOutlinerScriptSourceLinkHook()
{
	if (!GIsEditor || IsRunningCommandlet() || GClassHyperlinkHook.Original != nullptr)
		return;
	if (FModuleManager::Get().LoadModule(TEXT("SceneOutliner")) == nullptr)
		return;

#if PLATFORM_WINDOWS
	GSceneOutlinerHandle = FPlatformProcess::GetDllHandle(TEXT("UnrealEditor-SceneOutliner.dll"));
#elif PLATFORM_MAC
	GSceneOutlinerHandle = FPlatformProcess::GetDllHandle(TEXT("libUnrealEditor-SceneOutliner.dylib"));
#endif
	void* Target = FindSceneOutlinerSymbol(
		TEXT("?GetClassHyperlink@FSceneOutlinerHelpers@SceneOutliner@@SA?AV?$TSharedPtr@VSWidget@@$00@@PEAVUObject@@@Z"),
		"_ZN13SceneOutliner21FSceneOutlinerHelpers17GetClassHyperlinkEP7UObject");
	if (Target == nullptr || !InstallEditorInlineHook(GClassHyperlinkHook,
		Target, reinterpret_cast<void*>(&GetClassHyperlinkHook),
		TEXT("SceneOutliner::FSceneOutlinerHelpers::GetClassHyperlink")))
	{
		UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Scene Outliner script source-link hook is unavailable; the Outliner keeps Stock behavior."));
		if (GSceneOutlinerHandle != nullptr)
			FPlatformProcess::FreeDllHandle(GSceneOutlinerHandle);
		GSceneOutlinerHandle = nullptr;
	}
}

void ShutdownOutlinerScriptSourceLinkHook()
{
	const bool bRestored = RemoveEditorInlineHook(GClassHyperlinkHook,
		reinterpret_cast<void*>(&GetClassHyperlinkHook),
		TEXT("SceneOutliner::FSceneOutlinerHelpers::GetClassHyperlink"));
	if (bRestored && GSceneOutlinerHandle != nullptr)
		FPlatformProcess::FreeDllHandle(GSceneOutlinerHandle);
	if (bRestored)
	{
		GSceneOutlinerHandle = nullptr;
	}
}
}

#endif
