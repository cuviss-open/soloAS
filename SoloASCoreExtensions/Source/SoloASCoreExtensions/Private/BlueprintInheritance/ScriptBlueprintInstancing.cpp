// Blueprint ancestor generation awaits the core B-class extension point.
#if __has_include("Extensions/AngelscriptExtensionHooks.h")
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "ScriptBlueprintInstancing.h"
#include "Compat/SoloASRuntimeCompat.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/ChildActorComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/Stack.h"
#include "Misc/ConfigCacheIni.h"
#include "ComponentInstanceDataCache.h"
#include "UObject/StructOnScope.h"

namespace SoloASCoreExtensions::ScriptInterfaces
{
namespace
{
bool IsPersistentState(FProperty* Property, UObject* Object)
{
    if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient)) return false;
    UClass* Owner = Property->GetOwnerClass();
    if (Object->IsA<AActor>()) return Owner && Owner != AActor::StaticClass() && Owner != UObject::StaticClass();
    if (Object->IsA<UActorComponent>() && (Owner == UActorComponent::StaticClass()
        || Owner == USceneComponent::StaticClass() || Owner == UPrimitiveComponent::StaticClass()))
        return Property->HasAnyPropertyFlags(CPF_Edit | CPF_Interp);
    return true;
}

bool IsChildActorField(FProperty* Property)
{
    const FName Name = Property->GetFName();
    return Property->GetOwnerClass() == UChildActorComponent::StaticClass()
        && (Name == TEXT("ChildActorClass") || Name == TEXT("ChildActorTemplate") || Name == TEXT("ChildActorName"));
}

bool IsOwnedTemplate(UObject* Object, UObject* Root)
{
    for (UObject* Outer = Object; Outer && Outer != Root; Outer = Outer->GetOuter())
        if (Outer->IsTemplate()) return true;
    return false;
}

bool InstancingError(FString& Error, UObject* Object, const TCHAR* Reason)
{
    Error = FString::Printf(TEXT("Blueprint instancing rejected '%s' (class='%s', outer='%s'): %s."),
        *GetPathNameSafe(Object), *GetPathNameSafe(Object ? Object->GetClass() : nullptr),
        *GetPathNameSafe(Object ? Object->GetOuter() : nullptr), Reason);
    return false;
}
}

struct FScriptBlueprintInstancing::FNode
{
    struct FValue
    {
        FProperty* Property;
        void* Data = nullptr;
        TOptional<FMulticastScriptDelegate> SparseValue;
        explicit FValue(FProperty* InProperty, UObject* Source) : Property(InProperty)
        {
            if (auto* Sparse = CastField<FMulticastSparseDelegateProperty>(Property))
            {
                SparseValue.Emplace();
                if (const auto* Value = Sparse->GetMulticastDelegate(Property->ContainerPtrToValuePtr<void>(Source)))
                    SparseValue.GetValue() = *Value;
            }
            else
            {
                Data = Property->AllocateAndInitializeValue();
                Property->CopyCompleteValue(Data, Property->ContainerPtrToValuePtr<void>(Source));
            }
        }
        ~FValue() { if (Data) Property->DestroyAndFreeValue(Data); }
    };
    TStrongObjectPtr<UObject> Previous;
    UObject* Root = nullptr;
    TArray<TUniquePtr<FValue>> Values;
    TStructOnScope<FActorComponentInstanceData> ComponentData;
    TStrongObjectPtr<USceneComponent> Parent;
    FName Socket;
    FTransform RelativeTransform;
    bool bRegistered = false;
    bool bRoot = false;
    bool bChildComponent = false;
};

FScriptBlueprintInstancing::FScriptBlueprintInstancing() = default;
FScriptBlueprintInstancing::~FScriptBlueprintInstancing() = default;

bool GatherBlueprintChildActors(TArray<UObject*>& Roots, const TSet<UClass*>& GeneratedClasses,
    TArray<FBlueprintChildActorOwner>& Children, FString& Error)
{
    // A current parent CAC cannot replace only its child without retiring that
    // child early. Retain the complete owning Actor chain up to its root.
    const TArray<UObject*> Selected = Roots;
    for (UObject* Root : Selected)
        for (AActor* Actor = Cast<AActor>(Root); Actor && Actor->GetParentActor();)
        {
            Actor = Actor->GetParentActor();
            if (!Actor->GetClass()->HasAnyClassFlags(CLASS_Native) && !GeneratedClasses.Contains(Actor->GetClass()))
                return InstancingError(Error, Actor, TEXT("the owning shared Blueprint class has no private candidate; deep child replacement was not started"));
            Roots.AddUnique(Actor);
        }
    for (int32 Index = 0; Index < Roots.Num(); ++Index)
    {
        AActor* Owner = Cast<AActor>(Roots[Index]);
        if (!Owner) continue;
        TInlineComponentArray<UChildActorComponent*> Components(Owner);
        for (UChildActorComponent* Component : Components)
        {
            AActor* Child = Component->GetChildActor();
            if (!Child) continue;
            // An unchanged shared Blueprint keeps its existing stock CAC path.
            // Its class maps and its internal graph are not admitted here.
            if (!Child->GetClass()->HasAnyClassFlags(CLASS_Native) && !GeneratedClasses.Contains(Child->GetClass())) continue;
            if (!IsValid(Child) || Child->IsActorBeingDestroyed() || Child->GetParentComponent() != Component
                || Child->GetWorld() != Owner->GetWorld() || Child->GetLevel() != Owner->GetLevel()
                || Child->GetClass() != Component->GetChildActorClass() || Child->GetIsReplicated()
                || Child->GetLocalRole() != ROLE_Authority || Child->IsActorBeginningPlay())
                return InstancingError(Error, Child, TEXT("the ChildActorComponent ownership or lifecycle identity is not stable"));
            if (Component->CreationMethod == EComponentCreationMethod::UserConstructionScript)
                return InstancingError(Error, Component, TEXT("a UCS-created child component needs a separate construction-owner migration"));
            TSet<AActor*> Ancestors;
            for (AActor* Actor = Owner; Actor; Actor = Actor->GetParentActor())
            {
                if (Actor == Child || Ancestors.Contains(Actor))
                    return InstancingError(Error, Child, TEXT("the ChildActorComponent ownership graph contains a cycle"));
                Ancestors.Add(Actor);
            }
            const auto* Existing = Children.FindByPredicate([&](const auto& Item) { return Item.Previous.Get() == Child; });
            if (Existing)
            {
                if (Existing->Parent.Get() != Component)
                    return InstancingError(Error, Child, TEXT("more than one component claims the same child"));
                continue;
            }
            auto& State = Children.AddDefaulted_GetRef();
            State.Previous.Reset(Child);
            State.Parent.Reset(Component);
            State.Relative = Child->GetRootComponent() ? Child->GetRootComponent()->GetRelativeTransform() : FTransform::Identity;
            Roots.AddUnique(Child);
        }
    }
    return true;
}

bool DeferBlueprintChildActorComponents(AActor* Previous, AActor* Candidate,
    const TArray<FBlueprintChildActorOwner>& Children, const TMap<UObject*, UObject*>& Types,
    TArray<FDeferredBlueprintChildActor>& Deferred, FString& Error)
{
    auto* ClassProperty = FindFProperty<FClassProperty>(UChildActorComponent::StaticClass(), TEXT("ChildActorClass"));
    if (!ClassProperty || ClassProperty->MetaClass != AActor::StaticClass())
        return InstancingError(Error, Candidate, TEXT("the engine ChildActorClass reflection contract changed"));
    for (const auto& Child : Children)
    {
        if (Child.Parent->GetOwner() != Previous) continue;
        auto* Component = FindObject<UChildActorComponent>(Candidate, *Child.Parent->GetName());
        if (!Component) continue; // SCS components are deferred through their private templates.
        if (Deferred.ContainsByPredicate([&](const auto& Item) { return Item.Component.Get() == Component; })) continue;
        if (Component->IsRegistered() || Component->GetChildActor())
            return InstancingError(Error, Component, TEXT("child creation began before the candidate pre-spawn boundary"));
        UClass* Class = Child.Previous->GetClass();
        if (UObject* Replacement = Types.FindRef(Class)) Class = CastChecked<UClass>(Replacement);
        auto& State = Deferred.AddDefaulted_GetRef();
        State.Component.Reset(Component);
        State.Class.Reset(Class);
        State.Template.Reset(Child.Parent->GetChildActorTemplate());
        ClassProperty->SetObjectPropertyValue_InContainer(Component, nullptr);
    }
    return true;
}

bool CreateDeferredBlueprintChildActor(FDeferredBlueprintChildActor& State,
    TFunction<void(AActor*)> Customize, FString& Error)
{
    UChildActorComponent* Component = State.Component.Get();
    if (!IsValid(Component) || !IsValid(Component->GetOwner()) || Component->GetChildActor())
        return InstancingError(Error, Component, TEXT("the deferred child owner changed before construction"));
    auto* ClassProperty = FindFProperty<FClassProperty>(UChildActorComponent::StaticClass(), TEXT("ChildActorClass"));
    auto* TemplateProperty = FindFProperty<FObjectPropertyBase>(UChildActorComponent::StaticClass(), TEXT("ChildActorTemplate"));
    if (!ClassProperty || !TemplateProperty || TemplateProperty->PropertyClass != AActor::StaticClass())
        return InstancingError(Error, Component, TEXT("the engine child template reflection contract changed"));
    AActor* Template = nullptr;
    if (State.Template.IsValid())
    {
        Template = NewObject<AActor>(Component, State.Class.Get(), NAME_None, RF_Transient | RF_ArchetypeObject);
        UEngine::FCopyPropertiesForUnrelatedObjectsParams Options;
        Options.bDoDelta = true;
        Options.bNotifyObjectReplacement = false;
        UEngine::CopyPropertiesForUnrelatedObjects(State.Template.Get(), Template, Options);
    }
    ClassProperty->SetObjectPropertyValue_InContainer(Component, State.Class.Get());
    TemplateProperty->SetObjectPropertyValue_InContainer(Component, Template);
    Component->SetChildActorName(NAME_None);
    Component->CreateChildActor(MoveTemp(Customize));
    AActor* Child = Component->GetChildActor();
    if (!IsValid(Child) || Child->GetClass() != State.Class.Get() || Child->GetParentComponent() != Component)
        return InstancingError(Error, Component, TEXT("UE did not create the child with its mapped class and parent"));
    return true;
}

bool FScriptBlueprintInstancing::Prepare(const TArray<UObject*>& Roots,
    const TArray<FBlueprintChildActorOwner>& Children, FBlueprintPropertyReferences VisitReferences, FString& Error)
{
    for (const auto& Child : Children) ChildComponents.Add(Child.Parent.Get());
    TArray<UObject*> Inventory;
    TSet<UObject*> Reachable;
    for (UObject* Root : Roots)
    {
        TArray<UObject*> Owned;
        SoloASBridge::Compat::GetNestedObjects(Root, Owned);
        Owned.Add(Root);
        for (UObject* Object : Owned)
        {
            if (!IsValid(Object) || IsOwnedTemplate(Object, Root)) continue;
            if (Owners.Contains(Object)) continue;
            Owners.Add(Object, Root);
            Inventory.Add(Object);
            if (Object == Root || Object->IsA<UActorComponent>() || Object->HasAnyFlags(RF_DefaultSubObject))
                Reachable.Add(Object);
            if (auto* Component = Cast<UActorComponent>(Object))
            {
                if (Component->GetIsReplicated()) return InstancingError(Error, Object, TEXT("network component state has no local replacement contract"));
                if (Component->CreationMethod != EComponentCreationMethod::Native
                    && Component->CreationMethod != EComponentCreationMethod::SimpleConstructionScript)
                    RequiredRoots.Add(Root);
            }
        }
    }
    // Inspect persistent fields, including nested containers and dynamic structs.
    // Outer ownership determines identity; property flags do not split aliases.
    for (int32 Pass = 0; Pass < Inventory.Num(); ++Pass)
    {
        bool bAdded = false;
        for (UObject* Object : Inventory)
        {
            if (!Reachable.Contains(Object)) continue;
            for (TFieldIterator<FProperty> Field(Object->GetClass()); Field; ++Field)
            {
                if (!IsPersistentState(*Field, Object) || (ChildComponents.Contains(Object) && IsChildActorField(*Field))) continue;
                if (CastField<FDelegateProperty>(*Field) || CastField<FMulticastDelegateProperty>(*Field)) continue;
                if (!VisitReferences(*Field, Field->ContainerPtrToValuePtr<void>(Object),
                    [&](UObject*& Value, UClass*, bool)
                    {
                        if (!Value) return true;
                        References.Emplace(Value);
                        if (!Owners.Contains(Value)) return true;
                        if (auto* Component = Cast<UActorComponent>(Value);
                            Component && Component->CreationMethod == EComponentCreationMethod::SimpleConstructionScript
                            && (Component->GetOwner() != Object || Field->GetFName() != Component->GetFName()))
                        {
                            RequiredRoots.Add(Owners.FindChecked(Object));
                            RequiredRoots.Add(Owners.FindChecked(Value));
                        }
                        for (UObject* Current = Value; Current && Owners.Contains(Current); Current = Current->GetOuter())
                            if (!Reachable.Contains(Current)) { Reachable.Add(Current); bAdded = true; }
                        return true;
                    }, false)) return InstancingError(Error, Object, TEXT("a persistent field has no admitted reflected-reference contract"));
            }
        }
        if (!bAdded) break;
    }
    // Ensure each allocation's owner is allocated first. The UObject Outer chain
    // is acyclic even when the reflected reference graph contains cycles.
    auto Depth = [](UObject* Object)
    {
        int32 Result = 0;
        for (; Object; Object = Object->GetOuter()) ++Result;
        return Result;
    };
    Inventory.StableSort([&](const UObject& A, const UObject& B) { return Depth(const_cast<UObject*>(&A)) < Depth(const_cast<UObject*>(&B)); });
    for (UObject* Object : Inventory)
    {
        UObject* Root = Owners.FindChecked(Object);
        if (!Reachable.Contains(Object))
        {
            if (Object->HasAnyFlags(RF_Transient) && Object->GetClass()->HasAnyClassFlags(CLASS_Native))
            { Owners.Remove(Object); continue; }
            return InstancingError(Error, Object, TEXT("an owned object has no persistent reflected owner"));
        }
        if (Object != Root && !Object->IsA<UActorComponent>()) RequiredRoots.Add(Root);
        auto Node = MakeUnique<FNode>();
        Node->Previous.Reset(Object);
        Node->Root = Root;
        if (auto* Component = Cast<UActorComponent>(Object))
        {
            Node->bRegistered = Component->IsRegistered();
            Node->bChildComponent = ChildComponents.Contains(Component);
            if (Node->bChildComponent) Node->ComponentData.InitializeAs<FSceneComponentInstanceData>(CastChecked<USceneComponent>(Component));
            else Node->ComponentData = Component->GetComponentInstanceData();
            struct FPinReferences final : FReferenceCollector
            {
                TArray<TStrongObjectPtr<UObject>>& Pins;
                explicit FPinReferences(TArray<TStrongObjectPtr<UObject>>& InPins) : Pins(InPins) {}
                bool IsIgnoringArchetypeRef() const override { return false; }
                bool IsIgnoringTransient() const override { return false; }
                void HandleObjectReference(UObject*& Value, const UObject*, const FProperty*) override
                { if (Value) Pins.Emplace(Value); }
            } Pins(References);
            if (Node->ComponentData.IsValid()) Node->ComponentData->AddReferencedObjects(Pins);
        }
        if (auto* Scene = Cast<USceneComponent>(Object))
        {
            Node->Parent.Reset(Scene->GetAttachParent());
            Node->Socket = Scene->GetAttachSocketName();
            Node->RelativeTransform = Scene->GetRelativeTransform();
            Node->bRoot = Scene->GetOwner() && Scene->GetOwner()->GetRootComponent() == Scene;
        }
        for (TFieldIterator<FProperty> Field(Object->GetClass()); Field; ++Field)
            if (IsPersistentState(*Field, Object) && !(Node->bChildComponent && IsChildActorField(*Field))
                && !Field->Identical_InContainer(Object, Object->GetArchetype()))
            {
                auto Value = MakeUnique<FNode::FValue>(*Field, Object);
                if (Value->SparseValue.IsSet())
                    for (UObject* Target : Value->SparseValue.GetValue().GetAllObjects()) if (Target) References.Emplace(Target);
                Node->Values.Add(MoveTemp(Value));
            }
        Nodes.Add(MoveTemp(Node));
    }
    for (const auto& Child : Children)
    {
        RequireMigration(Child.Previous.Get());
        RequireMigration(Child.Parent.Get());
    }
    return true;
}

bool FScriptBlueprintInstancing::Contains(UObject* Object) const { return Owners.Contains(Object); }
bool FScriptBlueprintInstancing::RequiresMigration(UObject* Root) const { return RequiredRoots.Contains(Root); }
bool FScriptBlueprintInstancing::HasMigration() const { return !RequiredRoots.IsEmpty(); }
void FScriptBlueprintInstancing::RequireMigration(UObject* Object)
{
    if (UObject* Root = Owners.FindRef(Object)) RequiredRoots.Add(Root);
}

bool FScriptBlueprintInstancing::Allocate(TMap<UObject*, UObject*>& Objects,
    const TMap<UObject*, UObject*>& Types, bool bConstructionReady, FString& Error)
{
    for (const auto& Node : Nodes)
    {
        UObject* Previous = Node->Previous.Get();
        if (!RequiresMigration(Node->Root) || Previous == Node->Root) continue;
        UObject* Outer = Objects.FindRef(Previous->GetOuter());
        if (!Outer)
        {
            if (!bConstructionReady) continue;
            return InstancingError(Error, Previous, TEXT("the replacement Outer has no instance mapping"));
        }
        UClass* Class = Previous->GetClass();
        if (UObject* Candidate = Types.FindRef(Class)) Class = CastChecked<UClass>(Candidate);
        UObject* Replacement = StaticFindObjectFast(UObject::StaticClass(), Outer, Previous->GetFName());
        if (!Replacement)
        {
            if (auto* Component = Cast<UActorComponent>(Previous);
                Component && Component->CreationMethod == EComponentCreationMethod::SimpleConstructionScript)
            {
                if (!bConstructionReady) continue;
                return InstancingError(Error, Previous, TEXT("SCS did not create the expected component"));
            }
            const EObjectFlags Flags = Previous->GetFlags() & (RF_Transient | RF_Transactional | RF_DefaultSubObject);
            FObjectInstancingGraph Graph(Outer);
            for (const auto& Pair : Objects) Graph.AddNewInstance(Pair.Value, Pair.Key);
            Replacement = NewObject<UObject>(Outer, Class, Previous->GetFName(), Flags, nullptr, false, &Graph);
        }
        if (!Replacement || Replacement->GetClass() != Class)
            return InstancingError(Error, Previous, TEXT("the replacement name resolves to a different class"));
        // UE's component cache can install its own duplicated instanced values.
        // Keep earlier staging targets in the map until all holders are rebound.
        if (UObject* Staged = Objects.FindRef(Previous); Staged && Staged != Replacement) Objects.Add(Staged, Replacement);
        Objects.Add(Previous, Replacement);
        if (auto* Component = Cast<UActorComponent>(Replacement))
        {
            Component->CreationMethod = CastChecked<UActorComponent>(Previous)->CreationMethod;
            AActor* Owner = Component->GetOwner();
            if (!Owner) return InstancingError(Error, Previous, TEXT("the component has no replacement Actor owner"));
            if (Component->CreationMethod == EComponentCreationMethod::Instance) Owner->AddInstanceComponent(Component);
        }
    }
    return true;
}

bool FScriptBlueprintInstancing::Copy(TMap<UObject*, UObject*>& Objects,
    FBlueprintPropertyReferences VisitReferences, FString& Error)
{
    for (const auto& Node : Nodes)
    {
        if (!RequiresMigration(Node->Root)) continue;
        UObject* Previous = Node->Previous.Get();
        UObject* Replacement = Objects.FindRef(Previous);
        if (!Replacement) return InstancingError(Error, Previous, TEXT("the complete copy map has no replacement"));
        for (const auto& Value : Node->Values)
        {
            FProperty* Property = FindFProperty<FProperty>(Replacement->GetClass(), Value->Property->GetFName());
            if (!Property || Property->GetClass() != Value->Property->GetClass()
                || Property->GetElementSize() != Value->Property->GetElementSize() || Property->ArrayDim != Value->Property->ArrayDim)
                return InstancingError(Error, Previous, TEXT("a saved property has no compatible destination"));
            if (Value->SparseValue.IsSet())
                CastFieldChecked<FMulticastSparseDelegateProperty>(Property)->SetMulticastDelegate(
                    Property->ContainerPtrToValuePtr<void>(Replacement), Value->SparseValue.GetValue());
            else Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Replacement), Value->Data);
        }
    }
    RestoreTransforms(Objects);
    return Rebind(Objects, VisitReferences, Error);
}

void FScriptBlueprintInstancing::RestoreTransforms(const TMap<UObject*, UObject*>& Objects)
{
    for (const auto& Node : Nodes)
    {
        if (!RequiresMigration(Node->Root)) continue;
        auto* Scene = Cast<USceneComponent>(Objects.FindRef(Node->Previous.Get()));
        if (!Scene) continue;
        auto* Parent = Node->Parent.Get();
        if (UObject* Mapped = Objects.FindRef(Parent)) Parent = CastChecked<USceneComponent>(Mapped);
        else if (Contains(Parent)) continue;
        if (Node->bRoot) Scene->GetOwner()->SetRootComponent(Scene);
        auto* RootActor = Cast<AActor>(Node->Root);
        if (Parent && (!Node->bRoot || (RootActor && RootActor->GetParentComponent() == Node->Parent.Get())))
        {
            if (Scene->IsRegistered()) Scene->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform, Node->Socket);
            else Scene->SetupAttachment(Parent, Node->Socket);
        }
        Scene->SetRelativeTransform(Node->RelativeTransform);
    }
}

bool FScriptBlueprintInstancing::Rebind(const TMap<UObject*, UObject*>& Objects,
    FBlueprintPropertyReferences VisitReferences, FString& Error)
{
    for (const auto& Node : Nodes)
    {
        if (!RequiresMigration(Node->Root)) continue;
        UObject* Previous = Node->Previous.Get();
        UObject* Replacement = Objects.FindRef(Previous);
        if (!Replacement) return InstancingError(Error, Previous, TEXT("the complete copy map has no replacement"));
        for (TFieldIterator<FProperty> Field(Replacement->GetClass()); Field; ++Field)
            if (IsPersistentState(*Field, Replacement) && !(Node->bChildComponent && IsChildActorField(*Field))
                && !VisitReferences(*Field, Field->ContainerPtrToValuePtr<void>(Replacement),
                    [&](UObject*& Value, UClass* Constraint, bool bInterface)
                    {
                        int32 Remaining = Objects.Num() + 1;
                        while (UObject* Mapped = Objects.FindRef(Value))
                        {
                            if (Mapped == Value) break;
                            if (--Remaining == 0) return false;
                            Value = Mapped;
                        }
                        return !Value || (bInterface ? Value->GetClass()->ImplementsInterface(Constraint) : Value->IsA(Constraint));
                    }, true)) return InstancingError(Error, Previous, TEXT("a copied field cannot accept its mapped instance"));
    }
    return true;
}

void FScriptBlueprintInstancing::ApplyComponentData(const TMap<UObject*, UObject*>& Objects,
    const TMap<UObject*, UObject*>& Templates, ECacheApplyPhase Phase, AActor* Actor)
{
    struct FMapReferences final : FReferenceCollector
    {
        const TMap<UObject*, UObject*>& Instances;
        const TMap<UObject*, UObject*>& Archetypes;
        TArray<TStrongObjectPtr<UObject>>& Pins;
        FMapReferences(const TMap<UObject*, UObject*>& InInstances, const TMap<UObject*, UObject*>& InArchetypes,
            TArray<TStrongObjectPtr<UObject>>& InPins) : Instances(InInstances), Archetypes(InArchetypes), Pins(InPins) {}
        bool IsIgnoringArchetypeRef() const override { return false; }
        bool IsIgnoringTransient() const override { return false; }
        void HandleObjectReference(UObject*& Object, const UObject*, const FProperty*) override
        {
            if (UObject* Mapped = Instances.FindRef(Object)) Object = Mapped;
            else if (UObject* Template = Archetypes.FindRef(Object)) Object = Template;
            if (Object) Pins.Emplace(Object);
        }
    } Map(Objects, Templates, References);
    for (const auto& Node : Nodes)
    {
        if (!RequiresMigration(Node->Root) || !Node->ComponentData.IsValid()) continue;
        auto* Component = Cast<UActorComponent>(Objects.FindRef(Node->Previous.Get()));
        if (!Component || (Actor && Component->GetOwner() != Actor)) continue;
        if (Phase == ECacheApplyPhase::PostUserConstructionScript
            && !Component->GetOwner()->IsChildActor() && !Component->IsCreatedByConstructionScript()) continue;
        // NonConstructionScript already installed this cache's owned duplicates.
        // Applying it again renames those same objects out of their mapped slots.
        // The copied inputs are live now; preserve the user's construction output.
        if (Phase == ECacheApplyPhase::PostUserConstructionScript
            && !Node->ComponentData->GetDuplicatedObjects().IsEmpty()) continue;
        Node->ComponentData->AddReferencedObjects(Map);
        Node->ComponentData->FindAndReplaceInstances(Objects);
        Node->ComponentData->ApplyToComponent(Component, Phase);
    }
}

bool FScriptBlueprintInstancing::Validate(const TMap<UObject*, UObject*>& Objects, FString& Error) const
{
    for (const auto& Node : Nodes)
    {
        if (!RequiresMigration(Node->Root)) continue;
        UObject* Previous = Node->Previous.Get();
        UObject* Replacement = Objects.FindRef(Previous);
        if (!IsValid(Replacement) || Replacement->GetOuter() != Objects.FindRef(Previous->GetOuter()))
        {
            if (Previous == Node->Root && IsValid(Replacement)) continue;
            return InstancingError(Error, Previous, TEXT("construction changed a mapped object identity or Outer"));
        }
    }
    return true;
}

void FScriptBlueprintInstancing::RegisterComponents(const TMap<UObject*, UObject*>& Objects)
{
    for (const auto& Node : Nodes)
        if (RequiresMigration(Node->Root) && Node->bRegistered)
            if (auto* Component = Cast<UActorComponent>(Objects.FindRef(Node->Previous.Get())); Component && !Component->IsRegistered())
            {
                if (!Component->HasBeenCreated()) Component->OnComponentCreated();
                Component->RegisterComponent();
            }
}

struct FScriptBlueprintConstruction::FClass
{
    TStrongObjectPtr<UClass> Class;
    TStrongObjectPtr<UFunction> Original;
    TStrongObjectPtr<UFunction> Bridge;
};

thread_local FScriptBlueprintConstruction* FScriptBlueprintConstruction::Active = nullptr;
FScriptBlueprintConstruction::FScriptBlueprintConstruction() = default;
FScriptBlueprintConstruction::~FScriptBlueprintConstruction()
{
    for (const auto& Item : Classes)
    {
        Item->Class->AddFunctionToFunctionMap(Item->Original.Get(), TEXT("UserConstructionScript"));
        Item->Class->ClearFunctionMapsCaches();
        Item->Bridge->Rename(*MakeUniqueObjectName(GetTransientPackage(), UFunction::StaticClass()).ToString(),
            GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
    }
    if (Active == this) Active = nullptr;
}

bool FScriptBlueprintConstruction::Prepare(const TArray<AActor*>& Actors,
    TFunction<bool(AActor*)> BeforeUserScript, TFunction<bool(AActor*)> AfterUserScript, FString& Error)
{
    if (Active) { Error = TEXT("A Blueprint construction mapping batch is already active."); return false; }
#if WITH_EDITOR
    bool bDisabled = false;
    GConfig->GetBool(TEXT("Kismet"), TEXT("bTurnOffEditorConstructionScript"), bDisabled, GEngineIni);
    if (GIsEditor && bDisabled)
    { Error = TEXT("Blueprint instancing requires the normal SCS/UserConstructionScript boundary; editor construction scripts are disabled."); return false; }
#endif
    for (AActor* Actor : Actors) if (!AddActor(Actor, Error)) return false;
    Before = MoveTemp(BeforeUserScript);
    After = MoveTemp(AfterUserScript);
    Active = this;
    return true;
}

bool FScriptBlueprintConstruction::AddActor(AActor* Actor, FString& Error)
{
    Receivers.Add(Actor);
    UClass* Class = Actor->GetClass();
    if (Classes.ContainsByPredicate([&](const auto& Item) { return Item->Class.Get() == Class; })) return true;
    UFunction* Original = Class->FindFunctionByName(TEXT("UserConstructionScript"));
    for (const auto& Item : Classes) if (Original == Item->Bridge.Get()) Original = Item->Original.Get();
    if (!Original || Original->NumParms || Original->ParmsSize)
        return InstancingError(Error, Actor, TEXT("the construction callback has no supported no-argument reflection signature"));
    auto Item = MakeUnique<FClass>();
    Item->Class.Reset(Class);
    Item->Original.Reset(Original);
    Item->Bridge.Reset(NewObject<UFunction>(Class, NAME_None, RF_Transient));
    Item->Bridge->FunctionFlags = Original->FunctionFlags | FUNC_Native;
    Item->Bridge->SetNativeFunc(&Invoke);
    Class->AddFunctionToFunctionMap(Item->Bridge.Get(), TEXT("UserConstructionScript"));
    Class->ClearFunctionMapsCaches();
    Classes.Add(MoveTemp(Item));
    return true;
}

void FScriptBlueprintConstruction::Invoke(UObject* Context, FFrame& Stack, void* Result)
{
    P_FINISH;
    auto& Batch = *Active;
    AActor* Actor = CastChecked<AActor>(Context);
    const auto* Class = Batch.Classes.FindByPredicate([&](const auto& Item) { return Item->Bridge.Get() == Stack.CurrentNativeFunction; });
    check(Class);
    const bool bReceiver = Batch.Receivers.Contains(Actor);
    if (bReceiver)
    {
        Batch.EnteredReceivers.Add(Actor);
        Batch.bSucceeded &= Batch.Before(Actor);
    }
    if (Batch.bSucceeded || !bReceiver) Context->ProcessEvent((*Class)->Original.Get(), nullptr);
    if (bReceiver && Batch.bSucceeded) Batch.bSucceeded &= Batch.After(Actor);
}

bool FScriptBlueprintConstruction::Entered(AActor* Actor) const { return EnteredReceivers.Contains(Actor); }
bool FScriptBlueprintConstruction::Succeeded() const { return bSucceeded; }
}

#else
#endif
