#include "DetailWidgetRow.h"
#include "EditorClassUtils.h"
#include "BlueprintEditor.h"
#include "BlueprintEditorModule.h"
#include "ClassViewerFilter.h"
#include "ClassViewerModule.h"
#include "Editor/EditorEngine.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "IDetailsView.h"
#include "IDetailTreeNode.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "SourceCodeNavigation.h"
#include "Styling/AppStyle.h"
#include "Shared/SoloASScriptBlueprint.h"
#include "Editor/SoloASScriptBlueprintEditorInternal.h"
#include "Editor/SoloASEditorExperienceImports.h"
#include "Editor/SoloASBridgeCoreRedirectClassificationHook.h"
#include "UObject/ConstructorHelpers.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "Editor/SoloASBridgeOutlinerScriptSourceLinkHook.h"
#include "Editor/SoloASBridgeListViewEntryClassHook.h"
#include "Editor/SoloASBridgeScriptFunctionSourceNavigationHook.h"
#endif

namespace
{
FDelegateHandle GGameModeNavigationHandle;
FDelegateHandle GScriptParentSourceOverlayHandle;
FDelegateHandle GScriptParentOverlayPostEngineInitHandle;
FDelegateHandle GScriptParentCompileHandle;

class FRegisteredScriptParentPickerFilter final : public IClassViewerFilter
{
public:
    explicit FRegisteredScriptParentPickerFilter(UBlueprint* InBlueprint)
        : Blueprint(InBlueprint)
    {
        UClass* Class = InBlueprint != nullptr ? InBlueprint->ParentClass : nullptr;
        while (Class != nullptr && !Class->HasAnyClassFlags(CLASS_Native))
        {
            Class = Class->GetSuperClass();
        }
        ParentBaseClass = Class != nullptr ? Class : UObject::StaticClass();
        if (InBlueprint != nullptr)
        {
            InBlueprint->GetReparentingRules(AllowedChildren, DisallowedChildren);
        }
    }

    virtual bool IsClassAllowed(
        const FClassViewerInitializationOptions&,
        const UClass* Candidate,
        TSharedRef<FClassViewerFilterFuncs>) override
    {
        const UBlueprint* CurrentBlueprint = Blueprint.Get();
        if (CurrentBlueprint == nullptr || Candidate == nullptr
            || !SoloASBridge::IsRegisteredScriptClass(Candidate)
            || !Candidate->IsChildOf(ParentBaseClass)
            || Candidate->HasAnyClassFlags(
                CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
            || (CurrentBlueprint->GeneratedClass != nullptr
                && Candidate->IsChildOf(CurrentBlueprint->GeneratedClass))
            || !FKismetEditorUtilities::CanCreateBlueprintOfClass(Candidate))
        {
            return false;
        }

        if (AllowedChildren.Num() > 0)
        {
            bool bMatchesAllowedParent = false;
            for (const UClass* AllowedParent : AllowedChildren)
            {
                bMatchesAllowedParent |= AllowedParent != nullptr
                    && Candidate->IsChildOf(AllowedParent);
            }
            if (!bMatchesAllowedParent)
            {
                return false;
            }
        }
        for (const UClass* DisallowedParent : DisallowedChildren)
        {
            if (DisallowedParent != nullptr && Candidate->IsChildOf(DisallowedParent))
            {
                return false;
            }
        }
        return true;
    }

    virtual bool IsUnloadedClassAllowed(
        const FClassViewerInitializationOptions&,
        const TSharedRef<const IUnloadedBlueprintData>,
        TSharedRef<FClassViewerFilterFuncs>) override
    {
        return false;
    }

private:
    TWeakObjectPtr<UBlueprint> Blueprint;
    UClass* ParentBaseClass = nullptr;
    TSet<const UClass*> AllowedChildren;
    TSet<const UClass*> DisallowedChildren;
};

TSharedPtr<FBlueprintEditor> FindBlueprintEditor(UBlueprint* Blueprint)
{
    FBlueprintEditorModule* BlueprintEditorModule =
        FModuleManager::GetModulePtr<FBlueprintEditorModule>(TEXT("Kismet"));
    if (Blueprint == nullptr || BlueprintEditorModule == nullptr)
    {
        return nullptr;
    }
    for (const TSharedRef<IBlueprintEditor>& EditorInterface :
         BlueprintEditorModule->GetBlueprintEditors())
    {
        TSharedRef<FBlueprintEditor> Editor =
            StaticCastSharedRef<FBlueprintEditor>(EditorInterface);
        if (Editor->GetBlueprintObj() == Blueprint)
        {
            return Editor.ToSharedPtr();
        }
    }
    return nullptr;
}

void OpenScriptParentPicker(const TWeakObjectPtr<UBlueprint>& WeakBlueprint)
{
    UBlueprint* Blueprint = WeakBlueprint.Get();
    if (Blueprint == nullptr)
    {
        return;
    }

    FClassViewerInitializationOptions Options;
    Options.Mode = EClassViewerMode::ClassPicker;
    Options.bShowBackgroundBorder = false;
    Options.bIsBlueprintBaseOnly = false;
    Options.bShowUnloadedBlueprints = false;
    Options.ClassFilters.Add(MakeShared<FRegisteredScriptParentPickerFilter>(Blueprint));

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(NSLOCTEXT("SoloASBridgeEditor", "SelectScriptParentTitle", "Select Script Parent"))
        .ClientSize(FVector2D(420.0f, 560.0f))
        .SupportsMinimize(false)
        .SupportsMaximize(false);
    const TWeakPtr<SWindow> WeakWindow = Window;
    FOnClassPicked OnClassPicked = FOnClassPicked::CreateLambda(
        [WeakBlueprint, WeakWindow](UClass* SelectedClass)
        {
            if (UBlueprint* CurrentBlueprint = WeakBlueprint.Get())
            {
                if (TSharedPtr<FBlueprintEditor> Editor = FindBlueprintEditor(CurrentBlueprint))
                {
                    Editor->ReparentBlueprint_NewParentChosen(SelectedClass);
                }
            }
            if (TSharedPtr<SWindow> CurrentWindow = WeakWindow.Pin())
            {
                CurrentWindow->RequestDestroyWindow();
            }
        });

    FClassViewerModule& ClassViewerModule =
        FModuleManager::LoadModuleChecked<FClassViewerModule>(TEXT("ClassViewer"));
    Window->SetContent(ClassViewerModule.CreateClassViewer(Options, OnClassPicked));
    FSlateApplication::Get().AddModalWindow(
        Window, FGlobalTabmanager::Get()->GetRootWindow());
}

void ApplyScriptParentSourceOverlay(const TSharedPtr<FBlueprintEditor>& BlueprintEditor);

void RefreshScriptParentOverlaysOnCompile()
{
    FBlueprintEditorModule* BlueprintEditorModule =
        FModuleManager::GetModulePtr<FBlueprintEditorModule>(TEXT("Kismet"));
    if (BlueprintEditorModule == nullptr)
    {
        return;
    }

    for (const TSharedRef<IBlueprintEditor>& EditorInterface :
         BlueprintEditorModule->GetBlueprintEditors())
    {
        TSharedRef<FBlueprintEditor> BlueprintEditor =
            StaticCastSharedRef<FBlueprintEditor>(EditorInterface);
        if (BlueprintEditor->GetBlueprintObj() != nullptr
            && SoloASBridge::IsRegisteredScriptClass(
                BlueprintEditor->GetBlueprintObj()->ParentClass))
        {
            ApplyScriptParentSourceOverlay(BlueprintEditor);
        }
    }
}

void ApplyScriptParentSourceOverlay(
    const TSharedPtr<FBlueprintEditor>& BlueprintEditor)
{
    if (!BlueprintEditor.IsValid())
    {
        return;
    }
    UBlueprint* Blueprint = BlueprintEditor->GetBlueprintObj();
    if (Blueprint == nullptr
        || !SoloASBridge::IsRegisteredScriptClass(Blueprint->ParentClass))
    {
        return;
    }
    const TWeakObjectPtr<UBlueprint> WeakBlueprint(Blueprint);
    TSharedRef<SHorizontalBox> ParentOverlay = SNew(SHorizontalBox)
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
            SNew(STextBlock)
            .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            .ShadowOffset(FVector2D::UnitVector)
            .Text(NSLOCTEXT("SoloASBridgeEditor", "ParentClassLabel", "Parent class: "))
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
            SNew(SSpacer)
            .Size(FVector2D(2.0f, 1.0f))
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
            SNew(SHyperlink)
            .Style(FAppStyle::Get(), "Common.GotoNativeCodeHyperlink")
            .IsEnabled_Lambda([WeakBlueprint]()
            {
                const UBlueprint* CurrentBlueprint = WeakBlueprint.Get();
                return CurrentBlueprint != nullptr
                    && FSourceCodeNavigation::CanNavigateToClass(CurrentBlueprint->ParentClass);
            })
            .Visibility(EVisibility::Visible)
            .OnNavigate_Lambda([WeakBlueprint]()
            {
                if (UBlueprint* CurrentBlueprint = WeakBlueprint.Get())
                {
                    FSourceCodeNavigation::NavigateToClass(CurrentBlueprint->ParentClass);
                }
            })
            .Text_Lambda([WeakBlueprint]()
            {
                const UBlueprint* CurrentBlueprint = WeakBlueprint.Get();
                return CurrentBlueprint != nullptr && CurrentBlueprint->ParentClass != nullptr
                    ? CurrentBlueprint->ParentClass->GetDisplayNameText() : FText::GetEmpty();
            })
            .ToolTipText(NSLOCTEXT(
                "SoloASBridgeEditor", "ParentClassSourceTooltip",
                "Open the parent class source file."))
        ]
        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
            SNew(SSpacer)
            .Size(FVector2D(8.0f, 1.0f))
        ];
    BlueprintEditor->SetMenuOverlay(ParentOverlay);
}

void InstallScriptParentSourceOverlay(
    FWorkflowAllowedTabSet&,
    FName,
    TSharedPtr<FBlueprintEditor> BlueprintEditor)
{
    ApplyScriptParentSourceOverlay(BlueprintEditor);
}

void RegisterScriptParentSourceOverlay()
{
    if (GScriptParentOverlayPostEngineInitHandle.IsValid())
    {
        FCoreDelegates::GetOnPostEngineInit().Remove(
            GScriptParentOverlayPostEngineInitHandle);
        GScriptParentOverlayPostEngineInitHandle.Reset();
    }
    if (!GIsEditor || GEditor == nullptr
        || GScriptParentSourceOverlayHandle.IsValid())
    {
        return;
    }

    FBlueprintEditorModule& BlueprintEditorModule =
        FModuleManager::LoadModuleChecked<FBlueprintEditorModule>("Kismet");
    GScriptParentSourceOverlayHandle =
        BlueprintEditorModule.OnRegisterTabsForEditor().AddStatic(
            &InstallScriptParentSourceOverlay);
    if (!GScriptParentCompileHandle.IsValid())
    {
        GScriptParentCompileHandle = GEditor->OnBlueprintCompiled().AddStatic(
            &RefreshScriptParentOverlaysOnCompile);
    }
}

const FName DefaultClassProperties[] = {
    GET_MEMBER_NAME_CHECKED(AGameModeBase, DefaultPawnClass),
    GET_MEMBER_NAME_CHECKED(AGameModeBase, HUDClass),
    GET_MEMBER_NAME_CHECKED(AGameModeBase, PlayerControllerClass),
    GET_MEMBER_NAME_CHECKED(AGameModeBase, GameStateClass),
    GET_MEMBER_NAME_CHECKED(AGameModeBase, PlayerStateClass),
    GET_MEMBER_NAME_CHECKED(AGameModeBase, SpectatorClass),
};

bool IsNavigableScriptClass(const UClass* Class, const UClass* RequiredBase)
{
    return Class != nullptr && RequiredBase != nullptr
        && Class->IsChildOf(RequiredBase)
        && !Class->HasAnyClassFlags(CLASS_Native)
        && Class->ClassGeneratedBy == nullptr
        && FSourceCodeNavigation::CanNavigateToClass(Class);
}

bool IsGameModeProperty(const TSharedPtr<IPropertyHandle>& Handle)
{
    if (!Handle.IsValid() || Handle->GetProperty() == nullptr)
    {
        return false;
    }
    TArray<UObject*> OuterObjects;
    Handle->GetOuterObjects(OuterObjects);
    if (OuterObjects.Num() != 1 || !OuterObjects[0])
    {
        return false;
    }
    const FName PropertyName = Handle->GetProperty()->GetFName();
    const UClass* OwnerClass = Handle->GetProperty()->GetOwnerClass();
    return (OwnerClass != nullptr
            && OwnerClass->GetPathName()
                == TEXT("/Script/EngineSettings.GameMapsSettings")
            && OuterObjects[0]->IsA(OwnerClass)
            && PropertyName == FName(TEXT("GlobalDefaultGameMode")))
        || (OwnerClass == AWorldSettings::StaticClass()
            && OuterObjects[0]->IsA<AWorldSettings>()
            && PropertyName == GET_MEMBER_NAME_CHECKED(
                AWorldSettings, DefaultGameMode));
}

UClass* ResolveNavigableGameMode(const TSharedPtr<IPropertyHandle>& Handle)
{
    if (GIsSavingPackage || !IsGameModeProperty(Handle))
    {
        return nullptr;
    }
    FString ClassName;
    if (Handle->GetValueAsFormattedString(ClassName) != FPropertyAccess::Success)
    {
        return nullptr;
    }
    ConstructorHelpers::StripObjectClass(ClassName);
    UClass* Class = FEditorClassUtils::GetClassFromString(ClassName);
    if (!IsNavigableScriptClass(Class, AGameModeBase::StaticClass()))
    {
        return nullptr;
    }
    return Class;
}

UObject* GetSingleDetailsObject(const TWeakPtr<IDetailTreeNode>& WeakNode)
{
    const TSharedPtr<IDetailTreeNode> Node = WeakNode.Pin();
    const TSharedPtr<IDetailsView> Details = Node.IsValid()
        ? Node->GetNodeDetailsViewSharedPtr() : nullptr;
    if (!Details.IsValid() || Details->GetSelectedObjects().Num() != 1)
    {
        return nullptr;
    }
    return Details->GetSelectedObjects()[0].Get();
}

FProperty* GetSelectedGameModeProperty(UObject* Object)
{
    if (Object == nullptr)
    {
        return nullptr;
    }
    if (Object->IsA<AWorldSettings>())
    {
        return FindFProperty<FProperty>(
            AWorldSettings::StaticClass(),
            GET_MEMBER_NAME_CHECKED(AWorldSettings, DefaultGameMode));
    }
    const UClass* OwnerClass = Object->GetClass();
    if (OwnerClass->GetPathName()
        != TEXT("/Script/EngineSettings.GameMapsSettings"))
    {
        return nullptr;
    }
    return FindFProperty<FProperty>(OwnerClass, TEXT("GlobalDefaultGameMode"));
}

UClass* ResolveSelectedGameMode(UObject* Object)
{
    if (GIsSavingPackage)
    {
        return nullptr;
    }
    FProperty* Property = GetSelectedGameModeProperty(Object);
    if (Property == nullptr)
    {
        return nullptr;
    }
    FString ClassName;
    if (!Property->ExportText_InContainer(
        0, ClassName, Object, nullptr, Object, PPF_None))
    {
        return nullptr;
    }
    ConstructorHelpers::StripObjectClass(ClassName);
    UClass* Class = FEditorClassUtils::GetClassFromString(ClassName);
    return Class != nullptr && Class->IsChildOf(AGameModeBase::StaticClass())
        ? Class : nullptr;
}

FName FindDefaultClassProperty(const TWeakPtr<IDetailTreeNode>& WeakNode)
{
    const TSharedPtr<IDetailTreeNode> Node = WeakNode.Pin();
    if (!Node.IsValid())
    {
        return NAME_None;
    }
    const FNodeWidgets Widgets = Node->CreateNodeWidgets();
    if (!Widgets.NameWidget.IsValid()
        || Widgets.NameWidget->GetType() != FName(TEXT("STextBlock")))
    {
        return NAME_None;
    }
    const FText& Label = StaticCastSharedPtr<STextBlock>(
        Widgets.NameWidget)->GetText();
    FName Match = NAME_None;
    for (const FName PropertyName : DefaultClassProperties)
    {
        const FClassProperty* Property = FindFProperty<FClassProperty>(
            AGameModeBase::StaticClass(), PropertyName);
        if (Property == nullptr)
        {
            continue;
        }
        FString DisplayName = Property->GetDisplayNameText().ToString();
        if (DisplayName.IsEmpty())
        {
            DisplayName = Property->GetName();
        }
        DisplayName = FName::NameToDisplayString(DisplayName, false);
        if (Label.EqualTo(FText::FromString(DisplayName)))
        {
            if (!Match.IsNone())
            {
                return NAME_None;
            }
            Match = PropertyName;
        }
    }
    return Match;
}

UClass* ResolveNavigableDefaultClass(
    const TWeakPtr<IDetailTreeNode>& WeakNode, FName PropertyName)
{
    UObject* Object = GetSingleDetailsObject(WeakNode);
    UClass* GameModeClass = ResolveSelectedGameMode(Object);
    FClassProperty* Property = FindFProperty<FClassProperty>(
        AGameModeBase::StaticClass(), PropertyName);
    if (GameModeClass == nullptr || Property == nullptr)
    {
        return nullptr;
    }
    const AGameModeBase* DefaultObject =
        GameModeClass->GetDefaultObject<AGameModeBase>();
    UClass* Class = Cast<UClass>(
        Property->GetObjectPropertyValue_InContainer(DefaultObject));
    return IsNavigableScriptClass(Class, Property->MetaClass) ? Class : nullptr;
}

FPropertyRowExtensionButton MakeNavigationButton(
    FExecuteAction Execute, FCanExecuteAction CanExecute,
    FIsActionButtonVisible IsVisible)
{
    FPropertyRowExtensionButton Button;
    Button.Icon = FSourceCodeNavigation::GetOpenSourceCodeIDEIcon();
    Button.Label = NSLOCTEXT(
        "SoloASBridgeEditor", "OpenGameModeScript", "Open Script Source");
    Button.ToolTip = NSLOCTEXT(
        "SoloASBridgeEditor", "OpenGameModeScriptTooltip",
        "Open this AngelScript game mode class in the source editor.");
    Button.UIAction = FUIAction(
        MoveTemp(Execute), MoveTemp(CanExecute), FIsActionChecked(),
        MoveTemp(IsVisible));
    return Button;
}

FPropertyRowExtensionButton MakeScriptParentPickerButton(
    const TWeakObjectPtr<UBlueprint>& WeakBlueprint)
{
    FPropertyRowExtensionButton Button = MakeNavigationButton(
        FExecuteAction::CreateLambda([WeakBlueprint]()
        {
            OpenScriptParentPicker(WeakBlueprint);
        }),
        FCanExecuteAction::CreateLambda([WeakBlueprint]()
        {
            return WeakBlueprint.IsValid();
        }),
        FIsActionButtonVisible::CreateLambda([WeakBlueprint]()
        {
            return WeakBlueprint.IsValid();
        }));
    Button.Icon = FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search");
    Button.Label = NSLOCTEXT(
        "SoloASBridgeEditor", "ChooseScriptParent", "Choose Script Parent");
    Button.ToolTip = NSLOCTEXT(
        "SoloASBridgeEditor", "ChooseScriptParentTooltip",
        "Reparent this Blueprint to an AngelScript class.");
    return Button;
}

void AddGameModeNavigationButton(
    const FOnGenerateGlobalRowExtensionArgs& Args,
    TArray<FPropertyRowExtensionButton>& OutExtensions)
{
    UObject* DetailsObject = GetSingleDetailsObject(Args.OwnerTreeNode);
    if (UBlueprint* Blueprint = Cast<UBlueprint>(DetailsObject))
    {
        const TSharedPtr<IDetailTreeNode> OwnerNode = Args.OwnerTreeNode.Pin();
        FName PropertyName = OwnerNode.IsValid()
            ? OwnerNode->GetNodeName() : NAME_None;
        if (Args.PropertyHandle.IsValid()
            && Args.PropertyHandle->GetProperty() != nullptr)
        {
            PropertyName = Args.PropertyHandle->GetProperty()->GetFName();
        }
        else if (Args.Property != nullptr)
        {
            PropertyName = Args.Property->GetFName();
        }
        if (PropertyName == GET_MEMBER_NAME_CHECKED(UBlueprint, ParentClass))
        {
            if (!SoloASBridge::IsRegisteredScriptClass(Blueprint->ParentClass))
            {
                OutExtensions.Add(MakeScriptParentPickerButton(
                    TWeakObjectPtr<UBlueprint>(Blueprint)));
            }
            return;
        }
    }
    if (GetSelectedGameModeProperty(DetailsObject) == nullptr)
    {
        return;
    }
    const FName PropertyName = FindDefaultClassProperty(Args.OwnerTreeNode);
    if (!PropertyName.IsNone())
    {
        const TWeakPtr<IDetailTreeNode> WeakNode = Args.OwnerTreeNode;
        OutExtensions.Add(MakeNavigationButton(
            FExecuteAction::CreateLambda([WeakNode, PropertyName]()
            {
                if (UClass* Class = ResolveNavigableDefaultClass(
                    WeakNode, PropertyName))
                {
                    FSourceCodeNavigation::NavigateToClass(Class);
                }
            }),
            FCanExecuteAction::CreateLambda([WeakNode, PropertyName]()
            {
                return ResolveNavigableDefaultClass(
                    WeakNode, PropertyName) != nullptr;
            }),
            FIsActionButtonVisible::CreateLambda([WeakNode, PropertyName]()
            {
                return ResolveNavigableDefaultClass(
                    WeakNode, PropertyName) != nullptr;
            })));
        return;
    }
    if (IsGameModeProperty(Args.PropertyHandle))
    {
        const TWeakPtr<IPropertyHandle> WeakHandle = Args.PropertyHandle;
        OutExtensions.Add(MakeNavigationButton(
            FExecuteAction::CreateLambda([WeakHandle]()
            {
                if (UClass* Class = ResolveNavigableGameMode(WeakHandle.Pin()))
                {
                    FSourceCodeNavigation::NavigateToClass(Class);
                }
            }),
            FCanExecuteAction::CreateLambda([WeakHandle]()
            {
                return ResolveNavigableGameMode(WeakHandle.Pin()) != nullptr;
            }),
            FIsActionButtonVisible::CreateLambda([WeakHandle]()
            {
                return ResolveNavigableGameMode(WeakHandle.Pin()) != nullptr;
            })));
        return;
    }
}
}

class FSoloASBridgeEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
                                                                                                         
        SoloASBridge::Editor::InitializeEditorExperienceImports();
                                                                                                       
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
        SoloASBridge::Editor::InitializeCoreRedirectClassificationHook();
#endif
        SoloASBridge::Editor::InitializeScriptBlueprintProvider();
        SoloASBridge::Editor::InitializeScriptBlueprintCompiler();
        if (!GIsEditor)
        {
            return;
        }
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
        SoloASBridge::Editor::InitializeScriptFunctionSourceNavigationHook();
        SoloASBridge::Editor::InitializeOutlinerScriptSourceLinkHook();
        SoloASBridge::Editor::InitializeListViewEntryClassHook();
#endif
        SoloASBridge::Editor::InitializeScriptBlueprintValidator();
        if (!GScriptParentSourceOverlayHandle.IsValid())
        {
            if (GEditor != nullptr)
            {
                RegisterScriptParentSourceOverlay();
            }
            else if (!GScriptParentOverlayPostEngineInitHandle.IsValid())
            {
                GScriptParentOverlayPostEngineInitHandle =
                    FCoreDelegates::GetOnPostEngineInit().AddStatic(
                        &RegisterScriptParentSourceOverlay);
            }
        }
        if (GGameModeNavigationHandle.IsValid())
        {
            return;
        }
        FPropertyEditorModule& PropertyEditor =
            FModuleManager::LoadModuleChecked<FPropertyEditorModule>(
                "PropertyEditor");
        GGameModeNavigationHandle = PropertyEditor
            .GetGlobalRowExtensionDelegate().AddStatic(
                &AddGameModeNavigationButton);
        PropertyEditor.NotifyCustomizationModuleChanged();
    }

    virtual void ShutdownModule() override
    {
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
        SoloASBridge::Editor::ShutdownCoreRedirectClassificationHook();
#endif
                                                                                                          
        SoloASBridge::Editor::ShutdownEditorExperienceImports();
                                                                                                        
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
        SoloASBridge::Editor::ShutdownListViewEntryClassHook();
        SoloASBridge::Editor::ShutdownOutlinerScriptSourceLinkHook();
        SoloASBridge::Editor::ShutdownScriptFunctionSourceNavigationHook();
#endif
        if (GScriptParentOverlayPostEngineInitHandle.IsValid())
        {
            FCoreDelegates::GetOnPostEngineInit().Remove(
                GScriptParentOverlayPostEngineInitHandle);
            GScriptParentOverlayPostEngineInitHandle.Reset();
        }
        if (GScriptParentSourceOverlayHandle.IsValid())
        {
            if (FBlueprintEditorModule* BlueprintEditorModule =
                FModuleManager::GetModulePtr<FBlueprintEditorModule>("Kismet"))
            {
                BlueprintEditorModule->OnRegisterTabsForEditor().Remove(
                    GScriptParentSourceOverlayHandle);
            }
            GScriptParentSourceOverlayHandle.Reset();
        }
        if (GScriptParentCompileHandle.IsValid())
        {
            if (GEditor != nullptr)
            {
                GEditor->OnBlueprintCompiled().Remove(GScriptParentCompileHandle);
            }
            GScriptParentCompileHandle.Reset();
        }
        SoloASBridge::Editor::ShutdownScriptBlueprintValidator();
        SoloASBridge::Editor::ShutdownScriptBlueprintProvider();
        if (!GGameModeNavigationHandle.IsValid())
        {
            return;
        }
        if (FPropertyEditorModule* PropertyEditor =
            FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
        {
            PropertyEditor->GetGlobalRowExtensionDelegate().Remove(
                GGameModeNavigationHandle);
            PropertyEditor->NotifyCustomizationModuleChanged();
        }
        GGameModeNavigationHandle.Reset();
    }

    virtual bool SupportsDynamicReloading() override
    {
        return false;
    }
};

IMPLEMENT_MODULE(FSoloASBridgeEditorModule, SoloASBridgeEditor)
