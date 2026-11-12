// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "FeatureMods/FeatureModManagerWidget.h"

#include "FeatureMods/FeatureModCatalog.h"
#include "Core/FeatureModEditor.h"
#include "Core/FeatureModSettings.h"
#include "Core/ModKitSettings.h"
#include "../../../AngelscriptModKit/Private/FeatureMods/ModKitRuntimeOps.h"
#include "../../../AngelscriptModKit/Private/FeatureMods/ModKitModRegistry.h"
#include "Editor.h"
#include "../../../AngelscriptModKit/Private/DeveloperTools/ModKitCommandlets.h"
#include "Misc/Guid.h"
#include "Misc/MessageDialog.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

namespace
{
	FText AsText(const FString& Value)
	{
		return FText::FromString(Value);
	}

	FString LocationName(const EFeatureModLocation Location)
	{
		return Location == EFeatureModLocation::Internal ? TEXT("Internal") : TEXT("External");
	}

}

void SFeatureModManagerWidget::Construct(const FArguments& Arguments)
{
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(6.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
			[
				SNew(SButton).Text(AsText(TEXT("Create FeatureMod"))).OnClicked(this, &SFeatureModManagerWidget::CreateFeatureMod)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
			[
				SNew(SButton).Text(AsText(TEXT("One-click check"))).OnClicked(this, &SFeatureModManagerWidget::RunCheck)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
			[
				SNew(SButton).Text(AsText(TEXT("Project Settings"))).OnClicked(this, &SFeatureModManagerWidget::OpenSettings)
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(6.0f)
		[
			SNew(SSplitter)
			+ SSplitter::Slot().Value(0.34f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
				[
					SNew(STextBlock).Text(AsText(TEXT("FeatureMods — location | layer | enabled | manifest version | package manifest | runtime trust | load state")))
				]
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SAssignNew(FeatureModList, SListView<FFeatureModItem>)
						.ListItemsSource(&FeatureModItems)
						.OnGenerateRow(this, &SFeatureModManagerWidget::MakeFeatureModRow)
						.OnSelectionChanged(this, &SFeatureModManagerWidget::OnSelectionChanged)
				]
			]
			+ SSplitter::Slot().Value(0.66f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
				[
					SAssignNew(SelectedTitle, STextBlock).Text(AsText(TEXT("Select a FeatureMod")))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
					[
						SNew(SButton).Text(AsText(TEXT("Load")))
						.IsEnabled_Lambda([this] { return CanOperateSelected() && !FModKitRuntimeOps::IsOperationPending(SelectedFeatureModId) && !FModKitModRegistry::IsModActivated(SelectedFeatureModId); })
						.OnClicked_Lambda([this] { return RequestSelected(true); })
						.ToolTipText(AsText(TEXT("Request FeatureMod activation in the editor. Stop PIE first. Runtime validation and player approval still apply.")))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
					[
						SNew(SButton).Text(AsText(TEXT("Unload")))
						.IsEnabled_Lambda([this] { return CanOperateSelected(false) && !FModKitRuntimeOps::IsOperationPending(SelectedFeatureModId) && FModKitModRegistry::IsModActivated(SelectedFeatureModId); })
						.OnClicked_Lambda([this] { return RequestSelected(false); })
						.ToolTipText(AsText(TEXT("Request FeatureMod deactivation in the editor. Stop PIE first; live instances may prevent unloading.")))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
					[
						SNew(SButton).Text(AsText(TEXT("Hot reload")))
						.IsEnabled_Lambda([this] { return CanReloadSelected(); })
						.OnClicked_Lambda([this]
						{
							const bool bKeepEdits = FeatureMods.ContainsByPredicate([this](const FFeatureModInfo& Item)
							{
								return Item.Id == SelectedFeatureModId && Item.ManifestText != ManifestEditor->GetText().ToString();
							});
							FModKitRuntimeOps::RequestReload(SelectedFeatureModId);
							RefreshCatalog(bKeepEdits);
							SelectFeatureMod(SelectedFeatureModId);
							return FReply::Handled();
						})
						.ToolTipText(AsText(TEXT("Reload an active FeatureMod in the editor. Stop PIE first. Script hot reload is enabled by default and can be disabled in Project Settings.")))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
					[
						SNew(SButton).Text(AsText(TEXT("Assemble")))
						.IsEnabled_Lambda([this] { return !SelectedFeatureModId.IsEmpty(); })
						.OnClicked(this, &SFeatureModManagerWidget::OpenPackaging)
						.ToolTipText(AsText(TEXT("Assemble this FeatureMod into a separate delivery directory. Prepared source or cache files are preserved; this does not cook content.")))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
					[
						SNew(SButton).Text(AsText(TEXT("Sign"))).IsEnabled_Lambda([this] { return !SelectedFeatureModId.IsEmpty(); })
						.OnClicked_Lambda([]()
						{
							FMessageDialog::Open(EAppMsgType::Ok, AsText(TEXT("Configure your publisher public key and Key ID, then Assemble to produce the package draft and .canonical bytes. Sign those exact bytes with your external Ed25519 tool and put the Base64 signature in signature.value. Run ModKitPatchVerify -Kind=package before deploying the Mod directory and its manifest. Private keys stay outside the product. Test fixture keys must not be used for publishing.")));
							return FReply::Handled();
						})
						.ToolTipText(AsText(TEXT("Show the official package signing workflow.")))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
					[
						SNew(SButton).Text(AsText(TEXT("Save FeatureMod.json"))).IsEnabled(this, &SFeatureModManagerWidget::CanSaveManifest)
						.OnClicked(this, &SFeatureModManagerWidget::SaveManifest)
					]
				]
				+ SVerticalBox::Slot().FillHeight(0.30f).Padding(4.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()[SAssignNew(DependencyChoices, SVerticalBox)]
				]
				+ SVerticalBox::Slot().FillHeight(0.20f).Padding(4.0f)
				[
					SAssignNew(DependencyView, SMultiLineEditableTextBox)
						.IsReadOnly(true)
				]
				+ SVerticalBox::Slot().FillHeight(0.46f).Padding(4.0f)
				[
					SAssignNew(ManifestEditor, SMultiLineEditableTextBox)
						.IsReadOnly(false)
						.OnTextChanged_Lambda([this](const FText& Text)
						{
							TArray<FString> Errors;
							FFeatureModCatalog::ParseManifest(Text.ToString(), EditedManifest, Errors);
						})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
				[
					SAssignNew(DiagnosticView, STextBlock).AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
				[
					SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]
					{
						return AsText(FModKitRuntimeOps::GetOperationStatus(SelectedFeatureModId));
					})
				]
			]
		]
	];

	RefreshCatalog();
}

TSharedRef<ITableRow> SFeatureModManagerWidget::MakeFeatureModRow(FFeatureModItem Item,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Signature = Item->bSigned ? TEXT("Present (unverified)") : TEXT("Unsigned");
	return SNew(STableRow<FFeatureModItem>, OwnerTable)
	[
		SNew(STextBlock).Text_Lambda([Item, Signature]
		{
			FString ScriptDirectory, RelativeRoot;
			const bool bKnown = FModKitModRegistry::GetModScriptPaths(Item->Id, ScriptDirectory, RelativeRoot)
				&& FPaths::IsSamePath(ScriptDirectory, Item->Directory / TEXT("Script"));
			const TCHAR* Trust = TEXT("Not verified");
			if (bKnown && (FModKitModRegistry::IsModActivated(Item->Id)
				|| FModKitModRegistry::IsPlayerModApprovalPending(Item->Id)))
			{
				switch (FModKitModRegistry::GetModTrust(Item->Id))
				{
				case EModKitModTrust::BuiltIn: Trust = TEXT("Built-in"); break;
				case EModKitModTrust::Official: Trust = TEXT("Official"); break;
				case EModKitModTrust::Player: Trust = TEXT("Player"); break;
				}
			}
			const TCHAR* State = !bKnown ? TEXT("Not registered") : FModKitModRegistry::IsPlayerModApprovalPending(Item->Id)
				? TEXT("Awaiting player confirmation")
				: (FModKitModRegistry::IsModActivated(Item->Id) ? TEXT("Active") : TEXT("Inactive"));
			return AsText(FString::Printf(TEXT("%s | %s | Leaf | %s | %s | %s | %s | %s"),
				*Item->Id, *LocationName(Item->Location), (bKnown && FModKitModRegistry::IsModActivated(Item->Id)) ? TEXT("Yes") : TEXT("No"),
				Item->Version.IsEmpty() ? TEXT("—") : *Item->Version, *Signature, Trust, State));
		}).ToolTipText(AsText(Item->Directory))
	];
}

void SFeatureModManagerWidget::OnSelectionChanged(FFeatureModItem Item, ESelectInfo::Type SelectInfo)
{
	SelectedFeatureModId = Item.IsValid() ? Item->Id : FString();
	SelectedManifestPath = Item.IsValid() ? Item->ManifestPath : FString();
	UpdateDetails();
}

void SFeatureModManagerWidget::RefreshCatalog(bool bPreserveEdits)
{
	const FString EditedJson = ManifestEditor.IsValid() ? ManifestEditor->GetText().ToString() : FString();
	const UFeatureModSettings* Settings = UFeatureModSettings::Get();
	FFeatureModCatalog::Discover(FPaths::ProjectDir(), Settings->AllowedRoots, FeatureMods, Plugins);
	if (bPreserveEdits && ManifestEditor.IsValid())
	{
		for (FFeatureModInfo& FeatureMod : FeatureMods)
		{
			if (FeatureMod.Id == SelectedFeatureModId)
				FeatureMod.ManifestText = EditedJson;
		}
		FFeatureModCatalog::Validate(FeatureMods, Plugins);
	}
	AddWhitelistDiagnostics(FeatureMods);
	FeatureModItems.Reset();
	for (const FFeatureModInfo& FeatureMod : FeatureMods)
	{
		FeatureModItems.Add(MakeShared<FFeatureModInfo>(FeatureMod));
	}
	if (FeatureModList.IsValid())
	{
		FeatureModList->RequestListRefresh();
	}
	if (DependencyView.IsValid())
	{
		DependencyView->SetText(AsText(FFeatureModCatalog::BuildDependencyView(FeatureMods, Plugins)));
	}
	UpdateDetails();
	if (DiagnosticView.IsValid())
	{
		DiagnosticView->SetText(AsText(BuildDiagnosticText()));
	}
}

void SFeatureModManagerWidget::AddWhitelistDiagnostics(TArray<FFeatureModInfo>& Mods) const
{
	const UModKitSettings* Allowlist = UModKitSettings::Get();
	for (FFeatureModInfo& FeatureMod : Mods)
	{
		FFeatureModCatalog::CheckWhitelistCapabilities(FeatureMod, *Allowlist);
	}
}

void SFeatureModManagerWidget::UpdateDetails()
{
	UpdateDependencyChoices();
	if (!ManifestEditor.IsValid() || !SelectedTitle.IsValid())
	{
		return;
	}
	for (const FFeatureModInfo& FeatureMod : FeatureMods)
	{
		if (FeatureMod.Id == SelectedFeatureModId)
		{
			SelectedTitle->SetText(AsText(FString::Printf(TEXT("%s — %s — %s"), *FeatureMod.DisplayName,
				*LocationName(FeatureMod.Location), *FeatureMod.Id)));
			ManifestEditor->SetText(AsText(FeatureMod.ManifestText));
			if (DiagnosticView.IsValid())
			{
				DiagnosticView->SetText(AsText(BuildDiagnosticText()));
			}
			return;
		}
	}
	SelectedManifestPath.Reset();
	SelectedTitle->SetText(AsText(TEXT("Select a FeatureMod")));
	ManifestEditor->SetText(FText::GetEmpty());
	if (DiagnosticView.IsValid())
	{
		DiagnosticView->SetText(AsText(BuildDiagnosticText()));
	}
}

void SFeatureModManagerWidget::UpdateDependencyChoices()
{
	if (!DependencyChoices.IsValid()) return;
	DependencyChoices->ClearChildren();
	if (SelectedFeatureModId.IsEmpty()) return;
	auto AddChoice = [this](const FString& Kind, const FString& Id)
	{
		DependencyChoices->AddSlot().AutoHeight()
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this, Kind, Id]
			{
				const auto& Values = Kind == TEXT("trunkModules") ? EditedManifest.TrunkModules
					: Kind == TEXT("branches") ? EditedManifest.Branches : EditedManifest.FeatureMods;
				return Values.ContainsByPredicate([&Id](const FString& Value)
				{
					return Value.Equals(Id, ESearchCase::IgnoreCase);
				}) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, Kind, Id](ECheckBoxState State)
			{
				SetDependency(Kind, Id, State == ECheckBoxState::Checked);
			})
			[SNew(STextBlock).Text(AsText(Kind + TEXT(" / ") + Id))]
		];
	};
	AddChoice(TEXT("trunkModules"), TEXT("Project"));
	for (const auto& Plugin : Plugins)
	{
		if (!Plugin.bFeatureMod)
			AddChoice(Plugin.bGameFeature ? TEXT("branches") : TEXT("trunkModules"), Plugin.Id);
	}
	for (const auto& Mod : FeatureMods)
		if (Mod.Id != SelectedFeatureModId) AddChoice(TEXT("featureMods"), Mod.Id);
}

void SFeatureModManagerWidget::SetDependency(const FString& Kind, const FString& Id, bool bEnabled)
{
	FString Json;
	TArray<FString> Errors;
	if (!FFeatureModCatalog::SetDependency(ManifestEditor->GetText().ToString(), Kind, Id, bEnabled,
		SelectedFeatureModId, FeatureMods, Plugins, Json, Errors))
	{
		DiagnosticView->SetText(AsText(FString::Join(Errors, TEXT("\n"))));
		return;
	}
	ManifestEditor->SetText(AsText(Json));
	RunCheck();
}

void SFeatureModManagerWidget::SelectFeatureMod(const FString& Id)
{
	for (const FFeatureModItem& Item : FeatureModItems)
	{
		if (Item->Id.Equals(Id, ESearchCase::IgnoreCase))
		{
			FeatureModList->SetSelection(Item);
			OnSelectionChanged(Item, ESelectInfo::Direct);
			return;
		}
	}
}

FReply SFeatureModManagerWidget::CreateFeatureMod()
{
	const UFeatureModSettings* Settings = UFeatureModSettings::Get();
	if (Settings->AllowedRoots.IsEmpty())
	{
		DiagnosticView->SetText(AsText(TEXT("Add at least one project-relative root in Project Settings → FeatureMods.")));
		return FReply::Handled();
	}

	TArray<TSharedPtr<FString>> RootOptions;
	for (const FString& Root : Settings->AllowedRoots)
	{
		RootOptions.Add(MakeShared<FString>(Root));
	}
	TSharedPtr<FString> SelectedRoot = RootOptions[0];
	TSharedRef<SEditableTextBox> IdInput = SNew(SEditableTextBox).Text(AsText(TEXT("NewFeatureMod")));
	TSharedRef<STextBlock> ErrorText = SNew(STextBlock).AutoWrapText(true);
	TSharedPtr<SWindow> DialogWindow;
	TWeakPtr<SFeatureModManagerWidget> WeakWidget = SharedThis(this);

	TSharedRef<SWindow> Dialog = SNew(SWindow)
		.Title(AsText(TEXT("Create FeatureMod")))
		.ClientSize(FVector2D(480.0f, 220.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
			[
				SNew(STextBlock).Text(AsText(TEXT("Stable ID")))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
			[
				IdInput
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
			[
				SNew(STextBlock).Text(AsText(TEXT("Install root")))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&RootOptions)
				.InitiallySelectedItem(SelectedRoot)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
				{
					return SNew(STextBlock).Text(Item.IsValid() ? AsText(*Item) : FText::GetEmpty());
				})
				.OnSelectionChanged_Lambda([&SelectedRoot](TSharedPtr<FString> Item, ESelectInfo::Type)
				{
					if (Item.IsValid())
					{
						SelectedRoot = Item;
					}
				})
				[
					SNew(STextBlock).Text_Lambda([&SelectedRoot]()
					{
						return SelectedRoot.IsValid() ? AsText(*SelectedRoot) : FText::GetEmpty();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(8.0f)
			[
				ErrorText
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
				[
					SNew(SButton).Text(AsText(TEXT("Create"))).OnClicked_Lambda([WeakWidget, IdInput, &SelectedRoot, ErrorText, &DialogWindow]()
					{
						if (TSharedPtr<SFeatureModManagerWidget> Widget = WeakWidget.Pin())
						{
							FString Directory;
							FString Error;
							if (!FFeatureModCatalog::CreateFromTemplate(FPaths::ProjectDir(), *SelectedRoot,
								IdInput->GetText().ToString(), Directory, Error))
							{
								ErrorText->SetText(AsText(Error));
								return FReply::Handled();
							}
							Widget->RefreshCatalog();
							Widget->SelectFeatureMod(IdInput->GetText().ToString());
							if (DialogWindow.IsValid())
							{
								DialogWindow->RequestDestroyWindow();
							}
							return FReply::Handled();
						}
						return FReply::Unhandled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
				[
					SNew(SButton).Text(AsText(TEXT("Cancel"))).OnClicked_Lambda([&DialogWindow]()
					{
						if (DialogWindow.IsValid())
						{
							DialogWindow->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
			]
		];
	DialogWindow = Dialog;
	FSlateApplication::Get().AddModalWindow(Dialog, FGlobalTabmanager::Get()->GetRootWindow());
	return FReply::Handled();
}

FReply SFeatureModManagerWidget::RequestSelected(bool bLoad)
{
	if (!CanOperateSelected(bLoad) || FModKitRuntimeOps::IsOperationPending(SelectedFeatureModId))
		return FReply::Handled();
	if (bLoad)
	{
		FString SavedJson;
		if (!FFileHelper::LoadFileToString(SavedJson, *SelectedManifestPath)
			|| SavedJson != ManifestEditor->GetText().ToString())
		{
			DiagnosticView->SetText(AsText(TEXT("Save FeatureMod.json before loading to apply dependency changes.")));
			return FReply::Handled();
		}
		FModKitRuntimeOps::RequestLoad(SelectedFeatureModId);
	}
	else FModKitRuntimeOps::RequestUnload(SelectedFeatureModId);
	return FReply::Handled();
}

FReply SFeatureModManagerWidget::SaveManifest()
{
	if (SelectedFeatureModId.IsEmpty() || SelectedManifestPath.IsEmpty() || !ManifestEditor.IsValid())
	{
		return FReply::Handled();
	}
	const FString Json = ManifestEditor->GetText().ToString();
	TArray<FFeatureModInfo> Candidate = FeatureMods;
	for (FFeatureModInfo& FeatureMod : Candidate)
	{
		if (FeatureMod.Id == SelectedFeatureModId)
		{
			FeatureMod.ManifestText = Json;
			break;
		}
	}
	FFeatureModCatalog::Validate(Candidate, Plugins);
	AddWhitelistDiagnostics(Candidate);
	const int32 CandidateIndex = Candidate.IndexOfByPredicate([this](const FFeatureModInfo& FeatureMod)
	{
		return FeatureMod.Id == SelectedFeatureModId;
	});
	if (CandidateIndex == INDEX_NONE || !Candidate[CandidateIndex].Diagnostics.IsEmpty())
	{
		if (DiagnosticView.IsValid())
		{
			DiagnosticView->SetText(AsText(CandidateIndex == INDEX_NONE
				? TEXT("Selected FeatureMod was not found")
				: FString::Join(Candidate[CandidateIndex].Diagnostics, TEXT("\n"))));
		}
		return FReply::Handled();
	}

	TArray<FString> Errors;
	if (!FFeatureModCatalog::SaveManifest(SelectedManifestPath, Json, SelectedFeatureModId,
		FeatureMods, Plugins, Errors))
	{
		DiagnosticView->SetText(AsText(FString::Join(Errors, TEXT("\n"))));
		return FReply::Handled();
	}
	RefreshCatalog();
	SelectFeatureMod(SelectedFeatureModId);
	DiagnosticView->SetText(AsText(TEXT("FeatureMod.json saved and validated.")));
	return FReply::Handled();
}

FReply SFeatureModManagerWidget::RunCheck()
{
	RefreshCatalog(true);
	if (DependencyView.IsValid())
	{
		DependencyView->SetText(AsText(FFeatureModCatalog::BuildDependencyView(FeatureMods, Plugins)));
	}
	if (DiagnosticView.IsValid())
	{
		DiagnosticView->SetText(AsText(BuildDiagnosticText()));
	}
	return FReply::Handled();
}

FReply SFeatureModManagerWidget::OpenSettings()
{
	FFeatureModEditor::OpenSettings();
	return FReply::Handled();
}

bool SFeatureModManagerWidget::CanSaveManifest() const
{
	return !SelectedFeatureModId.IsEmpty() && !SelectedManifestPath.IsEmpty() && ManifestEditor.IsValid();
}

FString SFeatureModManagerWidget::BuildDiagnosticText() const
{
	TArray<FString> Lines;
	if (UFeatureModSettings::Get()->AllowedRoots.IsEmpty())
		Lines.Add(TEXT("No catalog roots are enabled. Runtime discovery is unchanged."));
	for (const FString& Root : UFeatureModSettings::Get()->AllowedRoots)
	{
		if (!FFeatureModCatalog::IsSupportedRoot(Root))
			Lines.Add(TEXT("Unsupported root: ") + Root + TEXT(". Use Plugins/FeatureMods or FeatureMods."));
	}
	for (const FFeatureModInfo& FeatureMod : FeatureMods)
	{
		for (const FString& Diagnostic : FeatureMod.Diagnostics)
		{
			Lines.Add(FString::Printf(TEXT("%s: %s"), *FeatureMod.Id, *Diagnostic));
		}
	}
	return (Lines.IsEmpty() ? TEXT("FeatureMod directory, descriptor, dependency and capability checks passed.")
		: FString::Join(Lines, TEXT("\n")))
		+ FString(TEXT("\nFeatureMods can deliver source or a prepared module cache set. Configured capability names do not replace runtime API validation. Package signatures are verified when loading. Runtime policy changes require a restart."));
}

bool SFeatureModManagerWidget::CanReloadSelected() const
{
	return UFeatureModSettings::Get()->bEnableDevelopmentHotReload
		&& CanOperateSelected() && FModKitRuntimeOps::CanReload(SelectedFeatureModId);
}

bool SFeatureModManagerWidget::CanOperateSelected(bool bRequireValidManifest) const
{
	return GEditor && !GEditor->PlayWorld && !GEditor->bIsSimulatingInEditor
		&& FeatureMods.ContainsByPredicate([this, bRequireValidManifest](const FFeatureModInfo& Item)
		{
			if (Item.Id != SelectedFeatureModId || (bRequireValidManifest && !Item.Diagnostics.IsEmpty()))
				return false;
			FString ScriptDirectory, RelativeRoot;
			if (!FModKitModRegistry::GetModScriptPaths(Item.Id, ScriptDirectory, RelativeRoot))
				return true;
			return FPaths::IsSamePath(ScriptDirectory, Item.Directory / TEXT("Script"));
		});
}

FReply SFeatureModManagerWidget::OpenPackaging()
{
	if (GEditor && (GEditor->PlayWorld || GEditor->bIsSimulatingInEditor))
	{
		DiagnosticView->SetText(AsText(TEXT("Stop PIE or simulation before assembling.")));
		return FReply::Handled();
	}
	const FFeatureModInfo* Selected = FeatureMods.FindByPredicate([this](const FFeatureModInfo& Item) { return Item.Id == SelectedFeatureModId; });
	if (!Selected || !CanOperateSelected()) return FReply::Handled();
    if (ManifestEditor->GetText().ToString() != Selected->ManifestText)
    {
        DiagnosticView->SetText(AsText(TEXT("Save FeatureMod.json before assembling.")));
        return FReply::Handled();
    }
	const FString Output = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("FeatureModPackages")
		/ (Selected->Id + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const UModKitSettings* Settings = UModKitSettings::Get();
	FString Params = FString::Printf(TEXT("-Source=\"%s\" -Output=\"%s\""), *Selected->Directory, *Output);
	if (!Settings->PatchPublicKeyBase64.IsEmpty())
		Params += FString::Printf(TEXT(" -Signer=\"%s\" -PublicKey=\"%s\""), *Settings->PatchVerifyKeyId, *Settings->PatchPublicKeyBase64);
	UModKitAssembleCommandlet* Assemble = NewObject<UModKitAssembleCommandlet>();
	const int32 Result = Assemble->Main(Params);
	DiagnosticView->SetText(AsText(Result == 0
		? TEXT("Assembled: ") + Output + (Settings->PatchPublicKeyBase64.IsEmpty()
            ? TEXT("\nUnsigned player delivery. Copy the Mod directory to the external FeatureMods root; whitelist and first approval still apply.")
            : TEXT("\nFor official delivery, sign the .canonical bytes with your external Ed25519 tool, fill signature.value, then run ModKitPatchVerify -Kind=package before deployment."))
		: TEXT("Assembly failed. Check the Output Log; the output directory must be new.")));
	return FReply::Handled();
}
