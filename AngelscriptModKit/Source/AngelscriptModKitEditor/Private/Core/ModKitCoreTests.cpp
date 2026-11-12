// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "Core/FeatureModEditor.h"
#include "Core/FeatureModSettings.h"
#include "Core/ModKitSettings.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitCoreSettingsTest,
	"AngelscriptModKit.Editor.Core.Settings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModKitCoreSettingsTest::RunTest(const FString&)
{
	const UModKitSettings* Policy = UModKitSettings::Get();
	UModKitSettings* Editable = GetMutableDefault<UModKitSettings>();
	TestTrue(TEXT("Runtime policy is separate from the editable settings"), Policy != Editable);
	TestEqual(TEXT("A fresh process captures its configured switch"), Policy->bEnableHotPatch, Editable->bEnableHotPatch);
	if (FMath::IsFinite(Editable->ModStartupActivationTimeoutSec) && Editable->ModStartupActivationTimeoutSec >= 0.1f)
	{
		TestEqual(TEXT("A fresh process captures its configured timeout"), Policy->ModStartupActivationTimeoutSec, Editable->ModStartupActivationTimeoutSec);
	}
	TestEqual(TEXT("A fresh process captures its configured patch feed"), Policy->PatchFeedUrl, Editable->PatchFeedUrl);
	TestEqual(TEXT("A fresh process captures its configured patch staging root"), Policy->PatchStagingRoot, Editable->PatchStagingRoot);
	const FString SavedFeed = Editable->PatchFeedUrl;
	const FString ActiveFeed = Policy->PatchFeedUrl;
	Editable->PatchFeedUrl = TEXT("http://127.0.0.1/edited-feed");
	TestEqual(TEXT("Feed edits require a process restart"), UModKitSettings::Get()->PatchFeedUrl, ActiveFeed);
	Editable->PatchFeedUrl = SavedFeed;
	const bool SavedHotPatch = Editable->bEnableHotPatch;
	const bool ActiveHotPatch = Policy->bEnableHotPatch;
	Editable->bEnableHotPatch = !ActiveHotPatch;
	TestEqual(TEXT("Editing an off/on switch does not change this process policy"), UModKitSettings::Get()->bEnableHotPatch, ActiveHotPatch);
	Editable->bEnableHotPatch = SavedHotPatch;
	const float SavedTimeout = Editable->ModStartupActivationTimeoutSec;
	const float ActiveTimeout = Policy->ModStartupActivationTimeoutSec;
	Editable->ModStartupActivationTimeoutSec = -1.0f;
	TestEqual(TEXT("An invalid edit cannot change the running timeout"), UModKitSettings::Get()->ModStartupActivationTimeoutSec, ActiveTimeout);
	Editable->ModStartupActivationTimeoutSec = SavedTimeout;
	TestTrue(TEXT("Startup timeout is finite and positive"), FMath::IsFinite(ActiveTimeout) && ActiveTimeout >= 0.1f);

	UFeatureModSettings* EditorSettings = GetMutableDefault<UFeatureModSettings>();
	const TArray<FString> SavedRoots = EditorSettings->AllowedRoots;
	const TArray<FString> ActiveRoots = UFeatureModSettings::Get()->AllowedRoots;
	EditorSettings->AllowedRoots = { TEXT("Invalid/../Root") };
	TestTrue(TEXT("Editor roots stay frozen while settings are edited"), UFeatureModSettings::Get()->AllowedRoots == ActiveRoots);
	EditorSettings->AllowedRoots = SavedRoots;

	for (const TCHAR* Removed : { TEXT("SigningPublicKey"), TEXT("UnsignedModPolicy"), TEXT("LoadTimeoutSeconds"),
		TEXT("bAutoDisableFailedMods"), TEXT("bAllowUnsignedInDevelopment") })
	{
		TestNull(FString::Printf(TEXT("No unused editor setting: %s"), Removed), FindFProperty<FProperty>(UFeatureModSettings::StaticClass(), Removed));
	}
	TestNotNull(TEXT("Editor script reload switch is available"), FindFProperty<FProperty>(UFeatureModSettings::StaticClass(), TEXT("bEnableDevelopmentHotReload")));
	TestNotNull(TEXT("Actual signing public key belongs to runtime settings"), FindFProperty<FProperty>(UModKitSettings::StaticClass(), TEXT("PatchPublicKeyBase64")));
	TestNotNull(TEXT("Actual signing key id belongs to runtime settings"), FindFProperty<FProperty>(UModKitSettings::StaticClass(), TEXT("PatchVerifyKeyId")));
	TestTrue(TEXT("Runtime module is enabled"), FModuleManager::Get().IsModuleLoaded(TEXT("AngelscriptModKit")));
	TestFalse(TEXT("Runtime DLL hot unload is unsupported"), FModuleManager::GetModuleChecked<IModuleInterface>(TEXT("AngelscriptModKit")).SupportsDynamicReloading());
	TestTrue(TEXT("FeatureMod menu entry is registered"), FFeatureModEditor::IsMenuEntryRegistered());
	TestTrue(TEXT("Settings and manager navigation are registered"), FFeatureModEditor::AreNavigationTargetsRegistered());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitCoreEditorLifecycleTest,
	"AngelscriptModKit.Editor.Core.EditorLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModKitCoreEditorLifecycleTest::RunTest(const FString&)
{
	IModuleInterface& Module = FModuleManager::GetModuleChecked<IModuleInterface>(TEXT("AngelscriptModKitEditor"));
	for (int32 Cycle = 0; Cycle < 2; ++Cycle)
	{
		Module.ShutdownModule();
		TestFalse(TEXT("Menu state is cleared on shutdown"), FFeatureModEditor::IsMenuEntryRegistered());
		TestFalse(TEXT("Settings and tab state are cleared on shutdown"), FFeatureModEditor::AreNavigationTargetsRegistered());
		Module.StartupModule();
		TestTrue(TEXT("Menu registration is restored"), FFeatureModEditor::IsMenuEntryRegistered());
		TestTrue(TEXT("Settings and tab registration are restored"), FFeatureModEditor::AreNavigationTargetsRegistered());
	}
	return true;
}

#endif
