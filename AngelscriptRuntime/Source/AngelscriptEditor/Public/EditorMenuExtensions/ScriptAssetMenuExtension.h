#pragma once
#include "ScriptEditorMenuExtension.h"
#include "Templates/SubclassOf.h"
#include "ScriptAssetMenuExtension.generated.h"

UCLASS(BlueprintType)
class UScriptAssetMenuExtension : public UScriptEditorMenuExtension
{
	GENERATED_BODY()
public:

	UScriptAssetMenuExtension()
	{
		ExtensionMenu = EScriptEditorMenuExtensionLocation::ContentBrowser_AssetViewContextMenu;
		ExtensionPoint = "CommonAssetActions";
	}

	UPROPERTY(EditDefaultsOnly, Category = "Menu Extension")
	TArray<TSubclassOf<UObject>> SupportedClasses;

	UFUNCTION(BlueprintNativeEvent)
	bool SupportsAsset(FAssetData AssetData) const;
	bool SupportsAsset_Implementation(FAssetData AssetData) const { return true; }

	UFUNCTION(ScriptCallable, Meta = (NotAngelscriptProperty))
	FAssetData GetSelectedAsset() const
	{
		if (!CurrentSelection.SelectedAssets.IsEmpty())
			return CurrentSelection.SelectedAssets[0];
		return FAssetData();
	}

	UFUNCTION(ScriptCallable, Meta = (NotAngelscriptProperty))
	const TArray<FAssetData>& GetSelectedAssets() const
	{
		return CurrentSelection.SelectedAssets;
	}

	virtual TArray<UFunction*> GatherExtensionFunctions() const;
	virtual void CallFunctionOnSelection(UFunction* Function, FExtenderSelection Selection) const;
};