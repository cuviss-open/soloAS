// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Core/AngelscriptModKit.h"
#include "AngelscriptStructLiveValues.h"
#include "Core/ModKitSettings.h"
#include "Modules/ModuleManager.h"
#include "AngelscriptCoreExtensionPoints.h"
#include "ApiPolicy/ModKitApiAllowlist.h"
#include "ApiPolicy/ModKitApiClassification.h"
#include "ScriptCache/ModKitCacheTransform.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "HotPatch/ModKitHotPatch.h"
#include "ScriptCache/ModKitJitGeneration.h"
#include "RuntimeServices/ModKitMemoryIntegrity.h"
#include "FeatureMods/ModKitModRegistry.h"
#include "FeatureMods/ModKitRuntimeOps.h"
#include "FeatureMods/ModKitSignature.h"
#include "RuntimeServices/ModKitScriptExecutionTimeout.h"
#include "RuntimeServices/ModKitDebugAccess.h"
#include "Engine/AssetManager.h"
#include "Engine/AssetManagerSettings.h"
#include "GameFeatureData.h"
#include "Misc/CoreDelegates.h"
#include "Misc/App.h"
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#endif

namespace
{
bool ModKitDeniesDebugStart(bool bUnattended, bool bCommandlet, bool bEditor)
{
    return bUnattended || bCommandlet || !bEditor;
}

void EnsureDefaultGameFeatureDataRule()
{
    const FPrimaryAssetType Type(UGameFeatureData::StaticClass()->GetFName());
    UAssetManagerSettings* Settings = GetMutableDefault<UAssetManagerSettings>();
    if (Settings->PrimaryAssetTypesToScan.ContainsByPredicate(
        [&Type](const FPrimaryAssetTypeInfo& Info) { return Info.PrimaryAssetType == Type; }))
    {
        return;
    }
    if (UAssetManager::IsInitialized())
    {
        UAssetManager& Manager = UAssetManager::Get();
        FPrimaryAssetTypeInfo Existing;
        if (Manager.GetPrimaryAssetTypeInfo(Type, Existing))
        {
            return;
        }
        Manager.ScanPathsForPrimaryAssets(Type, {TEXT("/Game")}, UGameFeatureData::StaticClass(), false, false, false);
        FPrimaryAssetRules Rules;
        Rules.Priority = 0;
        Manager.SetPrimaryAssetTypeRules(Type, Rules);
    }
    else
    {
        // PreDefault runs before AssetManager scans config or GameFeatures validates it.
        FDirectoryPath Directory;
        Directory.Path = TEXT("/Game");
        FPrimaryAssetTypeInfo Info(UGameFeatureData::StaticClass()->GetFName(), UGameFeatureData::StaticClass(), false, false,
            {Directory}, {});
        Info.Rules.Priority = 0;
        Settings->PrimaryAssetTypesToScan.Add(MoveTemp(Info));
    }
    UE_LOG(LogAngelscriptModKit, Display, TEXT("ModKit installed the missing default GameFeatureData asset scan rule"));
}
}

class FAngelscriptModKitModule : public IModuleInterface
{
public:
	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool SupportsAutomaticShutdown() override
	{
		return !FModKitCacheTransform::RetainsFileView() && !DebugServerStartHandle.IsValid()
			&& !FModKitDebugAccess::IsRetained() && !FModKitCompilationManager::HasProviders()
			&& !FModKitSignature::HasSourceValidator() && !FModKitScriptExecutionTimeout::IsRetained()
            && !FModKitApiClassification::IsRegistered();
	}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
    bool HasDebugStartPolicyForTest() const { return DebugServerStartHandle.IsValid(); }
#endif

	virtual void StartupModule() override
	{
		StructLiveValuesToken = EnableAngelscriptStructLiveValues();
		UModKitSettings::Get();
		FModKitCacheTransform::Startup();
		FModKitJitGeneration::Startup();
		EnsureDefaultGameFeatureDataRule();
		FModKitSignature::Startup();
		const auto DebugRegistration = AngelscriptCoreExtensionPoints::RegisterDebugStartPolicy(
			[]() -> bool
			{
				return ModKitDeniesDebugStart(FApp::IsUnattended(), IsRunningCommandlet(), WITH_EDITOR != 0);
			}, DebugServerStartHandle);
		if (DebugRegistration != AngelscriptCoreExtensionPoints::EDebugPolicyRegistration::Registered)
			UE_LOG(LogAngelscriptModKit, Fatal, TEXT("ModKit debug start policy unavailable or too late"));
		FModKitDebugAccess::Startup();
		DebugPreExitHandle = FCoreDelegates::OnPreExit.AddRaw(this, &FAngelscriptModKitModule::RevokeCoreProviders);


		// Bind budget ownership before the first compiler/JIT query.
		FModKitScriptExecutionTimeout::Startup();

		FModKitApiClassification::Startup();
		FModKitModRegistry::Startup();
		FModKitCompilationManager::Startup();
		FModKitRuntimeOps::Startup();

		// Hot-patch orchestration : S-1 enqueue/ticker pump for
		// ModKit.Patch.FetchApply; landing re-verifies the MK-T702 chain.
		FModKitHotPatch::Startup();


		// Registration point CS.Runtime.Modding.RuntimeMemoryIntegrityGate
		// : delayed-degradation runtime integrity check sharing the
		// v11 disk-.text hash domain. Registered only when bMemoryIntegrityCheck
		// is enabled; an empty registry keeps both call sites no-ops.
		FModKitMemoryIntegrity::Startup();

	}

	virtual void ShutdownModule() override
	{
		RevokeCoreProviders();
		if (DebugServerStartHandle.IsValid() || FModKitDebugAccess::IsRetained()
			|| FModKitCompilationManager::HasProviders() || FModKitSignature::HasSourceValidator()
            || FModKitApiClassification::IsRegistered())
		{
			UE_LOG(LogAngelscriptModKit, Warning, TEXT("ModKit retains busy core providers during shutdown"));
			return;
		}
		if (DebugPreExitHandle.IsValid()) FCoreDelegates::OnPreExit.Remove(DebugPreExitHandle);
		FModKitMemoryIntegrity::Shutdown();
		FModKitCacheTransform::Shutdown();
		FModKitHotPatch::Shutdown();
		FModKitCompilationManager::Shutdown();
		FModKitSignature::Shutdown();
		FModKitModRegistry::Shutdown();
		FModKitScriptExecutionTimeout::Shutdown();
		DisableAngelscriptStructLiveValues(StructLiveValuesToken);
		StructLiveValuesToken = 0;
	}

private:
	void RevokeCoreProviders()
	{
		FModKitRuntimeOps::Shutdown();
		FModKitApiClassification::Shutdown();
		FModKitDebugAccess::Shutdown();
		if (DebugServerStartHandle.IsValid()
			&& AngelscriptCoreExtensionPoints::UnregisterDebugStartPolicy(DebugServerStartHandle)
				== AngelscriptCoreExtensionPoints::ERemoveResult::Removed)
			DebugServerStartHandle.Reset();
		FModKitCompilationManager::TryRevokeProviders();
		FModKitSignature::TryRevokeSourceValidator();
	}
	uint64 StructLiveValuesToken = 0;
	FDelegateHandle DebugServerStartHandle;
	FDelegateHandle DebugPreExitHandle;
};

IMPLEMENT_MODULE(FAngelscriptModKitModule, AngelscriptModKit)

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitDebugStartPolicyTest,
    "AngelscriptModKit.DebugAccess.StartPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModKitDebugStartPolicyTest::RunTest(const FString&)
{
    const auto* Module = FModuleManager::GetModulePtr<FAngelscriptModKitModule>(TEXT("AngelscriptModKit"));
    if (!TestNotNull(TEXT("ModKit module is loaded"), Module)) return false;
    TestTrue(TEXT("ModKit debug start policy is registered"), Module->HasDebugStartPolicyForTest());
    TestFalse(TEXT("Interactive editor permits debug start"), ModKitDeniesDebugStart(false, false, true));
    TestTrue(TEXT("Unattended mode denies debug start"), ModKitDeniesDebugStart(true, false, true));
    TestTrue(TEXT("Commandlet denies debug start"), ModKitDeniesDebugStart(false, true, true));
    TestTrue(TEXT("Game denies debug start"), ModKitDeniesDebugStart(false, false, false));
    TestTrue(TEXT("Current unattended process is denied"),
        ModKitDeniesDebugStart(FApp::IsUnattended(), IsRunningCommandlet(), WITH_EDITOR != 0));
    return !HasAnyErrors();
}
#endif
