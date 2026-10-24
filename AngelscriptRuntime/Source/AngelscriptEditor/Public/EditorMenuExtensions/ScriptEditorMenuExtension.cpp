#include "ScriptEditorMenuExtension.h"
#include "AngelscriptManager.h"
#include "ClassGenerator/AngelscriptClassGenerator.h"
#include "ClassGenerator/ASClass.h"
                                                                                                                            
class asITypeInfo;
ANGELSCRIPTCODE_API asITypeInfo* GetAngelscriptClassScriptType(const UClass*);
                                                                                                                          
                                                                                                              
struct FToolMenuCategorySnapshot
{
	FString Name;
	TArray<FName> FunctionNames;
	TArray<TSharedRef<const FToolMenuCategorySnapshot>> SubCategories;
};

static TSharedRef<const FToolMenuCategorySnapshot>
MakeToolMenuCategorySnapshot(const struct FFunctionCategory& Category);

static UScriptEditorMenuExtension* ResolveToolMenuExtension(TWeakObjectPtr<UClass> ExtensionClass)
{
	UClass* Class = ExtensionClass.Get();
	if (!IsValid(Class) || Class->HasAnyClassFlags(CLASS_NewerVersionExists)
		|| !Class->IsChildOf(UScriptEditorMenuExtension::StaticClass()))
		return nullptr;
	return Cast<UScriptEditorMenuExtension>(Class->GetDefaultObject(false));
}

static UFunction* ResolveToolMenuFunction(
	TWeakObjectPtr<UClass> ExtensionClass,
	FName FunctionName,
	bool bRequireCallInEditor)
{
	const UScriptEditorMenuExtension* Extension = ResolveToolMenuExtension(ExtensionClass);
	if (!IsValid(Extension) || FunctionName.IsNone())
		return nullptr;

	UFunction* Function = Extension->FindFunction(FunctionName);
	UClass* DeclaringClass = IsValid(Function) ? Function->GetOuterUClass() : nullptr;
	if (!IsValid(Function) || !IsValid(DeclaringClass)
		|| DeclaringClass->HasAnyClassFlags(CLASS_NewerVersionExists)
		|| !Extension->GetClass()->IsChildOf(DeclaringClass)
		|| (bRequireCallInEditor && !Function->HasMetaData(FName(TEXT("CallInEditor"))))
		|| (bRequireCallInEditor && Function->GetReturnProperty() != nullptr))
		return nullptr;

	return Function;
}

static UFunction* ResolveToolMenuActionFunction(TWeakObjectPtr<UClass> ExtensionClass, FName FunctionName)
{
	return ResolveToolMenuFunction(ExtensionClass, FunctionName, true);
}

static UFunction* ResolveToolMenuPredicateFunction(TWeakObjectPtr<UClass> ExtensionClass, FName FunctionName)
{
	UFunction* Function = ResolveToolMenuFunction(ExtensionClass, FunctionName, false);
	if (!IsValid(Function) || Function->ReturnValueOffset != 0
		|| CastField<FBoolProperty>(Function->GetReturnProperty()) == nullptr)
		return nullptr;
	return Function;
}
                                                                                                            
#include "AngelscriptCodeModule.h"
#include "IStructureDetailsView.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "ScriptEditorPrompts.h"
#include "LevelEditor.h"
#include "SourceCodeNavigation.h"
#include "ContentBrowserDelegates.h"
#include "ContentBrowserModule.h"
#include "ToolMenus.h"
#include "ToolMenuSection.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"
#include "ContentBrowserMenuContexts.h"
#include "LevelEditorMenuContext.h"
#include "Editor.h"
#include "Selection.h"

void UScriptEditorMenuExtension::InitializeExtensions()
{
	FAngelscriptClassGenerator::OnPostReload.AddLambda([](bool bFullReload)
	{
		if (bFullReload)
		{
			UScriptEditorMenuExtension::UnregisterExtensions();
			UScriptEditorMenuExtension::RegisterExtensions();
		}
	});

	FCoreDelegates::OnEnginePreExit.AddLambda([]()
	{
		UScriptEditorMenuExtension::UnregisterExtensions();
	});

	FEditorDelegates::MapChange.AddLambda([](uint32 Flags)
	{
		UScriptEditorMenuExtension::RefreshExtensionRegistration();
	});

	FWorldDelegates::LevelAddedToWorld.AddLambda([](ULevel* InLevel, UWorld* InWorld)
	{
		if (IsValid(InWorld) && InWorld->WorldType == EWorldType::Editor)
			UScriptEditorMenuExtension::RefreshExtensionRegistration();
	});

	if (FAngelscriptManager::bIsInitialCompileFinished)
	{
		UScriptEditorMenuExtension::RegisterExtensions();
	}
}

UWorld* UScriptEditorMenuExtension::GetWorld() const
{
	return (GEditor != nullptr) ? GEditor->GetEditorWorldContext().World() : nullptr;
}

TArray<UFunction*> UScriptEditorMenuExtension::GatherExtensionFunctions() const
{
	TArray<UFunction*> Functions;

	const static FName NAME_CallInEditor(TEXT("CallInEditor"));
	for (TFieldIterator<UFunction> FunctionIt(GetClass(), EFieldIterationFlags::IncludeSuper); FunctionIt; ++FunctionIt)
	{
		UFunction* Function = *FunctionIt;
		if (Function == nullptr)
			continue;
		if (!Function->HasMetaData(NAME_CallInEditor))
			continue;
		if (Function->GetReturnProperty() != nullptr)
			continue;

		Functions.Add(Function);
	}

	return Functions;
}

FName UScriptEditorMenuExtension::GetExtensionPointOrDefault() const
{
	if (!ExtensionPoint.IsNone())
		return ExtensionPoint;

	switch (ExtensionMenu)
	{
		case EScriptEditorMenuExtensionLocation::LevelViewport_ContextMenu:
			return "ActorOptions";
		case EScriptEditorMenuExtensionLocation::ContentBrowser_AssetViewContextMenu:
			return "CommonAssetActions";
	}

	return NAME_None;
}

UObject* UScriptEditorMenuExtension::GetToolMenuContext(TSubclassOf<UObject> ContextClass) const
{
	if (ActiveToolMenuContext != nullptr && ContextClass.Get() != nullptr)
		return ActiveToolMenuContext->FindByClass(ContextClass.Get());
	return nullptr;
}

TSharedRef<FExtender> UScriptEditorMenuExtension::Extend(const TSharedRef<FUICommandList> CommandList, FExtenderSelection Selection) const
{
	FEditorScriptExecutionGuard ScriptGuard;
	TGuardValue<FExtenderSelection> ScopeSelection(CurrentSelection, Selection);

	TSharedRef<FExtender> Extender(new FExtender());

	if (!ShouldExtend())
		return Extender;

	TArray<UFunction*> Functions = GatherExtensionFunctions();
	if (Functions.Num() == 0)
		return Extender;

	Extender->AddMenuExtension(
		GetExtensionPointOrDefault(),
		(EExtensionHook::Position)ExtensionOrder,
		CommandList,
		FMenuExtensionDelegate::CreateLambda(
			[Extension=this, Functions, Selection](FMenuBuilder& MenuBuilder)
			{
				Extension->BuildMenu(MenuBuilder, Functions, Selection);
			}
		));

	return Extender;
}

void UScriptEditorMenuExtension::CallFunctionOnSelection(UFunction* Function, FExtenderSelection Selection) const
{
	FScriptEditorPromptOptions Options;
	FScriptEditorPrompts::ShowPromptToCallFunction(
		this,
		Function->GetFName(),
		Options,
		TArray<UObject*>()
	);
}

const static FName NAME_EXT_EditorIcon(TEXT("EditorIcon"));
const static FName NAME_EXT_ActionCanExecute(TEXT("ActionCanExecute"));
const static FName NAME_EXT_ActionIsVisible(TEXT("ActionIsVisible"));
const static FName NAME_EXT_ActionIsChecked(TEXT("ActionIsChecked"));
const static FName NAME_EXT_ActionType(TEXT("ActionType"));
const static FName NAME_EXT_EditorButtonStyle(TEXT("EditorButtonStyle"));
const static FName NAME_EXT_ToolbarLabel(TEXT("ToolbarLabel"));

static EUserInterfaceActionType GetExtensionActionType(UFunction* Function)
{
	if (Function->HasMetaData(NAME_EXT_ActionType))
	{
		const FString& ActionType = Function->GetMetaData(NAME_EXT_ActionType);
		if (ActionType == TEXT("None"))
			return EUserInterfaceActionType::None;
		else if (ActionType == TEXT("Button"))
			return EUserInterfaceActionType::Button;
		else if (ActionType == TEXT("ToggleButton"))
			return EUserInterfaceActionType::ToggleButton;
		else if (ActionType == TEXT("RadioButton"))
			return EUserInterfaceActionType::RadioButton;
		else if (ActionType == TEXT("Check"))
			return EUserInterfaceActionType::Check;
		else if (ActionType == TEXT("CollapsedButton"))
			return EUserInterfaceActionType::CollapsedButton;
	}

	return EUserInterfaceActionType::Button;
}

void UScriptEditorMenuExtension::AddMenuEntry(class FMenuBuilder& MenuBuilder, UFunction* Function, FExtenderSelection Selection) const
{
	FString IconName = Function->GetMetaData(NAME_EXT_EditorIcon);
	if (IconName.IsEmpty())
		IconName = TEXT("GraphEditor.Event_16x");

	FText DisplayName = Function->GetDisplayNameText();
	if (Function->HasMetaData(NAME_EXT_ToolbarLabel))
		DisplayName = FText::FromString(Function->GetMetaData(NAME_EXT_ToolbarLabel));

	MenuBuilder.AddMenuEntry(
		DisplayName,
		Function->GetToolTipText(),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), FName(*IconName)),
		CreateUIAction(Function, Selection),
		NAME_None,
		GetExtensionActionType(Function)
	);
}

void UScriptEditorMenuExtension::AddToolMenuEntry(FToolMenuSection& MenuSection, UFunction* Function) const
{
	FString IconName = Function->GetMetaData(NAME_EXT_EditorIcon);
	if (IconName.IsEmpty())
		IconName = TEXT("GraphEditor.Event_16x");

	FText DisplayName = Function->GetDisplayNameText();
	if (Function->HasMetaData(NAME_EXT_ToolbarLabel))
		DisplayName = FText::FromString(Function->GetMetaData(NAME_EXT_ToolbarLabel));

	MenuSection.AddMenuEntry(
		Function->GetFName(),
		DisplayName,
		Function->GetToolTipText(),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), FName(*IconName)),
		CreateToolUIAction(Function),
		GetExtensionActionType(Function)
	);
}

void UScriptEditorMenuExtension::AddToolMenuButton(FToolMenuSection& MenuSection, UFunction* Function) const
{
	FString IconName = Function->GetMetaData(NAME_EXT_EditorIcon);
	if (IconName.IsEmpty())
		IconName = TEXT("GraphEditor.Event_16x");

	FText DisplayName = Function->GetDisplayNameText();
	if (Function->HasMetaData(NAME_EXT_ToolbarLabel))
		DisplayName = FText::FromString(Function->GetMetaData(NAME_EXT_ToolbarLabel));

	auto Entry = FToolMenuEntry::InitToolBarButton(
		Function->GetFName(),
		CreateToolUIAction(Function),
		DisplayName,
		Function->GetToolTipText(),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), FName(*IconName)),
		GetExtensionActionType(Function)
	);

	FString StyleName = Function->GetMetaData(NAME_EXT_EditorButtonStyle);
	if (!StyleName.IsEmpty())
		Entry.StyleNameOverride = *StyleName;

	MenuSection.AddEntry(Entry);
}

UScriptEditorMenuExtension::FExtenderSelection UScriptEditorMenuExtension::GetSelectionFromToolMenuContext(const FToolMenuContext& Context) const
{
	FExtenderSelection Selection;

	auto* ContentBrowserContext = Context.FindContext<UContentBrowserAssetContextMenuContext>();
	if (ContentBrowserContext != nullptr)
		Selection.SelectedAssets.Append(ContentBrowserContext->SelectedAssets);

	return Selection;
}

FToolUIAction UScriptEditorMenuExtension::CreateToolUIAction(UFunction* Function) const
{
                                                                                                                  
	#if 0
                                                                                                                
	FToolUIAction Action;
	Action.ExecuteAction = FToolMenuExecuteAction::CreateLambda([Extension=this, Function](const FToolMenuContext& Context)
	{
		if (FSlateApplication::Get().GetModifierKeys().IsShiftDown())
		{
			// Browse to the function in script
		   const bool bFoundFunction = FSourceCodeNavigation::NavigateToFunction(Function);
		   if (!bFoundFunction)
		   {
			  FSourceCodeNavigation::NavigateToClass(Function->GetOuterUClass());
		   }
		}
		else
		{
			UScriptEditorMenuExtension* TempObject = NewObject<UScriptEditorMenuExtension>(GetTransientPackage(), Extension->GetClass());
			TempObject->AddToRoot();

			TGuardValue<const FToolMenuContext*> ScopeActiveContext(TempObject->ActiveToolMenuContext, &Context);

			FExtenderSelection Selection = Extension->GetSelectionFromToolMenuContext(Context);

			FEditorScriptExecutionGuard ScriptGuard;
			TGuardValue<FExtenderSelection> ScopeSelection(Extension->CurrentSelection, Selection);
			FScopedTransaction Transaction(FText::FromString(Function->GetDisplayNameText().ToString()));

			TempObject->CallFunctionOnSelection(Function, Selection);
			TempObject->RemoveFromRoot();
		}
	});

	FString CanExecute = Function->GetMetaData(NAME_EXT_ActionCanExecute);
	if (!CanExecute.IsEmpty())
	{
		Action.CanExecuteAction = FToolMenuCanExecuteAction::CreateLambda(
			[Extension=this, Function, FuncName=FName(CanExecute)](const FToolMenuContext& Context) -> bool
			{
				FEditorScriptExecutionGuard ScriptGuard;
				TGuardValue<const FToolMenuContext*> ScopeActiveContext(Extension->ActiveToolMenuContext, &Context);
				TGuardValue<FExtenderSelection> ScopeSelection(Extension->CurrentSelection, Extension->GetSelectionFromToolMenuContext(Context));

				UFunction* ExecFunction = Extension->FindFunction(FuncName);
				if (ExecFunction == nullptr)
					return false;
				if (ExecFunction->ReturnValueOffset != 0)
					return false;
				if (CastField<FBoolProperty>(ExecFunction->GetReturnProperty()) == nullptr)
					return false;

				bool bReturnValue = false;
				((UObject*)Extension)->ProcessEvent(ExecFunction, &bReturnValue);
				return bReturnValue;
			});
	}

	FString IsVisible = Function->GetMetaData(NAME_EXT_ActionIsVisible);
	if (!IsVisible.IsEmpty())
	{
		Action.IsActionVisibleDelegate = FToolMenuIsActionButtonVisible::CreateLambda(
			[Extension=this, Function, FuncName=FName(IsVisible)](const FToolMenuContext& Context) -> bool
			{
				FEditorScriptExecutionGuard ScriptGuard;
				TGuardValue<const FToolMenuContext*> ScopeActiveContext(Extension->ActiveToolMenuContext, &Context);
				TGuardValue<FExtenderSelection> ScopeSelection(Extension->CurrentSelection, Extension->GetSelectionFromToolMenuContext(Context));

				UFunction* ExecFunction = Extension->FindFunction(FuncName);
				if (ExecFunction == nullptr)
					return false;
				if (ExecFunction->ReturnValueOffset != 0)
					return false;
				if (CastField<FBoolProperty>(ExecFunction->GetReturnProperty()) == nullptr)
					return false;

				bool bReturnValue = false;
				((UObject*)Extension)->ProcessEvent(ExecFunction, &bReturnValue);
				return bReturnValue;
			});
	}

	FString IsChecked = Function->GetMetaData(NAME_EXT_ActionIsChecked);
	if (!IsChecked.IsEmpty())
	{
		Action.GetActionCheckState = FToolMenuGetActionCheckState::CreateLambda(
			[Extension=this, Function, FuncName=FName(IsChecked)](const FToolMenuContext& Context) -> ECheckBoxState
			{
				FEditorScriptExecutionGuard ScriptGuard;
				TGuardValue<const FToolMenuContext*> ScopeActiveContext(Extension->ActiveToolMenuContext, &Context);
				TGuardValue<FExtenderSelection> ScopeSelection(Extension->CurrentSelection, Extension->GetSelectionFromToolMenuContext(Context));

				UFunction* ExecFunction = Extension->FindFunction(FuncName);
				if (ExecFunction == nullptr)
					return ECheckBoxState::Unchecked;
				if (ExecFunction->ReturnValueOffset != 0)
					return ECheckBoxState::Unchecked;
				if (CastField<FBoolProperty>(ExecFunction->GetReturnProperty()) == nullptr)
					return ECheckBoxState::Unchecked;

				bool bReturnValue = false;
				((UObject*)Extension)->ProcessEvent(ExecFunction, &bReturnValue);
				return bReturnValue ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			});
	}

	return Action;
                                                                                                        
	#endif
	const FName FunctionName = Function->GetFName();
	const TWeakObjectPtr<UClass> ExtensionClass = GetClass();
	if (ResolveToolMenuActionFunction(ExtensionClass, FunctionName) == nullptr)
		return FToolUIAction();
	FToolUIAction Action;
	Action.ExecuteAction = FToolMenuExecuteAction::CreateLambda(
		[ExtensionClass, FunctionName](const FToolMenuContext& Context)
	{
		UScriptEditorMenuExtension* Extension = ResolveToolMenuExtension(ExtensionClass);
		UFunction* ResolvedFunction = ResolveToolMenuActionFunction(ExtensionClass, FunctionName);
		if (Extension == nullptr || ResolvedFunction == nullptr || ResolvedFunction->GetReturnProperty() != nullptr)
			return;
		if (FSlateApplication::Get().GetModifierKeys().IsShiftDown())
		{
			if (!FSourceCodeNavigation::NavigateToFunction(ResolvedFunction))
				FSourceCodeNavigation::NavigateToClass(ResolvedFunction->GetOuterUClass());
		}
		else
		{
			UScriptEditorMenuExtension* TempObject =
				NewObject<UScriptEditorMenuExtension>(GetTransientPackage(), Extension->GetClass());
			TempObject->AddToRoot();
			TGuardValue<const FToolMenuContext*> ScopeActiveContext(TempObject->ActiveToolMenuContext, &Context);
			FExtenderSelection Selection = Extension->GetSelectionFromToolMenuContext(Context);
			FEditorScriptExecutionGuard ScriptGuard;
			TGuardValue<FExtenderSelection> ScopeSelection(Extension->CurrentSelection, Selection);
			FScopedTransaction Transaction(FText::FromString(ResolvedFunction->GetDisplayNameText().ToString()));
			TempObject->CallFunctionOnSelection(ResolvedFunction, Selection);
			TempObject->RemoveFromRoot();
		}
	});

                                                                                                      
                                                                                                                  
	const FString CanExecute = Function->GetMetaData(NAME_EXT_ActionCanExecute);
	const FString IsVisible = Function->GetMetaData(NAME_EXT_ActionIsVisible);
	const FString IsChecked = Function->GetMetaData(NAME_EXT_ActionIsChecked);
	if (!CanExecute.IsEmpty())
	{
		const FName PredicateName(*CanExecute);
		Action.CanExecuteAction = FToolMenuCanExecuteAction::CreateLambda(
			[ExtensionClass, PredicateName](const FToolMenuContext& Context) -> bool
		{
			UScriptEditorMenuExtension* Extension = ResolveToolMenuExtension(ExtensionClass);
			UFunction* Predicate = ResolveToolMenuPredicateFunction(ExtensionClass, PredicateName);
			if (Extension == nullptr || Predicate == nullptr)
				return false;
			FEditorScriptExecutionGuard ScriptGuard;
			TGuardValue<const FToolMenuContext*> ScopeActiveContext(Extension->ActiveToolMenuContext, &Context);
			TGuardValue<FExtenderSelection> ScopeSelection(
				Extension->CurrentSelection,
				Extension->GetSelectionFromToolMenuContext(Context));
			bool bReturnValue = false;
			((UObject*)Extension)->ProcessEvent(Predicate, &bReturnValue);
			return bReturnValue;
		});
	}
	if (!IsVisible.IsEmpty())
	{
		const FName PredicateName(*IsVisible);
		Action.IsActionVisibleDelegate = FToolMenuIsActionButtonVisible::CreateLambda(
			[ExtensionClass, PredicateName](const FToolMenuContext& Context) -> bool
		{
			UScriptEditorMenuExtension* Extension = ResolveToolMenuExtension(ExtensionClass);
			UFunction* Predicate = ResolveToolMenuPredicateFunction(ExtensionClass, PredicateName);
			if (Extension == nullptr || Predicate == nullptr)
				return false;
			FEditorScriptExecutionGuard ScriptGuard;
			TGuardValue<const FToolMenuContext*> ScopeActiveContext(Extension->ActiveToolMenuContext, &Context);
			TGuardValue<FExtenderSelection> ScopeSelection(
				Extension->CurrentSelection,
				Extension->GetSelectionFromToolMenuContext(Context));
			bool bReturnValue = false;
			((UObject*)Extension)->ProcessEvent(Predicate, &bReturnValue);
			return bReturnValue;
		});
	}
	if (!IsChecked.IsEmpty())
	{
		const FName PredicateName(*IsChecked);
		Action.GetActionCheckState = FToolMenuGetActionCheckState::CreateLambda(
			[ExtensionClass, PredicateName](const FToolMenuContext& Context) -> ECheckBoxState
		{
			UScriptEditorMenuExtension* Extension = ResolveToolMenuExtension(ExtensionClass);
			UFunction* Predicate = ResolveToolMenuPredicateFunction(ExtensionClass, PredicateName);
			if (Extension == nullptr || Predicate == nullptr)
				return ECheckBoxState::Unchecked;
			FEditorScriptExecutionGuard ScriptGuard;
			TGuardValue<const FToolMenuContext*> ScopeActiveContext(Extension->ActiveToolMenuContext, &Context);
			TGuardValue<FExtenderSelection> ScopeSelection(
				Extension->CurrentSelection,
				Extension->GetSelectionFromToolMenuContext(Context));
			bool bReturnValue = false;
			((UObject*)Extension)->ProcessEvent(Predicate, &bReturnValue);
			return bReturnValue ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		});
	}
	return Action;
                                                                                                                
}

FUIAction UScriptEditorMenuExtension::CreateUIAction(UFunction* Function, FExtenderSelection Selection) const
{
	FUIAction Action;
	Action.ExecuteAction = FExecuteAction::CreateLambda([Extension=this, Function, Selection]()
	{
		if (FSlateApplication::Get().GetModifierKeys().IsShiftDown())
		{
			// Browse to the function in script
		   const bool bFoundFunction = FSourceCodeNavigation::NavigateToFunction(Function);
		   if (!bFoundFunction)
		   {
			  FSourceCodeNavigation::NavigateToClass(Function->GetOuterUClass());
		   }
		}
		else
		{
			UScriptEditorMenuExtension* TempObject = NewObject<UScriptEditorMenuExtension>(GetTransientPackage(), Extension->GetClass());
			TempObject->AddToRoot();

			FEditorScriptExecutionGuard ScriptGuard;
			TGuardValue<FExtenderSelection> ScopeSelection(TempObject->CurrentSelection, Selection);
			FScopedTransaction Transaction(FText::FromString(Function->GetDisplayNameText().ToString()));

			TempObject->CallFunctionOnSelection(Function, Selection);
			TempObject->RemoveFromRoot();
		}
	});

	FString CanExecute = Function->GetMetaData(NAME_EXT_ActionCanExecute);
	if (!CanExecute.IsEmpty())
	{
		Action.CanExecuteAction = FCanExecuteAction::CreateLambda(
			[Extension=this, Function, Selection, FuncName=FName(CanExecute)]() -> bool
			{
				FEditorScriptExecutionGuard ScriptGuard;
				TGuardValue<FExtenderSelection> ScopeSelection(Extension->CurrentSelection, Selection);

				UFunction* ExecFunction = Extension->FindFunction(FuncName);
				if (ExecFunction == nullptr)
					return false;
				if (ExecFunction->ReturnValueOffset != 0)
					return false;
				if (CastField<FBoolProperty>(ExecFunction->GetReturnProperty()) == nullptr)
					return false;

				bool bReturnValue = false;
				((UObject*)Extension)->ProcessEvent(ExecFunction, &bReturnValue);
				return bReturnValue;
			});
	}

	FString IsVisible = Function->GetMetaData(NAME_EXT_ActionIsVisible);
	if (!IsVisible.IsEmpty())
	{
		Action.IsActionVisibleDelegate = FIsActionButtonVisible::CreateLambda(
			[Extension=this, Function, Selection, FuncName=FName(IsVisible)]() -> bool
			{
				FEditorScriptExecutionGuard ScriptGuard;
				TGuardValue<FExtenderSelection> ScopeSelection(Extension->CurrentSelection, Selection);

				UFunction* ExecFunction = Extension->FindFunction(FuncName);
				if (ExecFunction == nullptr)
					return false;
				if (ExecFunction->ReturnValueOffset != 0)
					return false;
				if (CastField<FBoolProperty>(ExecFunction->GetReturnProperty()) == nullptr)
					return false;

				bool bReturnValue = false;
				((UObject*)Extension)->ProcessEvent(ExecFunction, &bReturnValue);
				return bReturnValue;
			});
	}

	FString IsChecked = Function->GetMetaData(NAME_EXT_ActionIsChecked);
	if (!IsChecked.IsEmpty())
	{
		Action.GetActionCheckState = FGetActionCheckState::CreateLambda(
			[Extension=this, Function, Selection, FuncName=FName(IsChecked)]() -> ECheckBoxState
			{
				FEditorScriptExecutionGuard ScriptGuard;
				TGuardValue<FExtenderSelection> ScopeSelection(Extension->CurrentSelection, Selection);

				UFunction* ExecFunction = Extension->FindFunction(FuncName);
				if (ExecFunction == nullptr)
					return ECheckBoxState::Unchecked;
				if (ExecFunction->ReturnValueOffset != 0)
					return ECheckBoxState::Unchecked;
				if (CastField<FBoolProperty>(ExecFunction->GetReturnProperty()) == nullptr)
					return ECheckBoxState::Unchecked;

				bool bReturnValue = false;
				((UObject*)Extension)->ProcessEvent(ExecFunction, &bReturnValue);
				return bReturnValue ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			});
	}

	return Action;
}

struct FSortedFunction
{
	UFunction* Function;
	int SortOrder;

	bool operator<(const FSortedFunction& Other) const
	{
		if (SortOrder < Other.SortOrder)
			return true;
		if (SortOrder > Other.SortOrder)
			return false;
		return Function->GetName() < Other.Function->GetName();
	}
};

struct FFunctionCategory
{
	FString Name;
	TArray<FSortedFunction> Functions;
	TArray<FFunctionCategory> SubCategories;

	bool operator<(const FFunctionCategory& Other) const
	{
		return Name < Other.Name;
	}

	void Sort()
	{
		Functions.Sort();
		SubCategories.Sort();
		for (auto& Category : SubCategories)
			Category.Sort();
	}
};
                                                                                                          
static TSharedRef<const FToolMenuCategorySnapshot> MakeToolMenuCategorySnapshot(const FFunctionCategory& Category)
{
	TSharedRef<FToolMenuCategorySnapshot> Snapshot = MakeShared<FToolMenuCategorySnapshot>();
	Snapshot->Name = Category.Name;
	for (const FSortedFunction& Function : Category.Functions)
	{
		if (Function.Function != nullptr)
			Snapshot->FunctionNames.Add(Function.Function->GetFName());
	}
	for (const FFunctionCategory& SubCategory : Category.SubCategories)
		Snapshot->SubCategories.Add(MakeToolMenuCategorySnapshot(SubCategory));
	return Snapshot;
}
                                                                                                        

static TMap<FString, FFunctionCategory> SortFunctionsByCategory(TArray<UFunction*> Functions)
{
	const static FName NAME_Category(TEXT("Category"));
	const static FName NAME_SortOrder(TEXT("SortOrder"));
	TMap<FString, FFunctionCategory> FunctionCategories;

	for (UFunction* Function : Functions)
	{
		FString CategoryString = Function->GetMetaData(NAME_Category);

		TArray<FString> CategoryElements;
		CategoryString.ParseIntoArray(CategoryElements, TEXT("|"), true);

		if (CategoryElements.Num() == 0)
			CategoryElements.Add(TEXT(""));

		FSortedFunction SortFun;
		SortFun.Function = Function;

		FString SortOrder = Function->GetMetaData(NAME_SortOrder);
		if (!SortOrder.IsEmpty())
		{
			LexFromString(SortFun.SortOrder, *SortOrder);
		}
		else
		{
			UASFunction* ASFunction = Cast<UASFunction>(Function);
			if (ASFunction != nullptr)
				SortFun.SortOrder = -1000000 + ASFunction->GetSourceLineNumber();
			else
				SortFun.SortOrder = 0;
		}

		FFunctionCategory* CheckCategory = nullptr;
		for (int i = 0, Count = CategoryElements.Num(); i < Count; ++i)
		{
			FString ElementName = CategoryElements[i].TrimStartAndEnd();

			if (CheckCategory == nullptr)
			{
				CheckCategory = &FunctionCategories.FindOrAdd(ElementName);
				CheckCategory->Name = ElementName;
			}
			else
			{
				auto* SubCategory = CheckCategory->SubCategories.FindByPredicate([&](FFunctionCategory& Cat)
				{
					return Cat.Name == ElementName;
				});

				if (SubCategory == nullptr)
				{
					SubCategory = &CheckCategory->SubCategories.Emplace_GetRef();
					SubCategory->Name = ElementName;
				}

				CheckCategory = SubCategory;
			}
		}

		if (CheckCategory != nullptr)
			CheckCategory->Functions.Add(SortFun);
	}

	for (auto& Category : FunctionCategories)
		Category.Value.Sort();
	return FunctionCategories;
}

void UScriptEditorMenuExtension::BuildMenu(class FMenuBuilder& MenuBuilder, TArray<UFunction*> Functions, FExtenderSelection Selection) const
{
	if (bAddSeparatorBeforeOptions)
		MenuBuilder.AddSeparator();
	if (!MenuSectionHeader.IsEmpty())
		MenuBuilder.BeginSection(GetClass()->GetFName(), MenuSectionHeader);

	TMap<FString, FFunctionCategory> FunctionCategories = SortFunctionsByCategory(Functions);

	auto AddEntriesWithCategory = [&](FMenuBuilder& SubMenu, const FFunctionCategory& Category)
	{
		// Add entries for functions with no category
		for (auto FunctionElem : Category.Functions)
			AddMenuEntry(SubMenu, FunctionElem.Function, Selection);
	};

	struct FSubMenuHelper
	{
		static void AddSubMenuForCategory(const UScriptEditorMenuExtension* Extension, FExtenderSelection Selection, FMenuBuilder& SubMenu, const FFunctionCategory& Category)
		{
			SubMenu.AddSubMenu(
				FText::FromString(Category.Name),
				FText(),
				FNewMenuDelegate::CreateLambda([Extension, Category, Selection](FMenuBuilder& SubSubMenu)
				{
					for (auto FunctionElem : Category.Functions)
						Extension->AddMenuEntry(SubSubMenu, FunctionElem.Function, Selection);

					for (const FFunctionCategory& SubCategoryElem : Category.SubCategories)
						AddSubMenuForCategory(Extension, Selection, SubSubMenu, SubCategoryElem);
				}),
				false,
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Event_16x")
			);
		}
	};

	// Add submenus for categories
	for (auto& Category : FunctionCategories)
	{
		if (Category.Key == TEXT(""))
			AddEntriesWithCategory(MenuBuilder, Category.Value);
		else
			FSubMenuHelper::AddSubMenuForCategory(this, Selection, MenuBuilder, Category.Value);
	}

	if (!MenuSectionHeader.IsEmpty())
		MenuBuilder.EndSection();
	if (bAddSeparatorAfterOptions)
		MenuBuilder.AddSeparator();
}

void UScriptEditorMenuExtension::BuildToolMenuSection(FToolMenuSection& MenuSection, bool bIsMenu) const
{
	FEditorScriptExecutionGuard ScriptGuard;
	FExtenderSelection Selection = GetSelectionFromToolMenuContext(MenuSection.Context);
	TGuardValue<FExtenderSelection> ScopeSelection(CurrentSelection, Selection);

	{
		TGuardValue<const FToolMenuContext*> ScopeActiveContext(ActiveToolMenuContext, &MenuSection.Context);
		if (!ShouldExtend())
			return;
	}

	TArray<UFunction*> Functions = GatherExtensionFunctions();
	if (Functions.Num() == 0)
		return;

                                                                                                                  
	#if 0
                                                                                                                
	TMap<FString, FFunctionCategory> FunctionCategories = SortFunctionsByCategory(Functions);

	if (bAddSeparatorBeforeOptions)
		MenuSection.AddSeparator(NAME_None);

	auto AddEntriesWithCategory = [&](FToolMenuSection& SubMenu, const FFunctionCategory& Category)
	{
		// Add entries for functions with no category
		for (auto FunctionElem : Category.Functions)
		{
			if (bIsMenu)
				AddToolMenuEntry(SubMenu, FunctionElem.Function);
			else
				AddToolMenuButton(SubMenu, FunctionElem.Function);
		}
	};

	struct FSubMenuHelper
	{
		static void AddSubMenuForCategory(const UScriptEditorMenuExtension* Extension, FExtenderSelection Selection, FToolMenuSection& SubMenu, const FFunctionCategory& Category)
		{
			SubMenu.AddSubMenu(
				*Category.Name,
				FText::FromString(Category.Name),
				FText(),
				FNewToolMenuDelegate::CreateLambda([Extension, Category, Selection](UToolMenu* SubSubMenu)
				{
					FToolMenuSection& SubSection = SubSubMenu->AddSection(NAME_None, FText());

					for (auto FunctionElem : Category.Functions)
						Extension->AddToolMenuEntry(SubSection, FunctionElem.Function);

					for (const FFunctionCategory& SubCategoryElem : Category.SubCategories)
						AddSubMenuForCategory(Extension, Selection, SubSection, SubCategoryElem);
				}),
				false,
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Event_16x")
			);
		}
	};

	// Add submenus for categories
	for (auto& Category : FunctionCategories)
	{
		if (Category.Key == TEXT(""))
		{
			AddEntriesWithCategory(MenuSection, Category.Value);
		}
		else
		{
			if (bIsMenu)
			{
				FSubMenuHelper::AddSubMenuForCategory(this, Selection, MenuSection, Category.Value);
			}
			else
			{
				MenuSection.AddEntry(FToolMenuEntry::InitComboButton(
					*Category.Value.Name,
					FUIAction(),
					FNewToolMenuDelegate::CreateLambda([Extension=this, Category=Category.Value, Selection](UToolMenu* SubSubMenu)
					{
						FToolMenuSection& SubSection = SubSubMenu->AddSection(NAME_None, FText());

						for (auto FunctionElem : Category.Functions)
							Extension->AddToolMenuEntry(SubSection, FunctionElem.Function);

						for (const FFunctionCategory& SubCategoryElem : Category.SubCategories)
							FSubMenuHelper::AddSubMenuForCategory(Extension, Selection, SubSection, SubCategoryElem);
					}),
					FText::FromString(Category.Value.Name),
					FText(),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Event_16x"),
					false
				));
			}
		}
	}

                                                                                                        
	#endif
	TMap<FString, FFunctionCategory> FunctionCategories = SortFunctionsByCategory(Functions);

	if (bAddSeparatorBeforeOptions)
		MenuSection.AddSeparator(NAME_None);

	const TWeakObjectPtr<UClass> ExtensionClass = GetClass();
	auto AddEntriesWithCategory =
		[ExtensionClass, bIsMenu](
			FToolMenuSection& SubMenu,
			const TSharedRef<const FToolMenuCategorySnapshot>& Category)
	{
		UScriptEditorMenuExtension* Extension = ResolveToolMenuExtension(ExtensionClass);
		if (Extension == nullptr)
			return;
		for (const FName FunctionName : Category->FunctionNames)
		{
			UFunction* Function = ResolveToolMenuActionFunction(ExtensionClass, FunctionName);
			if (Function == nullptr)
				continue;
			if (bIsMenu)
				Extension->AddToolMenuEntry(SubMenu, Function);
			else
				Extension->AddToolMenuButton(SubMenu, Function);
		}
	};

                                                                                                      
                                                                                                                
	struct FSubMenuHelper
	{
		static void AddSubMenuForCategory(
			TWeakObjectPtr<UClass> ExtensionClass,
			FToolMenuSection& SubMenu,
			const TSharedRef<const FToolMenuCategorySnapshot>& Category)
		{
			SubMenu.AddSubMenu(
				*Category->Name,
				FText::FromString(Category->Name),
				FText(),
				FNewToolMenuDelegate::CreateLambda([ExtensionClass, Category](UToolMenu* SubSubMenu)
				{
					UScriptEditorMenuExtension* Extension = ResolveToolMenuExtension(ExtensionClass);
					if (SubSubMenu == nullptr || Extension == nullptr)
						return;
					FToolMenuSection& SubSection = SubSubMenu->AddSection(NAME_None, FText());
					for (const FName FunctionName : Category->FunctionNames)
					{
						UFunction* Function = ResolveToolMenuActionFunction(ExtensionClass, FunctionName);
						if (Function != nullptr)
							Extension->AddToolMenuEntry(SubSection, Function);
					}
					for (const TSharedRef<const FToolMenuCategorySnapshot>& SubCategory : Category->SubCategories)
						AddSubMenuForCategory(ExtensionClass, SubSection, SubCategory);
				}),
				false,
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Event_16x")
			);
		}
	};

                                                                                                              
                                                                                                                
	for (const auto& Category : FunctionCategories)
	{
		TSharedRef<const FToolMenuCategorySnapshot> Snapshot = MakeToolMenuCategorySnapshot(Category.Value);
		if (Category.Key == TEXT(""))
			AddEntriesWithCategory(MenuSection, Snapshot);
		else if (bIsMenu)
			FSubMenuHelper::AddSubMenuForCategory(ExtensionClass, MenuSection, Snapshot);
		else
		{
			MenuSection.AddEntry(FToolMenuEntry::InitComboButton(
				*Snapshot->Name,
				FUIAction(),
				FNewToolMenuDelegate::CreateLambda([ExtensionClass, Snapshot](UToolMenu* SubSubMenu)
				{
					UScriptEditorMenuExtension* Extension = ResolveToolMenuExtension(ExtensionClass);
					if (SubSubMenu == nullptr || Extension == nullptr)
						return;
					FToolMenuSection& SubSection = SubSubMenu->AddSection(NAME_None, FText());
					for (const FName FunctionName : Snapshot->FunctionNames)
					{
						UFunction* Function = ResolveToolMenuActionFunction(ExtensionClass, FunctionName);
						if (Function != nullptr)
							Extension->AddToolMenuEntry(SubSection, Function);
					}
					for (const TSharedRef<const FToolMenuCategorySnapshot>& SubCategory : Snapshot->SubCategories)
						FSubMenuHelper::AddSubMenuForCategory(ExtensionClass, SubSection, SubCategory);
				}),
				FText::FromString(Snapshot->Name),
				FText(),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Event_16x"),
				false
			));
		}
	}
                                                                                                              
	if (bAddSeparatorAfterOptions)
		MenuSection.AddSeparator(NAME_None);
}

TMap<UASClass*, UScriptEditorMenuExtension::FRegisteredExtender> UScriptEditorMenuExtension::RegisteredExtensions;

void UScriptEditorMenuExtension::UnregisterExtensions()
{
	FLevelEditorModule& LevelEditorModule = FModuleManager::Get().LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

	// Unregisterall previous extensions
	for (auto& Elem : RegisteredExtensions)
		UnregisterExtension(Elem.Value);

	RegisteredExtensions.Reset();
}

void UScriptEditorMenuExtension::RegisterExtensions()
{
	FLevelEditorModule& LevelEditorModule = FModuleManager::Get().LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	FEditorScriptExecutionGuard ScriptGuard;

	// Register new extensions
	for (TObjectIterator<UASClass> ScriptClass; ScriptClass; ++ScriptClass)
	{
		if (!ScriptClass->IsChildOf(UScriptEditorMenuExtension::StaticClass()))
			continue;
		if (ScriptClass->HasAnyClassFlags(CLASS_Abstract))
			continue;
		if (ScriptClass->HasAnyClassFlags(CLASS_NewerVersionExists))
			continue;
                                                                                                                                     
		if (GetAngelscriptClassScriptType(*ScriptClass) == nullptr)
                                                                                                                                   
			continue;

		auto* CDO = ScriptClass->GetDefaultObject<UScriptEditorMenuExtension>();
		if (!CDO->ShouldRegisterExtension())
			continue;

		RegisterExtension(*ScriptClass);
	}
}

void UScriptEditorMenuExtension::RegisterExtension(UASClass* ScriptClass)
{
	FLevelEditorModule& LevelEditorModule = FModuleManager::Get().LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

	auto* CDO = ScriptClass->GetDefaultObject<UScriptEditorMenuExtension>();

	FRegisteredExtender Registered;
	Registered.Location = CDO->ExtensionMenu;
	Registered.ExtensionPoint = CDO->ExtensionPoint;

	switch (CDO->ExtensionMenu)
	{
	case EScriptEditorMenuExtensionLocation::LevelViewport_ContextMenu:
		LevelEditorModule.GetAllLevelViewportContextMenuExtenders().Add(
			FLevelEditorModule::FLevelViewportMenuExtender_SelectedActors::CreateLambda(
				[CDO](const TSharedRef<FUICommandList> CommandList, const TArray<AActor*> SelectedActors) -> TSharedRef<FExtender>
				{
					FExtenderSelection Selection;
					for (auto* Actor : SelectedActors)
						Selection.SelectedObjects.Add(Actor);
					return CDO->Extend(CommandList, Selection);
				}
			)
		);
		Registered.DelegateHandle = LevelEditorModule.GetAllLevelViewportContextMenuExtenders().Last().GetHandle();
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_DragDropContextMenu:
	{
		auto Delegate = FLevelEditorModule::FLevelViewportMenuExtender_SelectedObjects::CreateLambda(
			[CDO](const TSharedRef<FUICommandList> CommandList, const TArray<UObject*> SelectedObjects) -> TSharedRef<FExtender>
			{
				FExtenderSelection Selection;
				for (UObject* Object : SelectedObjects)
					Selection.SelectedObjects.Add(Object);
				return CDO->Extend(CommandList, Selection);
			}
		);
		LevelEditorModule.GetAllLevelViewportDragDropContextMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_OptionsMenu:
	{
		auto Delegate = FLevelEditorModule::FLevelEditorMenuExtender::CreateLambda(
			[CDO](const TSharedRef<FUICommandList> CommandList) -> TSharedRef<FExtender>
			{
				return CDO->Extend(CommandList, FExtenderSelection());
			}
		);
		LevelEditorModule.GetAllLevelViewportOptionsMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_ShowMenu:
	{
		auto Delegate = FLevelEditorModule::FLevelEditorMenuExtender::CreateLambda(
			[CDO](const TSharedRef<FUICommandList> CommandList) -> TSharedRef<FExtender>
			{
				return CDO->Extend(CommandList, FExtenderSelection());
			}
		);
		LevelEditorModule.GetAllLevelViewportShowMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_ViewMenu:
	{
		auto Delegate = FLevelEditorModule::FLevelEditorMenuExtender::CreateLambda(
			[CDO](const TSharedRef<FUICommandList> CommandList) -> TSharedRef<FExtender>
			{
				return CDO->Extend(CommandList, FExtenderSelection());
			}
		);
		LevelEditorModule.GetAllLevelEditorToolbarViewMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_BuildMenu:
	{
		auto Delegate = FLevelEditorModule::FLevelEditorMenuExtender::CreateLambda(
			[CDO](const TSharedRef<FUICommandList> CommandList) -> TSharedRef<FExtender>
			{
				return CDO->Extend(CommandList, FExtenderSelection());
			}
		);
		LevelEditorModule.GetAllLevelEditorToolbarBuildMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_CompileMenu:
	{
		auto Delegate = FLevelEditorModule::FLevelEditorMenuExtender::CreateLambda(
			[CDO](const TSharedRef<FUICommandList> CommandList) -> TSharedRef<FExtender>
			{
				return CDO->Extend(CommandList, FExtenderSelection());
			}
		);
		LevelEditorModule.GetAllLevelEditorToolbarCompileMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_SourceControlMenu:
	{
		auto Delegate = FLevelEditorModule::FLevelEditorMenuExtender::CreateLambda(
			[CDO](const TSharedRef<FUICommandList> CommandList) -> TSharedRef<FExtender>
			{
				return CDO->Extend(CommandList, FExtenderSelection());
			}
		);
		LevelEditorModule.GetAllLevelEditorToolbarSourceControlMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_CreateMenu:
	{
		auto Delegate = FLevelEditorModule::FLevelEditorMenuExtender::CreateLambda(
			[CDO](const TSharedRef<FUICommandList> CommandList) -> TSharedRef<FExtender>
			{
				return CDO->Extend(CommandList, FExtenderSelection());
			}
		);
		LevelEditorModule.GetAllLevelEditorToolbarCreateMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_PlayMenu:
	{
		auto Delegate = FLevelEditorModule::FLevelEditorMenuExtender::CreateLambda(
			[CDO](const TSharedRef<FUICommandList> CommandList) -> TSharedRef<FExtender>
			{
				return CDO->Extend(CommandList, FExtenderSelection());
			}
		);
		LevelEditorModule.GetAllLevelEditorToolbarPlayMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_BlueprintsMenu:
	{
		Registered.Extender = CDO->Extend(MakeShared<FUICommandList>(), FExtenderSelection());
		LevelEditorModule.GetAllLevelEditorToolbarBlueprintsMenuExtenders().Add(Registered.Extender);
	}
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_CinematicsMenu:
	{
		Registered.Extender = CDO->Extend(MakeShared<FUICommandList>(), FExtenderSelection());
		LevelEditorModule.GetAllLevelEditorToolbarCinematicsMenuExtenders().Add(Registered.Extender);
	}
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_LevelMenu:
	{
		auto Delegate = FLevelEditorModule::FLevelEditorMenuExtender::CreateLambda(
			[CDO](const TSharedRef<FUICommandList> CommandList) -> TSharedRef<FExtender>
			{
				return CDO->Extend(CommandList, FExtenderSelection());
			}
		);
		LevelEditorModule.GetAllLevelEditorLevelMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::ContentBrowser_AssetContextMenu:
	{
		auto Delegate = FContentBrowserMenuExtender_SelectedPaths::CreateLambda(
			[CDO](const TArray<FString>& Paths) -> TSharedRef<FExtender>
			{
				return CDO->Extend(MakeShared<FUICommandList>(), FExtenderSelection());
			}
		);
		ContentBrowserModule.GetAllAssetContextMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::ContentBrowser_PathViewContextMenu:
	{
		auto Delegate = FContentBrowserMenuExtender_SelectedPaths::CreateLambda(
			[CDO](const TArray<FString>& Paths) -> TSharedRef<FExtender>
			{
				return CDO->Extend(MakeShared<FUICommandList>(), FExtenderSelection());
			}
		);
		ContentBrowserModule.GetAllPathViewContextMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::ContentBrowser_CollectionListContextMenu:
	{
		auto Delegate = FContentBrowserMenuExtender::CreateLambda(
			[CDO]() -> TSharedRef<FExtender>
			{
				return CDO->Extend(MakeShared<FUICommandList>(), FExtenderSelection());
			}
		);
		ContentBrowserModule.GetAllCollectionListContextMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::ContentBrowser_CollectionViewContextMenu:
	{
		auto Delegate = FContentBrowserMenuExtender::CreateLambda(
			[CDO]() -> TSharedRef<FExtender>
			{
				return CDO->Extend(MakeShared<FUICommandList>(), FExtenderSelection());
			}
		);
		ContentBrowserModule.GetAllCollectionViewContextMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::ContentBrowser_AssetViewContextMenu:
	{
		auto Delegate = FContentBrowserMenuExtender_SelectedAssets::CreateLambda(
			[CDO](const TArray<FAssetData>& Assets) -> TSharedRef<FExtender>
			{
				FExtenderSelection Selection;
				Selection.SelectedAssets = Assets;
				return CDO->Extend(MakeShared<FUICommandList>(), Selection);
			}
		);
		ContentBrowserModule.GetAllAssetViewContextMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::ContentBrowser_AssetViewViewMenu:
	{
		auto Delegate = FContentBrowserMenuExtender::CreateLambda(
			[CDO]() -> TSharedRef<FExtender>
			{
				return CDO->Extend(MakeShared<FUICommandList>(), FExtenderSelection());
			}
		);
		ContentBrowserModule.GetAllAssetViewViewMenuExtenders().Add(Delegate);
		Registered.DelegateHandle = Delegate.GetHandle();
	}
	break;
	case EScriptEditorMenuExtensionLocation::ToolMenu:
	{
		Registered.SectionName = CDO->GetClass()->GetFName();

		auto* Menu = UToolMenus::Get()->ExtendMenu(Registered.ExtensionPoint);
		FText SectionHeader = CDO->MenuSectionHeader;
		if (SectionHeader.IsEmpty())
			SectionHeader = CDO->GetClass()->GetDisplayNameText();

		FToolMenuInsert InsertPosition;
		InsertPosition.Position = CDO->ToolMenuInsertType;
		InsertPosition.Name = CDO->ToolMenuInsertPosition;

		FToolMenuSection& Section = Menu->AddSection(Registered.SectionName, SectionHeader, InsertPosition);
		Section.Alignment = CDO->ToolMenuSectionAlign;
                                                                                                                
		#if 0
                                                                                                              
		Section.AddDynamicEntry(Registered.SectionName, FNewToolMenuSectionDelegate::CreateLambda([CDO, Menu](FToolMenuSection& DynamicSection)
		{
			bool bIsMenu = (Menu->MenuType == EMultiBoxType::MenuBar) || (Menu->MenuType == EMultiBoxType::Menu);
			CDO->BuildToolMenuSection(DynamicSection, bIsMenu);
		}));
                                                                                                      
		#endif
		const TWeakObjectPtr<UClass> ExtensionClass = ScriptClass;
		const bool bIsMenu = (Menu->MenuType == EMultiBoxType::MenuBar) || (Menu->MenuType == EMultiBoxType::Menu);
		Section.AddDynamicEntry(
			Registered.SectionName,
			FNewToolMenuSectionDelegate::CreateLambda(
				[ExtensionClass, bIsMenu](FToolMenuSection& DynamicSection)
		{
			UScriptEditorMenuExtension* Extension = ResolveToolMenuExtension(ExtensionClass);
			if (Extension == nullptr)
				return;
			Extension->BuildToolMenuSection(DynamicSection, bIsMenu);
		}));
                                                                                                    
	}
	break;
	}

	RegisteredExtensions.Add(ScriptClass, Registered);
}

void UScriptEditorMenuExtension::UnregisterExtension(FRegisteredExtender& Extension)
{
	FLevelEditorModule& LevelEditorModule = FModuleManager::Get().LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

	switch (Extension.Location)
	{
	case EScriptEditorMenuExtensionLocation::LevelViewport_ContextMenu:
		LevelEditorModule.GetAllLevelViewportContextMenuExtenders().RemoveAll(
			[&](const FLevelEditorModule::FLevelViewportMenuExtender_SelectedActors& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_DragDropContextMenu:
		LevelEditorModule.GetAllLevelViewportDragDropContextMenuExtenders().RemoveAll(
			[&](const FLevelEditorModule::FLevelViewportMenuExtender_SelectedObjects& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_OptionsMenu:
		LevelEditorModule.GetAllLevelViewportOptionsMenuExtenders().RemoveAll(
			[&](const FLevelEditorModule::FLevelEditorMenuExtender& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_ShowMenu:
		LevelEditorModule.GetAllLevelViewportShowMenuExtenders().RemoveAll(
			[&](const FLevelEditorModule::FLevelEditorMenuExtender& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_ViewMenu:
		LevelEditorModule.GetAllLevelEditorToolbarViewMenuExtenders().RemoveAll(
			[&](const FLevelEditorModule::FLevelEditorMenuExtender& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_BuildMenu:
		LevelEditorModule.GetAllLevelEditorToolbarBuildMenuExtenders().RemoveAll(
			[&](const FLevelEditorModule::FLevelEditorMenuExtender& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_CompileMenu:
		LevelEditorModule.GetAllLevelEditorToolbarCompileMenuExtenders().RemoveAll(
			[&](const FLevelEditorModule::FLevelEditorMenuExtender& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_SourceControlMenu:
		LevelEditorModule.GetAllLevelEditorToolbarSourceControlMenuExtenders().RemoveAll(
			[&](const FLevelEditorModule::FLevelEditorMenuExtender& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_CreateMenu:
		LevelEditorModule.GetAllLevelEditorToolbarCreateMenuExtenders().RemoveAll(
			[&](const FLevelEditorModule::FLevelEditorMenuExtender& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_PlayMenu:
		LevelEditorModule.GetAllLevelEditorToolbarPlayMenuExtenders().RemoveAll(
			[&](const FLevelEditorModule::FLevelEditorMenuExtender& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_BlueprintsMenu:
		LevelEditorModule.GetAllLevelEditorToolbarBlueprintsMenuExtenders().RemoveAll(
			[&](const TSharedPtr<FExtender>& Extender)
			{
				return Extender == Extension.Extender;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_CinematicsMenu:
		LevelEditorModule.GetAllLevelEditorToolbarCinematicsMenuExtenders().RemoveAll(
			[&](const TSharedPtr<FExtender>& Extender)
			{
				return Extender == Extension.Extender;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::LevelViewport_LevelMenu:
		LevelEditorModule.GetAllLevelEditorLevelMenuExtenders().RemoveAll(
			[&](const FLevelEditorModule::FLevelEditorMenuExtender& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::ContentBrowser_AssetContextMenu:
		ContentBrowserModule.GetAllAssetContextMenuExtenders().RemoveAll(
			[&](const FContentBrowserMenuExtender_SelectedPaths& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::ContentBrowser_PathViewContextMenu:
		ContentBrowserModule.GetAllPathViewContextMenuExtenders().RemoveAll(
			[&](const FContentBrowserMenuExtender_SelectedPaths& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::ContentBrowser_CollectionListContextMenu:
		ContentBrowserModule.GetAllCollectionListContextMenuExtenders().RemoveAll(
			[&](const FContentBrowserMenuExtender& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::ContentBrowser_CollectionViewContextMenu:
		ContentBrowserModule.GetAllCollectionViewContextMenuExtenders().RemoveAll(
			[&](const FContentBrowserMenuExtender& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::ContentBrowser_AssetViewContextMenu:
		ContentBrowserModule.GetAllAssetViewContextMenuExtenders().RemoveAll(
			[&](const FContentBrowserMenuExtender_SelectedAssets& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::ContentBrowser_AssetViewViewMenu:
		ContentBrowserModule.GetAllAssetViewViewMenuExtenders().RemoveAll(
			[&](const FContentBrowserMenuExtender& Value)
			{
				return Value.GetHandle() == Extension.DelegateHandle;
			}
		);
	break;
	case EScriptEditorMenuExtensionLocation::ToolMenu:
	{
		auto* Menu = UToolMenus::Get()->ExtendMenu(Extension.ExtensionPoint);
		Menu->RemoveSection(Extension.SectionName);
	}
	break;
	}
}

void UScriptEditorMenuExtension::RefreshExtensionRegistration()
{
	FEditorScriptExecutionGuard ScriptGuard;

	// Register new extensions
	for (TObjectIterator<UASClass> ScriptClass; ScriptClass; ++ScriptClass)
	{
		if (!ScriptClass->IsChildOf(UScriptEditorMenuExtension::StaticClass()))
			continue;
		if (ScriptClass->HasAnyClassFlags(CLASS_Abstract))
			continue;
		if (ScriptClass->HasAnyClassFlags(CLASS_NewerVersionExists))
			continue;
                                                                                                                                    
		if (GetAngelscriptClassScriptType(*ScriptClass) == nullptr)
                                                                                                                                  
			continue;

		auto* CDO = ScriptClass->GetDefaultObject<UScriptEditorMenuExtension>();
		if (CDO->ShouldRegisterExtension())
		{
			// If we weren't registered already, do so now
			if (!RegisteredExtensions.Contains(*ScriptClass))
			{
				RegisterExtension(*ScriptClass);
			}
		}
		else
		{
			// If we were registered, remove that registration
			FRegisteredExtender* RegisteredExtender = RegisteredExtensions.Find(*ScriptClass);
			if (RegisteredExtender != nullptr)
			{
				UnregisterExtension(*RegisteredExtender);
				RegisteredExtensions.Remove(*ScriptClass);
			}
		}
	}
}
