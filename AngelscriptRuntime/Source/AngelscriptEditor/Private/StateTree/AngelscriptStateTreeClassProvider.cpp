// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptStateTreeClassProvider.h"

#include "ClassGenerator/AngelscriptClassState.h"
#include "ClassGenerator/AngelscriptClassGenerator.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "AngelscriptCodeModule.h"
#include "AngelscriptManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Containers/Ticker.h"
#include "StateTreeEditorModule.h"
#include "Editor/SoloASBridgeStateTree.h"
#include "Templates/Atomic.h"

namespace
{
// STATETREE_CLASS_DISCOVERY_MARKER: explicit external script-node provider.
bool IsAngelscriptStateTreeScriptNodeClass(const UClass* Class)
{
	return Class != nullptr
		&& IsValid(Class)
		&& Class->GetFName() != NAME_None
		&& !Class->HasAnyClassFlags(CLASS_NewerVersionExists)
		&& GetAngelscriptClassScriptType(Class) != nullptr
		&& Class->GetClassPathName().IsValid();
}

// Fixed UE 5.8.3 Haze exposes the script identity consumed by its private
// StateTree cache as a public UClass member. Stock UE has no such member; the
// requires-expression therefore removes this route from the stock build.
template <typename T>
bool MarkHazeScriptClassForStateTreeCache(T* Class)
{
	if (Class == nullptr)
	{
		return false;
	}

	bool bMarked = false;
	[&bMarked](T* Candidate)
	{
		if constexpr (requires(T& Value) { Value.bIsScriptClass; })
		{
			Candidate->bIsScriptClass = true;
			bMarked = true;
		}
	}(Class);
	return bMarked;
}
}

const TCHAR* FAngelscriptStateTreeClassProvider::GetMarker()
{
	return TEXT("AngelscriptStateTreeScriptNodeClassProvider");
}

void FAngelscriptStateTreeClassProvider::GetScriptNodeClasses(
	TArray<UClass*>& OutClasses)
{
	OutClasses.Reset();
	TSet<UClass*> SeenClasses;

	for (TObjectIterator<UClass> ClassIterator; ClassIterator; ++ClassIterator)
	{
		UClass* Class = *ClassIterator;
		if (!IsAngelscriptStateTreeScriptNodeClass(Class))
		{
			continue;
		}

		MarkHazeScriptClassForStateTreeCache(Class);

		if (SeenClasses.Contains(Class))
		{
			continue;
		}

		SeenClasses.Add(Class);
		OutClasses.Add(Class);
	}

	OutClasses.StableSort([](const UClass& Left, const UClass& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});
}

void FAngelscriptStateTreeClassProvider::GetScriptNodeClassDataForBaseClass(
	const UClass* BaseClass,
	TArray<TSharedPtr<FStateTreeNodeClassData>>& OutClassData)
{
	OutClassData.Reset();

	if (BaseClass == nullptr
		|| !IsValid(BaseClass))
	{
		return;
	}

	TArray<UClass*> ScriptClasses;
	GetScriptNodeClasses(ScriptClasses);
	TSet<FString> SeenClassPaths;

	for (UClass* Class : ScriptClasses)
	{
		if (!IsAngelscriptStateTreeScriptNodeClass(Class)
			|| !Class->IsChildOf(BaseClass))
		{
			continue;
		}

		const FString ClassPath = Class->GetPathName();
		if (ClassPath.IsEmpty() || SeenClassPaths.Contains(ClassPath))
		{
			continue;
		}

		SeenClassPaths.Add(ClassPath);
		OutClassData.Add(MakeShared<FStateTreeNodeClassData>(TNotNull<UStruct*>(Class)));
	}
}

namespace
{
struct FNormalStateTreeScriptClasses
{
    FDelegateHandle EngineInit, InitialCompile, PostCompile, ScriptReload, FilesLoaded, BlueprintCompiled, PackageChanged, ReloadComplete;
    FTSTicker::FDelegateHandle InitTick;
    bool bReady = false;
    bool bRefreshAfterCompile = true;
    TSet<FTopLevelAssetPath> PreviousPaths;
    bool bRefreshing = false;
    TAtomic<bool> bRefreshPending{false};

    void ScriptsCompiled()
    {
        // Soft reload preserves UClass identity and the native cache's weak class/path entries.
        if (!bRefreshAfterCompile) return;
        bRefreshAfterCompile = false;
        Refresh();
    }

    void Refresh()
    {
        if (!IsInGameThread())
        {
            bRefreshPending.Store(true);
            return;
        }
        if (!GEditor || !bReady || !FAngelscriptManager::IsInitialized() || bRefreshing) return;
        TGuardValue<bool> Guard(bRefreshing, true);
        TArray<UClass*> Classes;
        FAngelscriptStateTreeClassProvider::GetScriptNodeClasses(Classes);
        auto Cache = FStateTreeEditorModule::GetModule().GetNodeClassCache();
        SoloASBridge::Editor::SynchronizeStateTreeScriptClasses(*Cache, Classes, PreviousPaths);
        PreviousPaths.Reset();
        for (const UClass* Class : Classes) PreviousPaths.Add(Class->GetClassPathName());
    }

    void ScheduleReady()
    {
        if (!GEditor || FilesLoaded.IsValid()) return;
        // UE broadcasts multicast delegates in reverse registration order. Register
        // before the native cache constructor so its invalidation precedes compensation.
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        FilesLoaded = Registry.OnFilesLoaded().AddRaw(this, &FNormalStateTreeScriptClasses::Refresh);
        BlueprintCompiled = GEditor->OnBlueprintCompiled().AddRaw(this, &FNormalStateTreeScriptClasses::Refresh);
        PackageChanged = GEditor->OnClassPackageLoadedOrUnloaded().AddRaw(this, &FNormalStateTreeScriptClasses::Refresh);
        ReloadComplete = FCoreUObjectDelegates::ReloadCompleteDelegate.AddLambda([this](EReloadCompleteReason) { Refresh(); });
        // The native cache is assigned by a later PostEngineInit callback. Read it
        // on the first normal editor tick, after all initialization callbacks finish.
        InitTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
        {
            const bool bPending = bRefreshPending.Exchange(false);
            if (!bReady || bPending)
            {
                bReady = true;
                Refresh();
            }
            return true;
        }));
    }
};
FNormalStateTreeScriptClasses NormalStateTreeScriptClasses;
}

void FAngelscriptStateTreeClassProvider::Initialize()
{
    auto& State = NormalStateTreeScriptClasses;
    FStateTreeEditorModule::GetModule();
    State.EngineInit = FCoreDelegates::GetOnPostEngineInit().AddRaw(&State, &FNormalStateTreeScriptClasses::ScheduleReady);
    State.InitialCompile = FAngelscriptCodeModule::GetOnInitialCompileFinished().AddRaw(&State, &FNormalStateTreeScriptClasses::Refresh);
    State.ScriptReload = FAngelscriptClassGenerator::OnPostReload.AddLambda([&State](bool bFullReload)
    {
        State.bRefreshAfterCompile |= bFullReload;
    });
    State.PostCompile = FAngelscriptCodeModule::GetPostCompile().AddRaw(&State, &FNormalStateTreeScriptClasses::ScriptsCompiled);
    if (GIsRunning && GEditor) State.ScheduleReady();
}

void FAngelscriptStateTreeClassProvider::Shutdown()
{
    auto& State = NormalStateTreeScriptClasses;
    FCoreDelegates::GetOnPostEngineInit().Remove(State.EngineInit);
    FAngelscriptCodeModule::GetOnInitialCompileFinished().Remove(State.InitialCompile);
    FAngelscriptCodeModule::GetPostCompile().Remove(State.PostCompile);
    FAngelscriptClassGenerator::OnPostReload.Remove(State.ScriptReload);
    if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
        FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().OnFilesLoaded().Remove(State.FilesLoaded);
    if (GEditor)
    {
        GEditor->OnBlueprintCompiled().Remove(State.BlueprintCompiled);
        GEditor->OnClassPackageLoadedOrUnloaded().Remove(State.PackageChanged);
    }
    FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(State.ReloadComplete);
    FTSTicker::GetCoreTicker().RemoveTicker(State.InitTick);
    State.bReady = false;
    State.bRefreshAfterCompile = true;
    State.bRefreshPending.Store(false);
    State.PreviousPaths.Reset();
}
