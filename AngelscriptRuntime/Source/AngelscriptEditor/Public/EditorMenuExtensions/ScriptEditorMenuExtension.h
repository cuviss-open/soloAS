#pragma once
#include "CoreMinimal.h"
#include "Framework/Commands/UICommandList.h"
#include "AssetRegistry/AssetData.h"
#include "ToolMenuDelegates.h"
#include "ToolMenuSection.h"
#include "ScriptEditorMenuExtension.generated.h"

UENUM(BlueprintType)
enum class EScriptEditorMenuExtensionLocation : uint8
{
	ToolMenu,
	LevelViewport_ContextMenu,
	LevelViewport_DragDropContextMenu,
	LevelViewport_OptionsMenu,
	LevelViewport_ShowMenu,
	LevelViewport_ViewMenu,
	LevelViewport_BuildMenu,
	LevelViewport_CompileMenu,
	LevelViewport_SourceControlMenu,
	LevelViewport_CreateMenu,
	LevelViewport_PlayMenu,
	LevelViewport_BlueprintsMenu,
	LevelViewport_CinematicsMenu,
	LevelViewport_LevelMenu,
	ContentBrowser_AssetContextMenu,
	ContentBrowser_PathViewContextMenu,
	ContentBrowser_CollectionListContextMenu,
	ContentBrowser_CollectionViewContextMenu,
	ContentBrowser_AssetViewContextMenu,
	ContentBrowser_AssetViewViewMenu,
};

UENUM(BlueprintType)
enum class EScriptEditorMenuExtensionOrder : uint8
{
	Before,
	After,
	First,
};

UCLASS(BlueprintType)
class UScriptEditorMenuExtension : public UObject
{
	GENERATED_BODY()

public:

	virtual UWorld* GetWorld() const override;

	UPROPERTY(EditDefaultsOnly, Category = "Menu Extension")
	EScriptEditorMenuExtensionLocation ExtensionMenu = EScriptEditorMenuExtensionLocation::ToolMenu;

	UPROPERTY(EditDefaultsOnly, Category = "Menu Extension")
	FName ExtensionPoint = "MainFrame.MainMenu.Tools";

	UPROPERTY(EditDefaultsOnly, Category = "Menu Extension")
	EScriptEditorMenuExtensionOrder ExtensionOrder = EScriptEditorMenuExtensionOrder::After;

	// For tool menus, the position within the section to insert
	UPROPERTY(EditDefaultsOnly, Category = "Menu Extension")
	EToolMenuInsertType ToolMenuInsertType = EToolMenuInsertType::Default;

	// For tool menus, the alignment of the section within the parent
	UPROPERTY(EditDefaultsOnly, Category = "Menu Extension")
	EToolMenuSectionAlign ToolMenuSectionAlign = EToolMenuSectionAlign::Default;

	// For tool menus, which element to insert relative to
	UPROPERTY(EditDefaultsOnly, Category = "Menu Extension")
	FName ToolMenuInsertPosition;

	UPROPERTY(EditDefaultsOnly, Category = "Menu Extension")
	FText MenuSectionHeader;

	UPROPERTY(EditDefaultsOnly, Category = "Menu Extension")
	bool bAddSeparatorBeforeOptions = false;

	UPROPERTY(EditDefaultsOnly, Category = "Menu Extension")
	bool bAddSeparatorAfterOptions = false;

	/**
	 * Whether the extension should be active and extend the targeted menu.
	 * This is called every time the extended menu is generated.
	 */
	UFUNCTION(BlueprintNativeEvent)
	bool ShouldExtend() const;
	bool ShouldExtend_Implementation() const { return true; }

	/**
	 * Whether to register the extension to the extended menu for evaluation.
	 * This is called at startup and when changing maps in the editor, before menus are generated.
	 */
	UFUNCTION(BlueprintNativeEvent)
	bool ShouldRegisterExtension() const;
	bool ShouldRegisterExtension_Implementation() const { return true; }

	/**
	 * If the extender is part of a ToolMenu, retrieve part of that tool menu's context.
	 * Will return null if not a ToolMenu.
	 */
	UFUNCTION(ScriptCallable, Meta = (DeterminesOutputType="ContextClass"))
	UObject* GetToolMenuContext(TSubclassOf<UObject> ContextClass) const;

	static void InitializeExtensions();
	static void RefreshExtensionRegistration();

protected:

	struct FExtenderSelection
	{
		TArray<TWeakObjectPtr<UObject>> SelectedObjects;
		TArray<FAssetData> SelectedAssets;
	};

	mutable FExtenderSelection CurrentSelection;

	virtual TArray<UFunction*> GatherExtensionFunctions() const;
	virtual void CallFunctionOnSelection(UFunction* Function, FExtenderSelection Selection) const;

	virtual FName GetExtensionPointOrDefault() const;

private:

	struct FRegisteredExtender
	{
		EScriptEditorMenuExtensionLocation Location;
		FDelegateHandle DelegateHandle;
		TSharedPtr<class FExtender> Extender;
		FName ExtensionPoint;
		FName SectionName;
	};

	static TMap<class UASClass*, FRegisteredExtender> RegisteredExtensions;
	mutable const struct FToolMenuContext* ActiveToolMenuContext = nullptr;

	TSharedRef<class FExtender> Extend(const TSharedRef<FUICommandList> CommandList, FExtenderSelection Selection) const;
	void BuildMenu(class FMenuBuilder& MenuBuilder, TArray<UFunction*> Functions, FExtenderSelection Selection) const;
	void AddMenuEntry(class FMenuBuilder& MenuBuilder, UFunction* Function, FExtenderSelection Selection) const;

	void BuildToolMenuSection(struct FToolMenuSection& MenuSection, bool bIsMenu) const;
	void AddToolMenuEntry(struct FToolMenuSection& MenuSection, UFunction* Function) const;
	void AddToolMenuButton(struct FToolMenuSection& MenuSection, UFunction* Function) const;

	FUIAction CreateUIAction(UFunction* Function, FExtenderSelection Selection) const;
	FToolUIAction CreateToolUIAction(UFunction* Function) const;
	FExtenderSelection GetSelectionFromToolMenuContext(const FToolMenuContext& Context) const;

	static void UnregisterExtensions();
	static void RegisterExtensions();

	static void RegisterExtension(UASClass* Class);
	static void UnregisterExtension(FRegisteredExtender& Extension);

};
