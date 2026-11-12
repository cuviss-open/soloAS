// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "FeatureMods/FeatureModCatalog.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

class SFeatureModManagerWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFeatureModManagerWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& Arguments);

private:
	friend class FFeatureModEditorContractTest;
	friend class FFeatureModManagerLifecycleTest;
	using FFeatureModItem = TSharedPtr<FFeatureModInfo>;

	TSharedRef<ITableRow> MakeFeatureModRow(FFeatureModItem Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnSelectionChanged(FFeatureModItem Item, ESelectInfo::Type SelectInfo);
	void RefreshCatalog(bool bPreserveEdits = false);
	void AddWhitelistDiagnostics(TArray<FFeatureModInfo>& FeatureMods) const;
	void UpdateDetails();
	void UpdateDependencyChoices();
	void SetDependency(const FString& Kind, const FString& Id, bool bEnabled);
	void SelectFeatureMod(const FString& Id);
	FReply CreateFeatureMod();
	FReply SaveManifest();
	FReply RequestSelected(bool bLoad);
	FReply RunCheck();
	FReply OpenSettings();
	FReply OpenPackaging();
	bool CanSaveManifest() const;
	bool CanReloadSelected() const;
	bool CanOperateSelected(bool bRequireValidManifest = true) const;
	FString BuildDiagnosticText() const;

	FFeatureModManifest EditedManifest;
	TArray<FFeatureModInfo> FeatureMods;
	TArray<FFeatureModPluginInfo> Plugins;
	TArray<FFeatureModItem> FeatureModItems;
	FString SelectedFeatureModId;
	FString SelectedManifestPath;

	TSharedPtr<SListView<FFeatureModItem>> FeatureModList;
	TSharedPtr<SMultiLineEditableTextBox> ManifestEditor;
	TSharedPtr<SMultiLineEditableTextBox> DependencyView;
	TSharedPtr<class SVerticalBox> DependencyChoices;
	TSharedPtr<STextBlock> SelectedTitle;
	TSharedPtr<STextBlock> DiagnosticView;
};
