#if SOLOAS_WITH_STATETREE
// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptStateTreeClassProvider.h"
                                                                                                                                                  
#include "Compat/SoloASRuntimeCompat.h"
// SOLOAS-COMPAT-END

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
#include "StateTreeNodeClassCache.h"
#include "Editor/SoloASBridgeStateTree.h"
#include "Templates/Atomic.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

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

// Fixed UE 5.8.3 UE-AS fork exposes the script identity consumed by its private
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
		                                                                                          
                                                                                      
#if SOLOAS_UE_BEFORE(5, 7)
        OutClassData.Add(MakeShared<FStateTreeNodeClassData>(Class));
#else
        OutClassData.Add(MakeShared<FStateTreeNodeClassData>(TNotNull<UStruct*>(Class)));
#endif
// SOLOAS-COMPAT-END
                                                                                        
	}
}

namespace
{
struct FStateTreeReloadComplete
{
    using Type = void (FStateTreeNodeClassCache::*)(EReloadCompleteReason);
    friend Type Resolve(FStateTreeReloadComplete);
};
template <typename Tag, typename Tag::Type Member>
struct TStateTreeDelegateAccess
{
    friend typename Tag::Type Resolve(Tag) { return Member; }
};
template struct TStateTreeDelegateAccess<FStateTreeReloadComplete, &FStateTreeNodeClassCache::OnReloadComplete>;

struct FNormalStateTreeScriptClasses
{
    FDelegateHandle EngineInit, InitialCompile, PostCompile, ScriptReload, FilesLoaded, BlueprintCompiled, PackageChanged, ReloadComplete;
    FTSTicker::FDelegateHandle InitTick;
    bool bReady = false;
    bool bRefreshAfterCompile = true;
    TSet<FTopLevelAssetPath> PreviousPaths;
    bool bRefreshing = false;
    TAtomic<bool> bRefreshPending{false};
    TWeakPtr<FStateTreeNodeClassCache> BoundCache;

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
        if (bRefreshing)
        {
            bRefreshPending.Store(true);
            return;
        }
        if (!GEditor || !bReady || !FAngelscriptManager::IsInitialized()) return;
        TGuardValue<bool> Guard(bRefreshing, true);
        TArray<UClass*> Classes;
        FAngelscriptStateTreeClassProvider::GetScriptNodeClasses(Classes);
        auto Cache = SoloASBridge::Editor::GetStateTreeEditorModule().GetNodeClassCache();
        SoloASBridge::Editor::SynchronizeStateTreeScriptClasses(*Cache, Classes, PreviousPaths);
        PreviousPaths.Reset();
        for (const UClass* Class : Classes) PreviousPaths.Add(Class->GetClassPathName());
    }

    void InvalidateAndRefresh()
    {
        if (auto Cache = BoundCache.Pin())
        {
            Cache->InvalidateCache();
            Refresh();
        }
    }

    void BindInvalidation()
    {
        auto Cache = SoloASBridge::Editor::GetStateTreeEditorModule().GetNodeClassCache();
        BoundCache = Cache;
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        // CS.File.AngelscriptRuntime.Source.AngelscriptEditor.Private.StateTree.AngelscriptStateTreeClassProvider.cpp
        Registry.OnFilesLoaded().RemoveAll(Cache.Get());
        GEditor->OnBlueprintCompiled().RemoveAll(Cache.Get());
        GEditor->OnClassPackageLoadedOrUnloaded().RemoveAll(Cache.Get());
        FCoreUObjectDelegates::ReloadCompleteDelegate.RemoveAll(Cache.Get());
        FilesLoaded = Registry.OnFilesLoaded().AddRaw(this, &FNormalStateTreeScriptClasses::InvalidateAndRefresh);
        BlueprintCompiled = GEditor->OnBlueprintCompiled().AddRaw(this, &FNormalStateTreeScriptClasses::InvalidateAndRefresh);
        PackageChanged = GEditor->OnClassPackageLoadedOrUnloaded().AddRaw(this, &FNormalStateTreeScriptClasses::InvalidateAndRefresh);
        ReloadComplete = FCoreUObjectDelegates::ReloadCompleteDelegate.AddLambda([this](EReloadCompleteReason) { InvalidateAndRefresh(); });
    }

    void ScheduleReady()
    {
        if (!GEditor || InitTick.IsValid()) return;
        // The native cache is assigned by a later PostEngineInit callback. Read it
        // on the first normal editor tick, after all initialization callbacks finish.
        InitTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
        {
            const bool bPending = bRefreshPending.Exchange(false);
            if (!bReady || bPending)
            {
                if (!bReady) BindInvalidation();
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
    SoloASBridge::Editor::GetStateTreeEditorModule();
                                                                                                                                                      
    State.EngineInit = SoloASBridge::Compat::OnPostEngineInit().AddRaw(&State, &FNormalStateTreeScriptClasses::ScheduleReady);
// SOLOAS-COMPAT-END
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
                                                                                                                                                         
    SoloASBridge::Compat::OnPostEngineInit().Remove(State.EngineInit);
// SOLOAS-COMPAT-END
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
    if (auto Cache = State.BoundCache.Pin())
    {
        if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
            FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().OnFilesLoaded().AddRaw(Cache.Get(), &FStateTreeNodeClassCache::InvalidateCache);
        if (GEditor)
        {
            GEditor->OnBlueprintCompiled().AddRaw(Cache.Get(), &FStateTreeNodeClassCache::InvalidateCache);
            GEditor->OnClassPackageLoadedOrUnloaded().AddRaw(Cache.Get(), &FStateTreeNodeClassCache::InvalidateCache);
        }
        FCoreUObjectDelegates::ReloadCompleteDelegate.AddRaw(Cache.Get(), Resolve(FStateTreeReloadComplete{}));
    }
    State.BoundCache.Reset();
    State.InitTick.Reset();
    State.bReady = false;
    State.bRefreshAfterCompile = true;
    State.bRefreshPending.Store(false);
    State.PreviousPaths.Reset();
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStateTreeProviderShutdownRestoresNative,
    "SoloAS.StateTreeProviderShutdownRestoresNative", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStateTreeProviderShutdownRestoresNative::RunTest(const FString&)
{
    auto Cache = NormalStateTreeScriptClasses.BoundCache.Pin();
    if (!TestTrue(TEXT("Normal StateTree cache is bound"), Cache.IsValid())) return false;
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    FAngelscriptStateTreeClassProvider::Shutdown();
    TestTrue(TEXT("Asset registry invalidation restored"), Registry.OnFilesLoaded().IsBoundToObject(Cache.Get()));
    TestTrue(TEXT("Blueprint invalidation restored"), GEditor->OnBlueprintCompiled().IsBoundToObject(Cache.Get()));
    TestTrue(TEXT("Class package invalidation restored"), GEditor->OnClassPackageLoadedOrUnloaded().IsBoundToObject(Cache.Get()));
    TestTrue(TEXT("Reload invalidation restored"), FCoreUObjectDelegates::ReloadCompleteDelegate.IsBoundToObject(Cache.Get()));
    FAngelscriptStateTreeClassProvider::Initialize();
    return !HasAnyErrors();
}
#endif

#endif

#if !SOLOAS_WITH_STATETREE
#include "AngelscriptStateTreeClassProvider.h"
void FAngelscriptStateTreeClassProvider::Initialize() {}
void FAngelscriptStateTreeClassProvider::Shutdown() {}
#endif
