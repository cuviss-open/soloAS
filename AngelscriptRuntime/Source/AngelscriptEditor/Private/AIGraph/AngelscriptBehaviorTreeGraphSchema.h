// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "EdGraphSchema_BehaviorTreeDecorator.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_Task.h"

#include "AngelscriptBehaviorTreeGraphSchema.generated.h"

class UBehaviorTreeGraph;
class UBehaviorTreeDecoratorGraph;
class UEdGraph;
class UEdGraphSchema;
class UPackage;
class FObjectPostSaveContext;
class FObjectPreSaveContext;

/** Stock Behavior Tree schema with an explicit script-class action supplement. */
UCLASS()
class UAngelscriptBehaviorTreeGraphSchema : public UEdGraphSchema_BehaviorTree
{
	GENERATED_UCLASS_BODY()

	virtual void GetGraphContextActions(FGraphContextMenuBuilder& ContextMenuBuilder) const override;
	virtual void GetSubNodeClasses(
		int32 SubNodeFlags,
		TArray<FGraphNodeClassData>& ClassData,
		UClass*& GraphNodeClass) const override;
};

/** Stock decorator schema with an explicit script-class action supplement. */
UCLASS()
class UAngelscriptBehaviorTreeDecoratorGraphSchema : public UEdGraphSchema_BehaviorTreeDecorator
{
	GENERATED_UCLASS_BODY()

	virtual void GetGraphContextActions(FGraphContextMenuBuilder& ContextMenuBuilder) const override;
};

UCLASS()
class UAngelscriptBehaviorTreeGraphNodeComposite : public UBehaviorTreeGraphNode_Composite
{
	GENERATED_BODY()

public:
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
};

UCLASS()
class UAngelscriptBehaviorTreeGraphNodeTask : public UBehaviorTreeGraphNode_Task
{
	GENERATED_BODY()

public:
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
};

UCLASS()
class UAngelscriptBehaviorTreeGraphNodeDecorator : public UBehaviorTreeGraphNode_Decorator
{
	GENERATED_BODY()

public:
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
};

UCLASS()
class UAngelscriptBehaviorTreeGraphNodeService : public UBehaviorTreeGraphNode_Service
{
	GENERATED_BODY()

public:
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
};

/** Installs the schemas only while editing, and removes them before package serialization. */
UCLASS()
class UAngelscriptAIGraphSchemaSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void OnAssetEditorRequestedOpen(UObject* Asset);
	void OnAssetEditorOpened(UObject* Asset);
	void OnObjectConstructed(UObject* Object);
	void OnObjectPreSave(UObject* Object, FObjectPreSaveContext SaveContext);
	void OnPreSavePackage(UPackage* Package, FObjectPreSaveContext SaveContext);
	void OnPackageSaved(const FString& PackageFileName, UPackage* Package, FObjectPostSaveContext SaveContext);

	void InstallGraphsForAsset(UObject* Asset);
	void InstallLoadedGraphs(UPackage* Package = nullptr);
	void RestoreLoadedGraphs(UPackage* Package = nullptr);
	void InstallGraph(UEdGraph* Graph);
	void SetGraphSchema(UEdGraph* Graph, UClass* SchemaClass);
	void EnsureDeferredInstallTicker();
	bool FlushDeferredInstalls(float);

	FDelegateHandle AssetEditorRequestedOpenHandle;
	FDelegateHandle AssetEditorOpenedHandle;
	FDelegateHandle ObjectConstructedHandle;
	FDelegateHandle ObjectPreSaveHandle;
	FDelegateHandle PreSavePackageHandle;
	FDelegateHandle PackageSavedHandle;
	TSubclassOf<UEdGraphSchema> StockBehaviorTreeSchema;
	TSubclassOf<UEdGraphSchema> StockDecoratorSchema;
	TSubclassOf<UEdGraphSchema> ProductBehaviorTreeSchema;
	TSubclassOf<UEdGraphSchema> ProductDecoratorSchema;
	TSet<TWeakObjectPtr<UEdGraph>> InstalledGraphs;
	FTSTicker::FDelegateHandle DeferredInstallTickerHandle;
	TSet<TWeakObjectPtr<UEdGraph>> PendingGraphs;
	TSet<TWeakObjectPtr<UPackage>> PendingPackages;
};
