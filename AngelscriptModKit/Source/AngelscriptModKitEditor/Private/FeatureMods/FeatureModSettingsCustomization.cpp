// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "FeatureMods/FeatureModSettingsCustomization.h"

#include "Core/FeatureModEditor.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

TSharedRef<IDetailCustomization> FFeatureModSettingsCustomization::MakeInstance()
{
	return MakeShared<FFeatureModSettingsCustomization>();
}

void FFeatureModSettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	IDetailCategoryBuilder& Navigation = DetailBuilder.EditCategory(TEXT("Manager"), FText::FromString(TEXT("FeatureMod Manager")));
	Navigation.AddCustomRow(FText::FromString(TEXT("Runtime policy")))
		.WholeRowContent()
		[
			SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("Runtime trust, signing keys, API permissions and activation timeout are configured under Project Settings > SoloAS > ModKit Runtime. Changes require a restart. Discovery roots must be Plugins/FeatureMods or FeatureMods. Development hot reload only applies to editor script reloads; stop PIE before reloading.")))
		];
	Navigation.AddCustomRow(FText::FromString(TEXT("Runtime settings")))
		.WholeRowContent()
		[
			SNew(SButton).Text(FText::FromString(TEXT("Open ModKit runtime settings")))
			.OnClicked_Lambda([]()
			{
				if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
					Settings->ShowViewer(TEXT("Project"), TEXT("SoloAS"), TEXT("ModKitRuntime"));
				return FReply::Handled();
			})
		];
	Navigation.AddCustomRow(FText::FromString(TEXT("FeatureMod Manager")))
		.WholeRowContent()
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Open FeatureMods manager")))
			.ToolTipText(FText::FromString(TEXT("Return to the SoloAS FeatureMods manager window.")))
			.OnClicked_Lambda([]()
			{
				FFeatureModEditor::OpenManager();
				return FReply::Handled();
			})
		];
}
