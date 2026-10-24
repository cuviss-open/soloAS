// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptBehaviorTreeGraphSchema.h"

#include "AngelscriptAIGraphClassProvider.h"
#include "AngelscriptScriptNodeMenuTitle.h"
#include "AIGraphSchema.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTreeEditorTypes.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_SubtreeTask.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "ClassGenerator/ASClass.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Containers/Ticker.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"

namespace
{
UClass* ResolveClass(const FGraphNodeClassData& Data)
{
	FGraphNodeClassData MutableData = Data;
	return MutableData.GetClass(true);
}

bool HasClassData(const TArray<FGraphNodeClassData>& Existing, const UClass* Candidate)
{
	if (Candidate == nullptr)
	{
		return true;
	}

	const FString CandidatePath = Candidate->GetPathName();
	for (const FGraphNodeClassData& ExistingData : Existing)
	{
		if (UClass* ExistingClass = ResolveClass(ExistingData))
		{
			if (ExistingClass == Candidate || ExistingClass->GetPathName() == CandidatePath)
			{
				return true;
			}
		}
	}
	return false;
}

bool IsUsableClassData(const FGraphNodeClassData& Data)
{
	return !Data.IsAbstract() && !Data.IsDeprecated() && !Data.bIsHidden;
}

FString GetHazeScriptBehaviorTreeNodeName(const UClass* Class)
{
	FString ShortName = Class->GetName();
	const int32 PrefixSeparator = ShortName.Find(TEXT("_"), ESearchCase::CaseSensitive);
	if (PrefixSeparator != INDEX_NONE)
	{
		ShortName.MidInline(PrefixSeparator + 1, MAX_int32, EAllowShrinking::No);
	}
	return ShortName;
}

FText GetHazeScriptBehaviorTreeNodeTitle(const UBTNode* Node)
{
	if (Node == nullptr || Cast<UASClass>(Node->GetClass()) == nullptr || !Node->NodeName.IsEmpty())
	{
		return FText::GetEmpty();
	}
	return FText::FromString(GetHazeScriptBehaviorTreeNodeName(Node->GetClass()));
}

bool ResolveAllowedNodeFamilies(
	const UEdGraphSchema_BehaviorTree& StockSchema,
	const UEdGraphPin* FromPin,
	bool& bAllowComposites,
	bool& bAllowTasks)
{
	bAllowComposites = false;
	bAllowTasks = false;
	if (FromPin == nullptr)
	{
		bAllowComposites = true;
		bAllowTasks = true;
		return true;
	}

	const UBehaviorTreeGraphNode* Owner = Cast<UBehaviorTreeGraphNode>(FromPin->GetOwningNode());
	if (Owner == nullptr || (FromPin->Direction != EGPD_Input && FromPin->Direction != EGPD_Output))
	{
		return false;
	}

	UClass* ProxyClass = UBehaviorTreeGraphNode::StaticClass();
	if (Owner->IsA<UBehaviorTreeGraphNode_Task>())
	{
		ProxyClass = UBehaviorTreeGraphNode_Task::StaticClass();
	}
	else if (Owner->IsA<UBehaviorTreeGraphNode_Composite>())
	{
		ProxyClass = UBehaviorTreeGraphNode_Composite::StaticClass();
	}

	UBehaviorTreeGraphNode* Proxy = NewObject<UBehaviorTreeGraphNode>(
		GetTransientPackage(), ProxyClass, NAME_None, RF_Transient);
	UBehaviorTreeGraphNode* CompositeProbe = NewObject<UBehaviorTreeGraphNode>(
		GetTransientPackage(), UBehaviorTreeGraphNode_Composite::StaticClass(), NAME_None, RF_Transient);
	UBehaviorTreeGraphNode* TaskProbe = NewObject<UBehaviorTreeGraphNode>(
		GetTransientPackage(), UBehaviorTreeGraphNode_Task::StaticClass(), NAME_None, RF_Transient);
	if (Proxy == nullptr || CompositeProbe == nullptr || TaskProbe == nullptr)
	{
		return false;
	}

	const EEdGraphPinDirection ProbeDirection = FromPin->Direction == EGPD_Input ? EGPD_Output : EGPD_Input;
	UEdGraphPin* ProxyPin = Proxy->CreatePin(FromPin->Direction, FromPin->PinType, NAME_None);
	UEdGraphPin* CompositePin = CompositeProbe->CreatePin(ProbeDirection, NAME_None, NAME_None);
	UEdGraphPin* TaskPin = TaskProbe->CreatePin(ProbeDirection, NAME_None, NAME_None);
	if (ProxyPin == nullptr || CompositePin == nullptr || TaskPin == nullptr)
	{
		return false;
	}

	const FPinConnectionResponse CompositeResponse =
		StockSchema.UEdGraphSchema_BehaviorTree::CanCreateConnection(ProxyPin, CompositePin);
	const FPinConnectionResponse TaskResponse =
		StockSchema.UEdGraphSchema_BehaviorTree::CanCreateConnection(ProxyPin, TaskPin);
	auto ReadResponse = [](const FPinConnectionResponse& Response, bool& bAllowed)
	{
		if (Response.Response == CONNECT_RESPONSE_MAKE)
		{
			bAllowed = true;
			return true;
		}
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			bAllowed = false;
			return true;
		}
		return false;
	};
	return ReadResponse(CompositeResponse, bAllowComposites)
		&& ReadResponse(TaskResponse, bAllowTasks)
		&& (bAllowComposites || bAllowTasks);
}

void AppendUniqueClassData(TArray<FGraphNodeClassData>& Existing, const TArray<FGraphNodeClassData>& Supplement)
{
	for (const FGraphNodeClassData& Data : Supplement)
	{
		if (!IsUsableClassData(Data))
		{
			continue;
		}
		if (UClass* Class = ResolveClass(Data); Class != nullptr && !HasClassData(Existing, Class))
		{
			Existing.Add(Data);
		}
	}
}

void AddMainGraphActions(
	FGraphContextMenuBuilder& ContextMenuBuilder,
	const TArray<FGraphNodeClassData>& Existing,
	const TArray<FGraphNodeClassData>& Supplement,
	const bool bAllowComposites,
	const bool bAllowTasks)
{
	if (bAllowComposites)
	{
		FCategorizedGraphActionListBuilder Builder(TEXT("Composites"));
		for (const FGraphNodeClassData& NodeClass : Supplement)
		{
			UClass* Class = ResolveClass(NodeClass);
			if (!IsUsableClassData(NodeClass)
				|| Class == nullptr
				|| !Class->IsChildOf(UBTCompositeNode::StaticClass())
				|| HasClassData(Existing, Class))
			{
				continue;
			}

				const FText NodeTypeName = AngelscriptAIGraph::GetScriptNodeMenuTitle(*Class);
			TSharedPtr<FAISchemaAction_NewNode> Action = MakeShareable(
				new FAISchemaAction_NewNode(
					NodeClass.GetCategory(), NodeTypeName, NodeClass.GetTooltip(), 0));
			Builder.AddAction(Action);
			UBehaviorTreeGraphNode* Node = NewObject<UBehaviorTreeGraphNode>(
				ContextMenuBuilder.OwnerOfTemporaries, UAngelscriptBehaviorTreeGraphNodeComposite::StaticClass());
			Node->ClassData = NodeClass;
			Action->NodeTemplate = Node;
		}
		ContextMenuBuilder.Append(Builder);
	}

	if (bAllowTasks)
	{
		FCategorizedGraphActionListBuilder Builder(TEXT("Tasks"));
		for (const FGraphNodeClassData& NodeClass : Supplement)
		{
			UClass* Class = ResolveClass(NodeClass);
			if (!IsUsableClassData(NodeClass)
				|| Class == nullptr
				|| !Class->IsChildOf(UBTTaskNode::StaticClass())
				|| HasClassData(Existing, Class))
			{
				continue;
			}

				const FText NodeTypeName = AngelscriptAIGraph::GetScriptNodeMenuTitle(*Class);
			TSharedPtr<FAISchemaAction_NewNode> Action = MakeShareable(
				new FAISchemaAction_NewNode(
					NodeClass.GetCategory(), NodeTypeName, NodeClass.GetTooltip(), 0));
			Builder.AddAction(Action);
			UClass* GraphNodeClass = UAngelscriptBehaviorTreeGraphNodeTask::StaticClass();
			if (NodeClass.GetClassName() == UBTTask_RunBehavior::StaticClass()->GetName())
			{
				GraphNodeClass = UBehaviorTreeGraphNode_SubtreeTask::StaticClass();
			}
			UBehaviorTreeGraphNode* Node = NewObject<UBehaviorTreeGraphNode>(
				ContextMenuBuilder.OwnerOfTemporaries, GraphNodeClass);
			Node->ClassData = NodeClass;
			Action->NodeTemplate = Node;
		}
		ContextMenuBuilder.Append(Builder);
	}
}
}

FText UAngelscriptBehaviorTreeGraphNodeComposite::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	const FText ScriptTitle = GetHazeScriptBehaviorTreeNodeTitle(Cast<UBTNode>(NodeInstance));
	return ScriptTitle.IsEmpty() ? Super::GetNodeTitle(TitleType) : ScriptTitle;
}

FText UAngelscriptBehaviorTreeGraphNodeTask::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	const FText ScriptTitle = GetHazeScriptBehaviorTreeNodeTitle(Cast<UBTNode>(NodeInstance));
	return ScriptTitle.IsEmpty() ? Super::GetNodeTitle(TitleType) : ScriptTitle;
}

FText UAngelscriptBehaviorTreeGraphNodeDecorator::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	const FText ScriptTitle = GetHazeScriptBehaviorTreeNodeTitle(Cast<UBTDecorator>(NodeInstance));
	return ScriptTitle.IsEmpty() ? Super::GetNodeTitle(TitleType) : ScriptTitle;
}

FText UAngelscriptBehaviorTreeGraphNodeService::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	const FText ScriptTitle = GetHazeScriptBehaviorTreeNodeTitle(Cast<UBTService>(NodeInstance));
	return ScriptTitle.IsEmpty() ? Super::GetNodeTitle(TitleType) : ScriptTitle;
}

UAngelscriptBehaviorTreeGraphSchema::UAngelscriptBehaviorTreeGraphSchema(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UAngelscriptBehaviorTreeGraphSchema::GetGraphContextActions(FGraphContextMenuBuilder& ContextMenuBuilder) const
{
	Super::GetGraphContextActions(ContextMenuBuilder);

	bool bAllowComposites = false;
	bool bAllowTasks = false;
	if (!ResolveAllowedNodeFamilies(*this, ContextMenuBuilder.FromPin, bAllowComposites, bAllowTasks))
	{
		return;
	}

	TArray<FGraphNodeClassData> Existing;
	GetClassCache().GatherClasses(UBTCompositeNode::StaticClass(), Existing);
	TArray<FGraphNodeClassData> ExistingTasks;
	GetClassCache().GatherClasses(UBTTaskNode::StaticClass(), ExistingTasks);
	Existing.Append(ExistingTasks);
	TArray<FGraphNodeClassData> ScriptClasses;
	TArray<FGraphNodeClassData> ScriptCompositeClasses;
	TArray<FGraphNodeClassData> ScriptTaskClasses;
	FAngelscriptAIGraphClassProvider::GetScriptNodeClassDataForBaseClass(
		UBTCompositeNode::StaticClass(), ScriptCompositeClasses);
	FAngelscriptAIGraphClassProvider::GetScriptNodeClassDataForBaseClass(
		UBTTaskNode::StaticClass(), ScriptTaskClasses);
	ScriptClasses.Append(ScriptCompositeClasses);
	ScriptClasses.Append(ScriptTaskClasses);
	AddMainGraphActions(ContextMenuBuilder, Existing, ScriptClasses, bAllowComposites, bAllowTasks);
}

void UAngelscriptBehaviorTreeGraphSchema::GetSubNodeClasses(
	int32 SubNodeFlags, TArray<FGraphNodeClassData>& ClassData, UClass*& GraphNodeClass) const
{
	Super::GetSubNodeClasses(SubNodeFlags, ClassData, GraphNodeClass);
	if (SubNodeFlags == ESubNode::Decorator)
	{
		GraphNodeClass = UAngelscriptBehaviorTreeGraphNodeDecorator::StaticClass();
	}
	else if (SubNodeFlags == ESubNode::Service)
	{
		GraphNodeClass = UAngelscriptBehaviorTreeGraphNodeService::StaticClass();
	}
	const UClass* BaseClass = SubNodeFlags == ESubNode::Decorator
		? UBTDecorator::StaticClass()
		: UBTService::StaticClass();
	TArray<FGraphNodeClassData> ScriptClasses;
	FAngelscriptAIGraphClassProvider::GetScriptNodeClassDataForBaseClass(BaseClass, ScriptClasses);
	AppendUniqueClassData(ClassData, ScriptClasses);
}

UAngelscriptBehaviorTreeDecoratorGraphSchema::UAngelscriptBehaviorTreeDecoratorGraphSchema(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UAngelscriptBehaviorTreeDecoratorGraphSchema::GetGraphContextActions(
	FGraphContextMenuBuilder& ContextMenuBuilder) const
{
	Super::GetGraphContextActions(ContextMenuBuilder);

	TArray<FGraphNodeClassData> Existing;
	GetClassCache().GatherClasses(UBTDecorator::StaticClass(), Existing);
	TArray<FGraphNodeClassData> ScriptClasses;
	FAngelscriptAIGraphClassProvider::GetScriptNodeClassDataForBaseClass(
		UBTDecorator::StaticClass(), ScriptClasses);
	for (const FGraphNodeClassData& NodeClass : ScriptClasses)
	{
		UClass* Class = ResolveClass(NodeClass);
		if (!IsUsableClassData(NodeClass)
			|| Class == nullptr
			|| HasClassData(Existing, Class))
		{
			continue;
		}

		TSharedPtr<FDecoratorSchemaAction_NewNode> Action = AddNewDecoratorAction(
			ContextMenuBuilder,
			NodeClass.GetCategory(),
			AngelscriptAIGraph::GetScriptNodeMenuTitle(*Class),
			FText::GetEmpty());
		UBehaviorTreeDecoratorGraphNode_Decorator* Node = NewObject<UBehaviorTreeDecoratorGraphNode_Decorator>(
			ContextMenuBuilder.OwnerOfTemporaries);
		Node->ClassData = NodeClass;
		Action->NodeTemplate = Node;
	}
}

void UAngelscriptAIGraphSchemaSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	StockBehaviorTreeSchema = UEdGraphSchema_BehaviorTree::StaticClass();
	StockDecoratorSchema = UEdGraphSchema_BehaviorTreeDecorator::StaticClass();
	ProductBehaviorTreeSchema = UAngelscriptBehaviorTreeGraphSchema::StaticClass();
	ProductDecoratorSchema = UAngelscriptBehaviorTreeDecoratorGraphSchema::StaticClass();
	InstallLoadedGraphs();

	if (GEditor != nullptr)
	{
		if (UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorRequestedOpenHandle = AssetEditors->OnAssetEditorRequestedOpen()
				.AddUObject(
					this,
					&UAngelscriptAIGraphSchemaSubsystem::OnAssetEditorRequestedOpen);
			AssetEditorOpenedHandle = AssetEditors->OnAssetEditorOpened()
				.AddUObject(
					this,
					&UAngelscriptAIGraphSchemaSubsystem::OnAssetEditorOpened);
			for (UObject* Asset : AssetEditors->GetAllEditedAssets())
			{
				InstallGraphsForAsset(Asset);
			}
		}
	}
	ObjectConstructedHandle = FCoreUObjectDelegates::OnObjectConstructed.AddUObject(
		this, &UAngelscriptAIGraphSchemaSubsystem::OnObjectConstructed);
	ObjectPreSaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddUObject(
		this, &UAngelscriptAIGraphSchemaSubsystem::OnObjectPreSave);
	PreSavePackageHandle = UPackage::PreSavePackageWithContextEvent.AddUObject(
		this, &UAngelscriptAIGraphSchemaSubsystem::OnPreSavePackage);
	PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddUObject(
		this, &UAngelscriptAIGraphSchemaSubsystem::OnPackageSaved);
}

void UAngelscriptAIGraphSchemaSubsystem::Deinitialize()
{
	if (DeferredInstallTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DeferredInstallTickerHandle);
		DeferredInstallTickerHandle.Reset();
	}
	PendingGraphs.Reset();
	PendingPackages.Reset();
	if (GEditor != nullptr)
	{
		if (UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			if (AssetEditorRequestedOpenHandle.IsValid())
			{
				AssetEditors->OnAssetEditorRequestedOpen().Remove(AssetEditorRequestedOpenHandle);
			}
			if (AssetEditorOpenedHandle.IsValid())
			{
				AssetEditors->OnAssetEditorOpened().Remove(AssetEditorOpenedHandle);
			}
		}
	}
	if (PreSavePackageHandle.IsValid())
	{
		UPackage::PreSavePackageWithContextEvent.Remove(PreSavePackageHandle);
	}
	if (ObjectConstructedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectConstructed.Remove(ObjectConstructedHandle);
	}
	if (ObjectPreSaveHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPreSave.Remove(ObjectPreSaveHandle);
	}
	if (PackageSavedHandle.IsValid())
	{
		UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
	}
	RestoreLoadedGraphs();
	InstalledGraphs.Reset();
	Super::Deinitialize();
}

void UAngelscriptAIGraphSchemaSubsystem::OnAssetEditorRequestedOpen(UObject* Asset)
{
	InstallGraphsForAsset(Asset);
}

void UAngelscriptAIGraphSchemaSubsystem::OnAssetEditorOpened(UObject* Asset)
{
	InstallGraphsForAsset(Asset);
}

void UAngelscriptAIGraphSchemaSubsystem::OnObjectConstructed(UObject* Object)
{
	if (IsInGameThread())
	{
		UEdGraph* Graph = Cast<UEdGraph>(Object);
		if (Graph != nullptr && !Graph->HasAnyFlags(RF_NeedLoad))
		{
			InstallGraph(Graph);
		}
		if (Graph != nullptr
			&& (Graph->IsA<UBehaviorTreeGraph>() || Graph->IsA<UBehaviorTreeDecoratorGraph>()))
		{
			PendingGraphs.Add(Graph);
			EnsureDeferredInstallTicker();
		}
	}
}

void UAngelscriptAIGraphSchemaSubsystem::OnObjectPreSave(
	UObject* Object, FObjectPreSaveContext)
{
	if (UBehaviorTreeDecoratorGraph* Graph = Cast<UBehaviorTreeDecoratorGraph>(Object))
	{
		SetGraphSchema(Graph, StockDecoratorSchema);
	}
	else if (UBehaviorTreeGraph* BehaviorTreeGraph = Cast<UBehaviorTreeGraph>(Object))
	{
		SetGraphSchema(BehaviorTreeGraph, StockBehaviorTreeSchema);
	}
}

void UAngelscriptAIGraphSchemaSubsystem::OnPreSavePackage(UPackage* Package, FObjectPreSaveContext)
{
	RestoreLoadedGraphs(Package);
	if (Package != nullptr)
	{
		PendingPackages.Add(Package);
		EnsureDeferredInstallTicker();
	}
}

void UAngelscriptAIGraphSchemaSubsystem::OnPackageSaved(
	const FString&, UPackage* Package, FObjectPostSaveContext)
{
	PendingPackages.Remove(Package);
	InstallLoadedGraphs(Package);
}

void UAngelscriptAIGraphSchemaSubsystem::EnsureDeferredInstallTicker()
{
	if (!DeferredInstallTickerHandle.IsValid())
	{
		DeferredInstallTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(
				this, &UAngelscriptAIGraphSchemaSubsystem::FlushDeferredInstalls), 0.f);
	}
}

bool UAngelscriptAIGraphSchemaSubsystem::FlushDeferredInstalls(float)
{
	DeferredInstallTickerHandle.Reset();
	TSet<TWeakObjectPtr<UPackage>> Packages = MoveTemp(PendingPackages);
	TSet<TWeakObjectPtr<UEdGraph>> Graphs = MoveTemp(PendingGraphs);
	PendingPackages.Reset();
	PendingGraphs.Reset();
	for (const TWeakObjectPtr<UPackage>& WeakPackage : Packages)
	{
		if (UPackage* Package = WeakPackage.Get())
		{
			InstallLoadedGraphs(Package);
		}
	}
	for (const TWeakObjectPtr<UEdGraph>& WeakGraph : Graphs)
	{
		if (UEdGraph* Graph = WeakGraph.Get())
		{
			InstallGraph(Graph);
		}
	}
	return false;
}

void UAngelscriptAIGraphSchemaSubsystem::InstallGraphsForAsset(UObject* Asset)
{
	if (Asset == nullptr)
	{
		return;
	}
	InstallGraph(Cast<UEdGraph>(Asset));
	TArray<UObject*> NestedObjects;
	GetObjectsWithOuter(Asset, NestedObjects, true);
	for (UObject* Object : NestedObjects)
	{
		InstallGraph(Cast<UEdGraph>(Object));
	}
}

void UAngelscriptAIGraphSchemaSubsystem::InstallLoadedGraphs(UPackage* Package)
{
	TArray<UObject*> Objects;
	constexpr EObjectFlags ExcludedFlags = RF_ClassDefaultObject | RF_ArchetypeObject;
	GetObjectsOfClass(
		UBehaviorTreeGraph::StaticClass(), Objects, true, ExcludedFlags);
	TArray<UObject*> DecoratorGraphs;
	GetObjectsOfClass(
		UBehaviorTreeDecoratorGraph::StaticClass(),
		DecoratorGraphs,
		true,
		ExcludedFlags);
	Objects.Append(DecoratorGraphs);
	for (UObject* Object : Objects)
	{
		UEdGraph* Graph = Cast<UEdGraph>(Object);
		if (Graph != nullptr
			&& (Package == nullptr || Graph->GetOutermost() == Package)
			&& (Package == nullptr || InstalledGraphs.Contains(TWeakObjectPtr<UEdGraph>(Graph))))
		{
			InstallGraph(Graph);
		}
	}
}

void UAngelscriptAIGraphSchemaSubsystem::RestoreLoadedGraphs(UPackage* Package)
{
	TArray<UObject*> Objects;
	constexpr EObjectFlags ExcludedFlags = RF_ClassDefaultObject | RF_ArchetypeObject;
	GetObjectsOfClass(
		UBehaviorTreeGraph::StaticClass(), Objects, true, ExcludedFlags);
	TArray<UObject*> DecoratorGraphs;
	GetObjectsOfClass(
		UBehaviorTreeDecoratorGraph::StaticClass(),
		DecoratorGraphs,
		true,
		ExcludedFlags);
	Objects.Append(DecoratorGraphs);
	for (UObject* Object : Objects)
	{
		if (Package == nullptr || Object->GetOutermost() == Package)
		{
			if (Cast<UBehaviorTreeDecoratorGraph>(Object) != nullptr)
			{
				SetGraphSchema(Cast<UEdGraph>(Object), StockDecoratorSchema);
			}
			else
			{
				SetGraphSchema(Cast<UEdGraph>(Object), StockBehaviorTreeSchema);
			}
		}
	}
}

void UAngelscriptAIGraphSchemaSubsystem::InstallGraph(UEdGraph* Graph)
{
	if (Graph == nullptr || Graph->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return;
	}
	if (Graph->IsA<UBehaviorTreeDecoratorGraph>())
	{
		SetGraphSchema(Graph, ProductDecoratorSchema);
	}
	else if (Graph->IsA<UBehaviorTreeGraph>())
	{
		SetGraphSchema(Graph, ProductBehaviorTreeSchema);
	}
}

void UAngelscriptAIGraphSchemaSubsystem::SetGraphSchema(UEdGraph* Graph, UClass* SchemaClass)
{
	if (Graph == nullptr || SchemaClass == nullptr)
	{
		return;
	}
	if (Graph->IsA<UBehaviorTreeDecoratorGraph>()
		&& (Graph->Schema == UEdGraphSchema_BehaviorTreeDecorator::StaticClass()
			|| Graph->Schema == ProductDecoratorSchema))
	{
		Graph->Schema = SchemaClass;
		if (SchemaClass == ProductDecoratorSchema)
		{
			InstalledGraphs.Add(TWeakObjectPtr<UEdGraph>(Graph));
		}
	}
	else if (Graph->IsA<UBehaviorTreeGraph>()
		&& (Graph->Schema == UEdGraphSchema_BehaviorTree::StaticClass()
			|| Graph->Schema == ProductBehaviorTreeSchema))
	{
		Graph->Schema = SchemaClass;
		if (SchemaClass == ProductBehaviorTreeSchema)
		{
			InstalledGraphs.Add(TWeakObjectPtr<UEdGraph>(Graph));
		}
	}
}
