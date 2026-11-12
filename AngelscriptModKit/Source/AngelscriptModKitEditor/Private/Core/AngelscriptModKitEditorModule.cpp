// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Core/FeatureModEditor.h"

#include "FeatureMods/FeatureModManagerWidget.h"
#include "Core/FeatureModSettings.h"
#include "Core/ModKitSettings.h"
#include "FeatureMods/FeatureModSettingsCustomization.h"
#include "Framework/Docking/TabManager.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

namespace
{
	const FName SettingsCategoryName(TEXT("SoloAS"));
	const FName SettingsSectionName(TEXT("FeatureMods"));
	const FName MenuOwnerName(TEXT("AngelscriptModKitEditor"));
	bool bMenuEntryRegistered = false;
	bool bSettingsRegistered = false;
	bool bManagerTabRegistered = false;
}

const FName FFeatureModEditor::ManagerTabId(TEXT("AngelscriptModKit.FeatureMods"));

void FFeatureModEditor::OpenManager()
{
	FGlobalTabmanager::Get()->TryInvokeTab(ManagerTabId);
}

void FFeatureModEditor::OpenSettings()
{
	if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
	{
		Settings->ShowViewer(TEXT("Project"), SettingsCategoryName, SettingsSectionName);
	}
}

bool FFeatureModEditor::IsMenuEntryRegistered()
{
	return bMenuEntryRegistered;
}

bool FFeatureModEditor::AreNavigationTargetsRegistered()
{
	return bManagerTabRegistered && bSettingsRegistered;
}

void FFeatureModEditor::SetMenuEntryRegistered(bool bRegistered)
{
	bMenuEntryRegistered = bRegistered;
}

void FFeatureModEditor::SetSettingsRegistered(bool bRegistered)
{
	bSettingsRegistered = bRegistered;
}

void FFeatureModEditor::SetManagerTabRegistered(bool bRegistered)
{
	bManagerTabRegistered = bRegistered;
}

class FAngelscriptModKitEditorModule : public IModuleInterface
{
public:
	virtual bool SupportsDynamicReloading() override { return false; }

	virtual void StartupModule() override
	{
		UFeatureModSettings::Get();
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(FFeatureModEditor::ManagerTabId,
			FOnSpawnTab::CreateRaw(this, &FAngelscriptModKitEditorModule::SpawnManagerTab))
			.SetDisplayName(FText::FromString(TEXT("FeatureMods")))
			.SetTooltipText(FText::FromString(TEXT("Manage FeatureMods and project policy.")))
			.SetMenuType(ETabSpawnerMenuType::Hidden);
		FFeatureModEditor::SetManagerTabRegistered(true);

		ISettingsModule& Settings = FModuleManager::LoadModuleChecked<ISettingsModule>(TEXT("Settings"));
		const TSharedPtr<ISettingsSection> SettingsSection = Settings.RegisterSettings(TEXT("Project"), SettingsCategoryName, SettingsSectionName,
			FText::FromString(TEXT("FeatureMods")),
			FText::FromString(TEXT("FeatureMod discovery roots. Restart the editor after changing settings.")),
			GetMutableDefault<UFeatureModSettings>());
		const TSharedPtr<ISettingsSection> RuntimeSection = Settings.RegisterSettings(TEXT("Project"), SettingsCategoryName, TEXT("ModKitRuntime"),
			FText::FromString(TEXT("ModKit Runtime")),
			FText::FromString(TEXT("Runtime policy (DefaultEngine.ini). Restart to apply changes. Package signing uses PatchPublicKeyBase64 and PatchVerifyKeyId; Shipping trust changes require rebuilding.")),
			GetMutableDefault<UModKitSettings>());
		FFeatureModEditor::SetSettingsRegistered(SettingsSection.IsValid() && RuntimeSection.IsValid());

		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		PropertyEditor.RegisterCustomClassLayout(UFeatureModSettings::StaticClass()->GetFName(),
			FOnGetDetailCustomizationInstance::CreateStatic(&FFeatureModSettingsCustomization::MakeInstance));
		PropertyEditor.NotifyCustomizationModuleChanged();

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this,
			&FAngelscriptModKitEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(MenuOwnerName);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(FFeatureModEditor::ManagerTabId);
		FFeatureModEditor::SetMenuEntryRegistered(false);
		FFeatureModEditor::SetManagerTabRegistered(false);

		if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
		{
			Settings->UnregisterSettings(TEXT("Project"), SettingsCategoryName, SettingsSectionName);
			Settings->UnregisterSettings(TEXT("Project"), SettingsCategoryName, TEXT("ModKitRuntime"));
		}
		FFeatureModEditor::SetSettingsRegistered(false);

		if (FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
		{
			PropertyEditor->UnregisterCustomClassLayout(UFeatureModSettings::StaticClass()->GetFName());
			PropertyEditor->NotifyCustomizationModuleChanged();
		}
	}

private:
	TSharedRef<SDockTab> SpawnManagerTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SFeatureModManagerWidget)
			];
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(MenuOwnerName);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.SoloAS"));
		if (!Menu)
		{
			return;
		}
		FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("FeatureMods"));
		Section.AddMenuEntry(TEXT("OpenFeatureModManager"), FText::FromString(TEXT("FeatureMods")),
			FText::FromString(TEXT("Open the FeatureMod manager.")), FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&FFeatureModEditor::OpenManager)));
		FFeatureModEditor::SetMenuEntryRegistered(true);
	}
};

IMPLEMENT_MODULE(FAngelscriptModKitEditorModule, AngelscriptModKitEditor)
