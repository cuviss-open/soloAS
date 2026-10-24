#pragma once

#include "Modules/ModuleManager.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "UObject/ObjectRedirector.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"

#include "AssetToolsStatics.generated.h"

USTRUCT(BlueprintType)
struct FSaveAssetDialogSettings
{
	GENERATED_BODY()

	UPROPERTY(ScriptReadWrite)
	FText DialogTitleOverride;
	UPROPERTY(ScriptReadWrite)
	FString DefaultPath;
	UPROPERTY(ScriptReadWrite)
	FString DefaultAssetName;
	UPROPERTY(ScriptReadWrite)
	FVector2D WindowSizeOverride = FVector2D::ZeroVector;
	UPROPERTY(ScriptReadWrite)
	TArray<FTopLevelAssetPath> AssetClassNames;
	UPROPERTY(ScriptReadWrite)
	bool bAllowOverwriteExisting = false;
};

/**
 * Creates an AssetTools:: namespace in script with static functions that
 * aren't exposed to Blueprint, and therefore not bound by Angelscript.
 */
UCLASS()
class UAssetToolsStatics : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Creates an asset with the specified name, path, and optional factory
	 *
	 * @param AssetName the name of the new asset
	 * @param PackagePath the package that will contain the new asset
	 * @param AssetClass the class of the new asset
	 * @param Factory optional factory that will build the new asset
	 * @param CallingContext optional name of the module or method calling CreateAsset() - this is passed to the factory
	 * @return the new asset or nullptr if it fails
	 */
	UFUNCTION(ScriptCallable, Meta = (DeterminesOutputType = "AssetClass"))
	static UObject* CreateAsset(const FString& AssetName, const FString& PackagePath, UClass* AssetClass, UFactory* Factory = nullptr, FName CallingContext = NAME_None)
	{
		FAssetToolsModule& AssetToolsModule = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools");
		return AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, AssetClass, Factory, CallingContext);
	}

	/**
	 * Fix up references to the specified redirectors.
	 * 
	 * @param bCheckoutDialogPrompt indicates whether to prompt the user with files checkout dialog or silently attempt to checkout all necessary files.
	 */
	UFUNCTION(ScriptCallable)
	static void FixupReferencers(const TArray<UObject*>& Objects, bool bCheckoutDialogPrompt, ERedirectFixupMode FixupMode)
	{
		// Transform Objects array to ObjectRedirectors array
		TArray<UObjectRedirector*> Redirectors;
		for (UObject* Object : Objects)
		{
			if (Object != nullptr && Object->IsA<UObjectRedirector>())
			{
				Redirectors.Add(CastChecked<UObjectRedirector>(Object));			
			}
		}

		FAssetToolsModule& AssetToolsModule = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools");
		return AssetToolsModule.Get().FixupReferencers(Redirectors, bCheckoutDialogPrompt, FixupMode);
	}

	/** Returns whether redirectors are being fixed up. */
	UFUNCTION(ScriptCallable)
	bool IsFixupReferencersInProgress() const
	{
		FAssetToolsModule& AssetToolsModule = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools");
		return AssetToolsModule.Get().IsFixupReferencersInProgress();
	}

	/**
	 * Imports assets using tasks specified.
	 * 
	 * @param ImportTasks Tasks that specify how to import each file
	 */
	UFUNCTION(ScriptCallable)
	static void ImportAssetTasks(const TArray<UAssetImportTask*>& ImportTasks)
	{
		FAssetToolsModule& AssetToolsModule = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools");
		return AssetToolsModule.Get().ImportAssetTasks(ImportTasks);
	}

	/**
	 * Open an editor dialog to choose a location to save an asset.
	 *
	 * If the dialog is cancelled, the returned package name will be empty.
	 */
	UFUNCTION(ScriptCallable)
	static FString OpenSaveAssetDialog(const FSaveAssetDialogSettings& DialogSettings)
	{
		FSaveAssetDialogConfig Config;
		Config.DialogTitleOverride = DialogSettings.DialogTitleOverride;
		Config.AssetClassNames = DialogSettings.AssetClassNames;
		Config.WindowSizeOverride = DialogSettings.WindowSizeOverride;
		Config.DefaultPath = DialogSettings.DefaultPath;
		Config.DefaultAssetName = DialogSettings.DefaultAssetName;

		if (DialogSettings.bAllowOverwriteExisting)
			Config.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::Disallow;
		else
			Config.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::AllowButWarn;

		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
		return ContentBrowserModule.Get().CreateModalSaveAssetDialog(Config);
	}
};
