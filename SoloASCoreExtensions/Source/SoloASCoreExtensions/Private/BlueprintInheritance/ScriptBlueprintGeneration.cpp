// Blueprint ancestor generation awaits the core B-class extension point.
#if __has_include("Extensions/AngelscriptExtensionHooks.h")
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "ScriptBlueprintGeneration.h"
#include "Compat/SoloASRuntimeCompat.h"
#include "Interfaces/ScriptInterfacesExtension.h"
#include "Shared/SoloASScriptBlueprint.h"
#include "Core/SoloASCoreExtensionsModule.h"
#include "BlueprintInheritance/BlueprintQuickCallsExtension.h"
#include "AngelscriptBinds.h"
#include "AngelscriptCodeModule.h"
#include "ClassGenerator/AngelscriptClassGenerator.h"
#include "AngelscriptManager.h"
#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptType.h"
#include "AngelscriptBinds/Helper_PODType.h"
#include "UObject/GarbageCollectionSchema.h"
#include "UObject/ScriptInterface.h"
#include "UObject/Stack.h"
#include "Extensions/AngelscriptExtensionHooks.h"
#include "Preprocessor/AngelscriptPreprocessor.h"
#include "Internationalization/Regex.h"
#include "Misc/SecureHash.h"
#include "Misc/FileHelper.h"
#include "UObject/LinkerInstancingContext.h"
#include "Serialization/ArchiveUObject.h"
#include "Serialization/ObjectWriter.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "Misc/ScopeExit.h"
#include "Misc/CoreDelegates.h"
#include "UObject/UObjectArray.h"
#include "UObject/GCObject.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Base64.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/Class.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "UObject/StrongObjectPtr.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/UObjectIterator.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "ComponentInstanceDataCache.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/Level.h"
#include "GameFramework/Pawn.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "ComponentReregisterContext.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/UnrealType.h"
#include "StartAngelscriptHeaders.h"
#include "angelscript.h"
#include "as_module.h"
#include "EndAngelscriptHeaders.h"
#if WITH_EDITOR && !IS_MONOLITHIC
#include "Editor.h"
#include "Containers/Ticker.h"
#include "Engine/Blueprint.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Self.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/ReferencerFinder.h"
#include "UObject/PackageReload.h"
#include "UObject/LinkerInstancingContext.h"
#include "PackageTools.h"
#endif


namespace SoloASCoreExtensions::ScriptInterfaces
{
// Keep this independent of UE's mutable old reflection objects. Package reload
// repoints their fields before the VM view can be rebound.
TArray<FString> BlueprintImportAbi(UClass* Class, const TMap<UObject*, UObject*>* ReboundTypes,
    bool bIncludeStorage)
{
    TArray<FString> Rows;
    auto AddType = [&](const FString& Key, UObject* Type)
    {
        if (Type == Class) { Rows.Add(Key + TEXT(":self")); return; }
        if (ReboundTypes)
            if (UObject* const* Previous = ReboundTypes->Find(Type)) Type = *Previous;
        Rows.Add(Key + FString::Printf(TEXT(":%llu"), uint64(reinterpret_cast<UPTRINT>(Type))));
    };
    TFunction<void(const FString&, FProperty*)> AddProperty = [&](const FString& Key, FProperty* Property)
    {
        FString CppType = Property->GetCPPType();
        if (ReboundTypes)
            for (const auto& Type : *ReboundTypes)
                if (Type.Key && Type.Value) CppType.ReplaceInline(*Type.Key->GetName(), *Type.Value->GetName());
        Rows.Add(FString::Printf(TEXT("%s:%s:%s:%d:%d:%llu"), *Key,
            *Property->GetClass()->GetName(), *CppType,
            Property->GetElementSize(), Property->ArrayDim, uint64(Property->PropertyFlags)));
        if (bIncludeStorage)
            Rows.Add(FString::Printf(TEXT("%s:offset:%d"), *Key, Property->GetOffset_ForInternal()));
        if (auto* Bool = CastField<FBoolProperty>(Property); Bool && bIncludeStorage)
            Rows.Add(FString::Printf(TEXT("%s:bool:%u:%u"), *Key, Bool->GetFieldMask(), Bool->GetByteOffset()));
        if (auto* Object = CastField<FObjectPropertyBase>(Property)) AddType(Key + TEXT(":object"), Object->PropertyClass);
        if (auto* ClassProperty = CastField<FClassProperty>(Property)) AddType(Key + TEXT(":meta"), ClassProperty->MetaClass);
        if (auto* Interface = CastField<FInterfaceProperty>(Property)) AddType(Key + TEXT(":interface"), Interface->InterfaceClass);
        if (auto* Struct = CastField<FStructProperty>(Property)) AddType(Key + TEXT(":struct"), Struct->Struct);
        if (auto* Enum = CastField<FEnumProperty>(Property)) AddType(Key + TEXT(":enum"), Enum->GetEnum());
        if (auto* Byte = CastField<FByteProperty>(Property); Byte && Byte->Enum) AddType(Key + TEXT(":enum"), Byte->Enum);
        if (auto* Array = CastField<FArrayProperty>(Property)) AddProperty(Key + TEXT(":inner"), Array->Inner);
        if (auto* Set = CastField<FSetProperty>(Property)) AddProperty(Key + TEXT(":element"), Set->ElementProp);
        if (auto* Map = CastField<FMapProperty>(Property))
        {
            AddProperty(Key + TEXT(":key"), Map->KeyProp);
            AddProperty(Key + TEXT(":value"), Map->ValueProp);
        }
    };
    if (bIncludeStorage)
        Rows.Add(FString::Printf(TEXT("size:%d:alignment:%d"), Class->GetStructureSize(), Class->GetMinAlignment()));
    AddType(TEXT("parent"), Class->GetSuperClass());
    for (TFieldIterator<FProperty> It(Class); It; ++It) AddProperty(TEXT("property:") + It->GetName(), *It);
    for (TFieldIterator<UFunction> It(Class); It; ++It)
    {
        if (It->HasAnyFunctionFlags(FUNC_UbergraphFunction)) continue;
        const FString Key = TEXT("function:") + It->GetName();
        Rows.Add(FString::Printf(TEXT("%s:%u:%d"), *Key, uint32(It->FunctionFlags), It->ParmsSize));
        for (TFieldIterator<FProperty> Param(*It); Param && Param->HasAnyPropertyFlags(CPF_Parm); ++Param)
            AddProperty(Key + TEXT(":") + Param->GetName(), *Param);
    }
    Rows.Sort();
    return Rows;
}

bool BuildBlueprintGenerationSchema(UClass* Previous, UClass* Candidate,
    const TMap<UObject*, UObject*>& CandidateTypes, FBlueprintGenerationSchema& Schema, FString& Error)
{
    const auto Before = BlueprintImportAbi(Previous, nullptr, false);
    const auto After = BlueprintImportAbi(Candidate, &CandidateTypes, false);
    // The existing view remains callable until its consumers are recompiled.
    for (const FString& Row : Before)
        if (!After.Contains(Row))
        {
            FString CandidateRow;
            int32 BestPrefix = -1;
            for (const FString& Other : After)
            {
                int32 Prefix = 0;
                while (Prefix < FMath::Min(Row.Len(), Other.Len()) && Row[Prefix] == Other[Prefix]) ++Prefix;
                if (Prefix > BestPrefix) { BestPrefix = Prefix; CandidateRow = Other; }
            }
            Error = FString::Printf(TEXT("The Blueprint candidate removed or changed a reflected member still owned by the current script view; the current generation is unchanged. Previous=%s Candidate=%s Missing=%s CandidateRow=%s"),
                *Previous->GetPathName(), *Candidate->GetPathName(), *Row, *CandidateRow);
            return false;
        }
    for (TFieldIterator<FProperty> It(Previous); It; ++It)
    {
        FProperty* Replacement = FindFProperty<FProperty>(Candidate, It->GetFName());
        if (!Replacement)
        {
            Error = TEXT("The Blueprint candidate has no replacement for a current reflected field.");
            return false;
        }
        UClass* ReplacementOwner = Replacement->GetOwnerClass();
        if (UObject* const* Owner = CandidateTypes.Find(ReplacementOwner)) ReplacementOwner = CastChecked<UClass>(*Owner);
        if (It->GetOwnerClass() != ReplacementOwner)
        {
            Error = TEXT("A reflected field changed its declaring class; the current generation is unchanged.");
            return false;
        }
        Schema.Properties.Add(*It, Replacement);
    }
    for (TFieldIterator<FProperty> It(Candidate); It; ++It)
        if (!FindFProperty<FProperty>(Previous, It->GetFName())) Schema.AddedProperties.Add(*It);
    return true;
}

bool IsBlueprintActorStateField(FProperty* Property)
{
    UClass* Owner = Property->GetOwnerClass();
    return Owner && Owner != AActor::StaticClass() && Owner != UObject::StaticClass();
}

USCS_Node* BlueprintActorComponentSlot(UClass* Class, FProperty* Property)
{
    for (UClass* Owner = Class; Owner; Owner = Owner->GetSuperClass())
        if (auto* Blueprint = Cast<UBlueprintGeneratedClass>(Owner); Blueprint && Blueprint->SimpleConstructionScript)
            for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
                if (Node->GetVariableName() == Property->GetFName())
                    if (auto* Object = CastField<FObjectPropertyBase>(Property);
                        Object && Object->PropertyClass == Node->ComponentClass) return Node;
    return nullptr;
}

void FPendingBlueprintGeneration::RemoveMigrationDelegates()
    {
        FAngelscriptClassGenerator::OnClassReload.Remove(ClassReloadHandle);
        ClassReloadHandle.Reset();
#if WITH_EDITOR && !IS_MONOLITHIC
        FCoreUObjectDelegates::OnObjectsReinstanced.Remove(ObjectsReinstancedHandle);
        ObjectsReinstancedHandle.Reset();
#endif
    }

struct FBlueprintReceiverMigration
{
    struct FPropertyWrite
    {
        TStrongObjectPtr<UObject> Owner;
        FProperty* Property;
        void* Before = nullptr;
        TOptional<FMulticastScriptDelegate> SparseBefore;
        TArray<TStrongObjectPtr<UObject>> References;
        bool bAttempted = false;
        FPropertyWrite(UObject* InOwner, FProperty* InProperty) : Owner(InOwner), Property(InProperty)
        {
            if (auto* Sparse = CastField<FMulticastSparseDelegateProperty>(Property))
            {
                // Sparse invocation storage belongs to the real owner/name, not
                // to the local FSparseDelegate marker. Never copy/visit a fake owner.
                SparseBefore.Emplace();
                if (const auto* Value = Sparse->GetMulticastDelegate(Property->ContainerPtrToValuePtr<void>(Owner.Get())))
                    SparseBefore.GetValue() = *Value;
                for (UObject* Target : SparseBefore.GetValue().GetAllObjects()) if (Target) References.Emplace(Target);
            }
            else
            {
                Before = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
                Property->InitializeValue(Before);
                Property->CopyCompleteValue(Before, Property->ContainerPtrToValuePtr<void>(Owner.Get()));
            }
        }
        ~FPropertyWrite() { if (Before) { Property->DestroyValue(Before); FMemory::Free(Before); } }
        void Restore()
        {
            if (!bAttempted) return;
            void* Destination = Property->ContainerPtrToValuePtr<void>(Owner.Get());
            if (SparseBefore.IsSet())
                CastFieldChecked<FMulticastSparseDelegateProperty>(Property)->SetMulticastDelegate(Destination, SparseBefore.GetValue());
            else Property->CopyCompleteValue(Destination, Before);
        }
    };
    struct FActorWrite
    {
        TStrongObjectPtr<AActor> Actor, Owner;
        TStrongObjectPtr<APawn> Instigator;
        void Restore() const { Actor->SetOwner(Owner.Get()); Actor->SetInstigator(Instigator.Get()); }
    };
    struct FAttachmentWrite
    {
        TStrongObjectPtr<USceneComponent> Child, Parent;
        FName Socket;
        FTransform Relative, World;
        void Restore() const
        {
            if (Parent.IsValid())
            {
                Child->AttachToComponent(Parent.Get(), FAttachmentTransformRules::KeepRelativeTransform, Socket);
                Child->SetRelativeTransform(Relative);
            }
            else
            {
                Child->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
                Child->SetWorldTransform(World);
            }
        }
    };
    TMap<UObject*, UObject*> Objects;
    TArray<TStrongObjectPtr<UObject>> NewInstances;
    TArray<TWeakObjectPtr<AActor>> SpawnedActors;
    TArray<TUniquePtr<FPropertyWrite>> Properties;
    TArray<FActorWrite> Actors;
    TArray<FAttachmentWrite> Attachments;
    bool bIrreversible = false;
    ~FBlueprintReceiverMigration()
    {
        if (bIrreversible) return;
        for (int32 Index = Attachments.Num(); Index-- > 0;) Attachments[Index].Restore();
        for (int32 Index = Actors.Num(); Index-- > 0;) Actors[Index].Restore();
        for (int32 Index = Properties.Num(); Index-- > 0;) Properties[Index]->Restore();
        for (const auto& Actor : SpawnedActors)
            if (Actor.IsValid() && !Actor->IsActorBeingDestroyed()) Actor->GetWorld()->DestroyActor(Actor.Get(), false, false);
    }
};

FPendingBlueprintGeneration::FPendingBlueprintGeneration() = default;
FPendingBlueprintGeneration::~FPendingBlueprintGeneration()
{
    RemoveMigrationDelegates();
    // Restore journal values and destroy candidates before releasing their snapshots.
    Migration.Reset();
    Instancing.Reset();
    if (!bCommitAttempted)
        for (const auto& Subclass : Subclasses)
        {
            if (!Subclass.Package.IsValid()) continue;
            TArray<UObject*> Discarded;
            SoloASBridge::Compat::GetPackageObjects(Subclass.Package.Get(), Discarded);
            Discarded.Add(Subclass.Package.Get());
            uint32 IntermediateFlags = 0, RemainingFlags = 0;
            for (UObject* Object : Discarded)
            {
                // DuplicateDataWriter sets these even with bSkipPostLoad. This
                // private graph is discarded, so it must never route PostLoad.
                IntermediateFlags |= uint32(Object->GetFlags() & (RF_NeedPostLoad | RF_NeedPostLoadSubobjects));
                Object->ClearFlags(RF_NeedPostLoad | RF_NeedPostLoadSubobjects);
                Object->MarkAsGarbage();
                RemainingFlags |= uint32(Object->GetFlags()
                    & (RF_NeedLoad | RF_WillBeLoaded | RF_NeedPostLoad | RF_NeedPostLoadSubobjects));
            }
            UE_LOG(Angelscript, Display, TEXT("N04_PRIVATE_DISCARD objects=%d intermediate_before=0x%08x remaining=0x%08x"),
                Discarded.Num(), IntermediateFlags, RemainingFlags);
        }
    if (!InPlaceUpdate) return;
    // Unlike a new generation this isolated package never becomes the live one.
    ResetLoaders(Package.Get());
    TArray<UObject*> Objects;
    SoloASBridge::Compat::GetPackageObjects(Package.Get(), Objects);
    Objects.Add(Package.Get());
    for (UObject* Object : Objects)
    {
        Object->ClearFlags(RF_Standalone);
        Object->SetFlags(RF_Transient);
    }
}

struct FBlueprintChildCreation
{
    struct FTemplate
    {
        TStrongObjectPtr<UChildActorComponent> Component;
        TStrongObjectPtr<UClass> Class;
    };
    const TArray<FBlueprintChildActorOwner>& Children;
    const TMap<UObject*, UObject*>& Types;
    TMap<UObject*, UObject*>& Objects;
    FString& Error;
    FClassProperty* ClassProperty;
    TArray<FTemplate> Templates;
    TArray<FDeferredBlueprintChildActor> Deferred;
    TMap<UWorld*, FDelegateHandle> Worlds;
    TSet<AActor*> Started;
    UChildActorComponent* CreatingComponent = nullptr;
    AActor* CreatingPrevious = nullptr;
    bool bSucceeded = true;

    FBlueprintChildCreation(FPendingBlueprintGeneration& Pending, const TMap<UObject*, UObject*>& InTypes,
        TMap<UObject*, UObject*>& InObjects, FString& InError)
        : Children(Pending.ChildActors), Types(InTypes), Objects(InObjects), Error(InError),
          ClassProperty(FindFProperty<FClassProperty>(UChildActorComponent::StaticClass(), TEXT("ChildActorClass")))
    {
        if (Children.IsEmpty()) return;
        if (!ClassProperty) { Error = TEXT("The engine ChildActorClass reflection contract changed."); bSucceeded = false; return; }
        for (const auto& Child : Children)
        {
            if (Child.Parent->CreationMethod != EComponentCreationMethod::SimpleConstructionScript) continue;
            auto* Template = Cast<UChildActorComponent>(Pending.ActorTemplates.FindRef(Child.Parent->GetArchetype()));
            if (!Template || Template == Child.Parent->GetArchetype()
                || Template->GetOutermost() == Child.Parent->GetArchetype()->GetOutermost())
            {
                Error = TEXT("The child SCS slot has no private candidate template; shared Blueprint child behaviour is unchanged.");
                bSucceeded = false;
                return;
            }
            if (Templates.ContainsByPredicate([&](const auto& Item) { return Item.Component.Get() == Template; })) continue;
            UClass* Class = Template->GetChildActorClass();
            if (UObject* Candidate = Types.FindRef(Class)) Class = CastChecked<UClass>(Candidate);
            Templates.Add({TStrongObjectPtr<UChildActorComponent>(Template), TStrongObjectPtr<UClass>(Class)});
            ClassProperty->SetObjectPropertyValue_InContainer(Template, nullptr);
        }
        for (const auto& Plan : Pending.Actors)
            if (!Worlds.Contains(Plan.World.Get()))
                Worlds.Add(Plan.World.Get(), Plan.World->AddOnActorPreSpawnInitialization(FOnActorSpawned::FDelegate::CreateLambda(
                    [this](AActor* Actor)
                    {
                        // CAC supplies OverrideParentComponent before this public callback.
                        // Only the exact candidate component currently being created is owned.
                        if (!CreatingPrevious || Actor->GetParentComponent() != CreatingComponent) return;
                        UClass* Class = CreatingPrevious->GetClass();
                        if (UObject* Candidate = Types.FindRef(Class)) Class = CastChecked<UClass>(Candidate);
                        if (Actor->GetClass() != Class || Objects.Contains(CreatingPrevious))
                        {
                            Error = TEXT("A candidate child spawned more than once or with a different class.");
                            bSucceeded = false;
                            return;
                        }
                        Objects.Add(CreatingPrevious, Actor);
                        Defer(CreatingPrevious, Actor);
                    })));
    }
    ~FBlueprintChildCreation()
    {
        for (const auto& World : Worlds) World.Key->RemoveOnActorPreSpawnInitialization(World.Value);
        for (const auto& Template : Templates)
            ClassProperty->SetObjectPropertyValue_InContainer(Template.Component.Get(), Template.Class.Get());
        for (const auto& State : Deferred)
            if (IsValid(State.Component.Get()))
                ClassProperty->SetObjectPropertyValue_InContainer(State.Component.Get(), State.Class.Get());
    }
    void Defer(AActor* Previous, AActor* Candidate)
    {
        if (bSucceeded && !DeferBlueprintChildActorComponents(Previous, Candidate, Children, Types, Deferred, Error))
            bSucceeded = false;
    }
    bool IsChild(AActor* Actor) const
    { return Children.ContainsByPredicate([&](const auto& Item) { return Item.Previous.Get() == Actor; }); }
    bool AllReady(const TSet<AActor*>& Ready) const
    {
        for (const auto& Child : Children)
            if (!Ready.Contains(Cast<AActor>(Objects.FindRef(Child.Previous.Get())))) return false;
        return true;
    }
    bool CreateNext(const TSet<AActor*>& Ready, TFunctionRef<bool(AActor*, AActor*)> Customize, bool& Created)
    {
        Created = false;
        if (!bSucceeded) return false;
        for (const auto& Child : Children)
        {
            if (Started.Contains(Child.Previous.Get())) continue;
            auto* Component = Cast<UChildActorComponent>(Objects.FindRef(Child.Parent.Get()));
            if (!Component || !Ready.Contains(Component->GetOwner())) continue;
            FDeferredBlueprintChildActor State;
            State.Component.Reset(Component);
            UClass* Class = Child.Previous->GetClass();
            if (UObject* Candidate = Types.FindRef(Class)) Class = CastChecked<UClass>(Candidate);
            State.Class.Reset(Class);
            State.Template.Reset(Child.Parent->GetChildActorTemplate());
            Started.Add(Child.Previous.Get());
            TGuardValue<UChildActorComponent*> CreatingComponentGuard(CreatingComponent, Component);
            TGuardValue<AActor*> CreatingPreviousGuard(CreatingPrevious, Child.Previous.Get());
            if (!CreateDeferredBlueprintChildActor(State, [&](AActor* Actor)
                {
                    if (bSucceeded) bSucceeded = Customize(Child.Previous.Get(), Actor);
                }, Error) || !bSucceeded) return false;
            // CAC performs a final snap after FinishSpawning. Restore the user's
            // existing child-root offset after that stock attachment step.
            if (auto* Root = Component->GetChildActor()->GetRootComponent())
            {
                const FName Socket = Child.Previous->GetRootComponent() ? Child.Previous->GetRootComponent()->GetAttachSocketName() : NAME_None;
                Root->AttachToComponent(Component, FAttachmentTransformRules::KeepRelativeTransform, Socket);
                Root->SetRelativeTransform(Child.Relative);
            }
            Created = true;
            return true;
        }
        return true;
    }
};

bool PrepareBlueprintSubclassGenerations(FPendingBlueprintGeneration& Pending,
    TMap<UObject*, UObject*>& Types, TMap<UObject*, UObject*>& PreviousTypes, FString& Error)
{
    if (Pending.Subclasses.IsEmpty()) return true;
    auto& Objects = Pending.BlueprintMetadata;
    auto& Fields = Pending.BlueprintFields;
    Objects.Append(Types);
    Objects.Append(Pending.ActorTemplates);
    TFunction<bool(FField*, FField*)> AddField = [&](FField* Before, FField* After)
    {
        if (!After || Before->GetClass() != After->GetClass()) return false;
        Fields.Add(Before, After);
        TArray<FField*> BeforeInner, AfterInner;
        Before->GetInnerFields(BeforeInner);
        After->GetInnerFields(AfterInner);
        if (BeforeInner.Num() != AfterInner.Num()) return false;
        for (int32 Index = 0; Index < BeforeInner.Num(); ++Index)
            if (!AddField(BeforeInner[Index], AfterInner[Index])) return false;
        return true;
    };
    auto AddStruct = [&](UStruct* Before, UStruct* After)
    {
        for (TFieldIterator<FProperty> Field(Before, EFieldIteratorFlags::ExcludeSuper); Field; ++Field)
            if (!AddField(*Field, FindFProperty<FProperty>(After, Field->GetFName()))) return false;
        return true;
    };
    auto AddClass = [&](UClass* Before, UClass* After)
    {
        Objects.Add(Before, After);
        if (UObject* Defaults = Before->GetDefaultObject(false))
        {
            UObject* ReplacementDefaults = After->GetDefaultObject();
            Objects.Add(Defaults, ReplacementDefaults);
            TArray<UObject*> DefaultObjects;
            SoloASBridge::Compat::GetNestedObjects(Defaults, DefaultObjects);
            for (UObject* Object : DefaultObjects)
                if (UObject* Replacement = FindObject<UObject>(ReplacementDefaults, *Object->GetPathName(Defaults)))
                    Objects.Add(Object, Replacement);
        }
        if (!AddStruct(Before, After)) return false;
        for (TFieldIterator<UFunction> Function(Before, EFieldIteratorFlags::ExcludeSuper); Function; ++Function)
        {
            UFunction* Replacement = After->FindFunctionByName(Function->GetFName(), EIncludeSuperFlag::ExcludeSuper);
            if (!Replacement || !AddStruct(*Function, Replacement)) return false;
            Objects.Add(*Function, Replacement);
        }
        return true;
    };
    for (const auto& Pair : Types)
        if (!AddClass(CastChecked<UClass>(Pair.Key), CastChecked<UClass>(Pair.Value)))
        { Error = TEXT("A prepared parent has no complete declaring-field/function map."); return false; }
    for (auto& Subclass : Pending.Subclasses)
    {
        UClass* Before = Subclass.Previous.Get();
        if (BlueprintImportAbi(Before) != Subclass.Schema || Before->GetPathName() != Subclass.ClassPath)
        { Error = TEXT("A loaded Blueprint subclass changed after its schema snapshot."); return false; }
        const FString PackageName = TEXT("/Temp/SoloASBlueprintSubclass_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Subclass.Package.Reset(CreatePackage(*PackageName));
#if WITH_EDITORONLY_DATA
        UBlueprint* SourceBlueprint = Cast<UBlueprint>(Before->ClassGeneratedBy);
        UBlueprint* Blueprint = nullptr;
        if (SourceBlueprint)
        {
            if (SourceBlueprint->GetClass() != UBlueprint::StaticClass()
                && SourceBlueprint->GetClass() != USoloASScriptBlueprint::StaticClass())
            { Error = TEXT("A loaded Blueprint subclass uses an unsupported asset compiler."); return false; }
            Blueprint = NewObject<UBlueprint>(Subclass.Package.Get(), SourceBlueprint->GetClass(), SourceBlueprint->GetFName(), RF_Transient);
            Objects.Add(SourceBlueprint, Blueprint);
            for (TFieldIterator<FProperty> Field(SourceBlueprint->GetClass()); Field; ++Field)
                Field->CopyCompleteValue_InContainer(Blueprint, SourceBlueprint);
        }
#endif
        auto Duplicate = [&](UObject* Source, UObject* Outer) -> UObject*
        {
            FObjectDuplicationParameters Parameters(Source, Outer);
            Parameters.DestName = Source->GetFName();
            Parameters.ApplyFlags = RF_Transient;
            Parameters.FlagMask &= ~RF_Standalone;
            Parameters.bSkipPostLoad = true;
            Parameters.DuplicationSeed = Objects;
            TMap<UObject*, UObject*> Created;
            Parameters.CreatedObjects = &Created;
            UObject* Result = StaticDuplicateObjectEx(Parameters);
            Objects.Append(Created);
            return Result;
        };
        auto CloneClass = [&](UClass* Source) -> UClass*
        {
            UClass* Candidate = Cast<UClass>(Duplicate(Source, Subclass.Package.Get()));
            if (!Candidate) return nullptr;
            // A UClass duplicate can retain the source CDO (a package sibling).
            // Relink before allocating this class's own default layout.
            Candidate->SetDefaultObject(nullptr);
            UClass* Parent = Source->GetSuperClass();
            if (UObject* Mapped = Objects.FindRef(Parent)) Parent = CastChecked<UClass>(Mapped);
            Candidate->SetSuperStruct(Parent);
            Candidate->ClassFlags &= ~CLASS_NewerVersionExists;
            Candidate->Bind();
            Candidate->StaticLink(true);
            for (TFieldIterator<UFunction> Function(Candidate, EFieldIteratorFlags::ExcludeSuper); Function; ++Function)
            {
                Function->StaticLink(true);
                Function->Bind();
            }
            Candidate->AssembleReferenceTokenStream(true);
#if WITH_EDITORONLY_DATA
            if (Blueprint)
            {
                Candidate->ClassGeneratedBy = Blueprint;
                Blueprint->ParentClass = Parent;
                if (Source == Before) Blueprint->GeneratedClass = Candidate;
                else Blueprint->SkeletonGeneratedClass = Candidate;
            }
#endif
            UObject* Defaults = Candidate->GetDefaultObject();
            if (!Defaults || Defaults->GetClass() != Candidate) return nullptr;
            if (UObject* PreviousDefaults = Source->GetDefaultObject(false)) Objects.Add(PreviousDefaults, Defaults);
            return Candidate;
        };
        Subclass.Candidate.Reset(CloneClass(Before));
        if (!Subclass.Candidate.IsValid())
        { Error = TEXT("A private Blueprint subclass did not create its own linked CDO."); return false; }
        UClass* Candidate = Subclass.Candidate.Get();
        Types.Add(Before, Candidate);
        PreviousTypes.Add(Candidate, Before);
#if WITH_EDITORONLY_DATA
        if (SourceBlueprint && SourceBlueprint->SkeletonGeneratedClass
            && SourceBlueprint->SkeletonGeneratedClass != Before)
            if (!CloneClass(SourceBlueprint->SkeletonGeneratedClass))
            { Error = TEXT("A private Blueprint subclass has no linked skeleton CDO."); return false; }
        if (SourceBlueprint)
        {
            TArray<UObject*> Owned;
            GetObjectsWithOuter(SourceBlueprint, Owned);
            for (UObject* Object : Owned)
                if (!Objects.Contains(Object) && !Duplicate(Object, Blueprint))
                { Error = TEXT("A Blueprint asset-owned graph could not be cloned privately."); return false; }
        }
#endif
        UObject* BeforeDefaults = Before->GetDefaultObject(false);
        UObject* AfterDefaults = Candidate->GetDefaultObject(false);
        TArray<UObject*> DefaultsGraph;
        SoloASBridge::Compat::GetNestedObjects(BeforeDefaults, DefaultsGraph);
        DefaultsGraph.StableSort([](const UObject& A, const UObject& B)
        {
            auto Depth = [](const UObject* Object) { int32 Count = 0; for (; Object; Object = Object->GetOuter()) ++Count; return Count; };
            return Depth(&A) < Depth(&B);
        });
        for (UObject* Object : DefaultsGraph)
        {
            UObject* Outer = Objects.FindRef(Object->GetOuter());
            if (!Outer) { Error = TEXT("A Blueprint default graph has no mapped Outer."); return false; }
            UClass* Class = Object->GetClass();
            if (UObject* Mapped = Types.FindRef(Class)) Class = CastChecked<UClass>(Mapped);
            UObject* Replacement = StaticFindObjectFast(UObject::StaticClass(), Outer, Object->GetFName());
            if (!Replacement)
                Replacement = NewObject<UObject>(Outer, Class, Object->GetFName(),
                    Object->GetFlags() & (RF_DefaultSubObject | RF_ArchetypeObject | RF_Transactional | RF_Transient));
            if (!Replacement || Replacement->GetClass() != Class)
            { Error = TEXT("A Blueprint default subobject changed its class or owner identity."); return false; }
            Objects.Add(Object, Replacement);
        }
        DefaultsGraph.Insert(BeforeDefaults, 0);
        for (UObject* Object : DefaultsGraph)
        {
            UObject* Replacement = Objects.FindChecked(Object);
            UObject* Archetype = Object == BeforeDefaults ? Before->GetSuperClass()->GetDefaultObject(false) : Object->GetArchetype();
            for (TFieldIterator<FProperty> Field(Object->GetClass()); Field; ++Field)
            {
                if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient)) continue;
                FProperty* Destination = FindFProperty<FProperty>(Replacement->GetClass(), Field->GetFName());
                if (!Destination || Destination->GetClass() != Field->GetClass()
                    || Destination->GetElementSize() != Field->GetElementSize() || Destination->ArrayDim != Field->ArrayDim)
                { Error = TEXT("A Blueprint default field has no compatible candidate storage."); return false; }
                const bool bOwnDefault = Object == BeforeDefaults && Field->GetOwnerClass() == Before;
                if (bOwnDefault || !Archetype || !Field->Identical_InContainer(Object, Archetype))
                    Destination->CopyCompleteValue(Destination->ContainerPtrToValuePtr<void>(Replacement), Field->ContainerPtrToValuePtr<void>(Object));
            }
        }
        if (!AddClass(Before, Candidate))
        { Error = TEXT("A private Blueprint subclass has no complete declaring-field/function map."); return false; }
        for (const auto& Pair : Objects)
            if (auto* OldStruct = Cast<UStruct>(Pair.Key); OldStruct && Pair.Value->IsA<UStruct>())
                if (!AddStruct(OldStruct, CastChecked<UStruct>(Pair.Value)))
                { Error = TEXT("A private metadata field changed its shape."); return false; }
        TArray<UObject*> Metadata;
        SoloASBridge::Compat::GetPackageObjects(Subclass.Package.Get(), Metadata);
        for (UObject* Object : Metadata)
        {
            RebindBlueprintGenerationMetadata(Object, Objects, Fields);
            Pending.BlueprintMetadataPins.Emplace(Object);
        }
        CastChecked<UBlueprintGeneratedClass>(Candidate)->UpdateCustomPropertyListForPostConstruction();
        FBlueprintGenerationSchema Schema;
        if (!BuildBlueprintGenerationSchema(Before, Candidate, PreviousTypes, Schema, Error)) return false;
        // Duplication defers PostLoad while this loaded graph is relinked to its
        // new parent, defaults and metadata. That initialization is now complete;
        // do not replay asset-load fixups on the rebuilt graph or leave the
        // duplicate writer's intermediate state on objects that will publish.
        for (UObject* Object : Metadata)
            Object->ClearFlags(RF_NeedPostLoad | RF_NeedPostLoadSubobjects);
        Pending.ActorTemplates.Append(Objects);
    }
    return true;
}

bool PrepareBlueprintReceiverCommit(FPendingBlueprintGeneration& Pending, FString& Error)
{
    auto VisitReferences = [&](FProperty* Property, void* Value, FBlueprintObjectReference Visit, bool bReplace)
    {
        return VisitBlueprintGenerationReferences(Pending, Property, Value, Visit, bReplace);
    };
    TMap<UObject*, UObject*> Types;
    TMap<UObject*, UObject*> PreviousTypes;
    if (!Pending.bScriptParentOnly)
    {
        Types.Add(Pending.Previous.Get(), Pending.Candidate.Get());
        PreviousTypes.Add(Pending.Candidate.Get(), Pending.Previous.Get());
    }
    for (auto& Receiver : Pending.Receivers)
    {
        if (!Receiver.Candidate || Receiver.Candidate == Receiver.Previous.Get())
        {
            Error = TEXT("The prepared receiver did not produce an actual class generation after module swap.");
            return false;
        }
        Types.Add(Receiver.Previous.Get(), Receiver.Candidate);
        PreviousTypes.Add(Receiver.Candidate, Receiver.Previous.Get());
        for (const auto& Field : Receiver.TypedFields)
            if (Field.Key->PropertyClass != Field.Value)
            {
                Error = TEXT("Editor reinstancing changed retained receiver type metadata; explicit Editor coordination is required.");
                return false;
            }
    }
    if (!PrepareBlueprintSubclassGenerations(Pending, Types, PreviousTypes, Error)) return false;
    TMap<UClass*, FBlueprintGenerationSchema> Schemas;
    for (const auto& Pair : Types)
    {
        UClass* Previous = CastChecked<UClass>(Pair.Key);
        UClass* Candidate = CastChecked<UClass>(Pair.Value);
        FBlueprintGenerationSchema& Schema = Schemas.FindOrAdd(Previous);
        const auto* Receiver = Pending.Receivers.FindByPredicate([&](const auto& Item) { return Item.Previous.Get() == Previous; });
        if (!Receiver)
        {
            if (!BuildBlueprintGenerationSchema(Previous, Candidate, PreviousTypes, Schema, Error)) return false;
            continue;
        }
        const auto After = BlueprintImportAbi(Candidate, &PreviousTypes, false);
        for (const auto& Row : Receiver->Schema)
            if (!After.Contains(Row))
            {
                Error = TEXT("A committed receiver does not match its prepared reflected copy schema.");
                return false;
            }
        for (TFieldIterator<FProperty> Field(Previous); Field; ++Field)
        {
            FProperty* Replacement = FindFProperty<FProperty>(Candidate, Field->GetFName());
            if (!Replacement) { Error = TEXT("A prepared receiver field disappeared after swap."); return false; }
            Schema.Properties.Add(*Field, Replacement);
        }
    }
    // Validate every generated CDO before creating any replacement, including
    // replacements supplied by Editor reinstancing.
    for (const auto& Plan : Pending.Actors)
        if (Plan.NativeRoot.IsValid())
        {
            UClass* Candidate = Cast<UClass>(Types.FindRef(Plan.Previous->GetClass()));
            auto* Defaults = Candidate ? Cast<AActor>(Candidate->GetDefaultObject()) : nullptr;
            UClass* NativeParent = Candidate;
            while (NativeParent && !NativeParent->HasAnyClassFlags(CLASS_Native)) NativeParent = NativeParent->GetSuperClass();
            if (!Defaults || !BlueprintNativeSceneRoot(Defaults) || !NativeParent
                || !BlueprintNativeGraphMatches(CastChecked<AActor>(Candidate->GetDefaultObject()),
                    CastChecked<AActor>(NativeParent->GetDefaultObject())))
            { Error = TEXT("The generated candidate CDO changed its admitted native default root; the previous generation is unchanged."); return false; }
        }
    Pending.Migration = MakeUnique<FBlueprintReceiverMigration>();
    auto& Transaction = *Pending.Migration;
    auto& SpawnedActors = Transaction.SpawnedActors;
    auto& NewInstances = Transaction.NewInstances;
    auto& Objects = Transaction.Objects;
    FBlueprintChildCreation ChildCreation(Pending, Types, Objects, Error);
    if (!ChildCreation.bSucceeded) return false;
    if (!Pending.Subclasses.IsEmpty())
        Pending.PreviousInstances.StableSort([](const auto& A, const auto& B)
        {
            auto Depth = [](UObject* Object) { int32 Count = 0; for (; Object; Object = Object->GetOuter()) ++Count; return Count; };
            return Depth(A.Get()) < Depth(B.Get());
        });
    for (const auto& Previous : Pending.PreviousInstances)
    {
        UClass* PreviousClass = nullptr;
        for (const auto& Receiver : Pending.Receivers)
            if (Previous->GetClass() == Receiver.Previous.Get()) PreviousClass = Receiver.Previous.Get();
        if (!Pending.bScriptParentOnly && Previous->GetClass() == Pending.Previous.Get()) PreviousClass = Pending.Previous.Get();
        for (const auto& Subclass : Pending.Subclasses)
            if (Previous->GetClass() == Subclass.Previous.Get()) PreviousClass = Subclass.Previous.Get();
        if (!PreviousClass && Pending.Instancing->Contains(Previous.Get())) PreviousClass = Previous->GetClass();
        if (!PreviousClass)
        {
            Error = TEXT("Editor reinstancing changed a retained receiver's class identity; explicit Editor coordination is required.");
            return false;
        }
        if (ChildCreation.IsChild(Cast<AActor>(Previous.Get()))) continue;
        UClass* Candidate = Cast<UClass>(Types.FindRef(PreviousClass));
        if (!Candidate) Candidate = PreviousClass;
        UObject* Replacement = Pending.ReinstancedObjects.FindRef(Previous.Get());
        if (Replacement && Replacement->GetClass() != Candidate)
        {
            Error = TEXT("Editor returned an incompatible receiver replacement after module swap.");
            return false;
        }
        if (!Replacement)
        {
            const EObjectFlags Flags = Previous->GetFlags() & (RF_Public | RF_Standalone | RF_Transient | RF_Transactional);
            if (AActor* Actor = Cast<AActor>(Previous.Get()))
            {
                const auto* Plan = Pending.Actors.FindByPredicate([&](const auto& Item) { return Item.Previous.Get() == Actor; });
                if (!Plan || !IsValid(Plan->World.Get()) || Plan->World->bIsTearingDown
                    || Actor->GetLevel() != Plan->Level.Get() || Actor->IsActorBeingDestroyed())
                { Error = TEXT("The Actor lifecycle changed after final admission; the previous generation is unchanged."); return false; }
                FActorSpawnParameters Spawn;
                Spawn.OverrideLevel = Plan->Level.Get();
                Spawn.ObjectFlags = Flags;
                Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                Spawn.bDeferConstruction = true;
                SoloASBridge::Compat::SetPreSpawnInitialization(Spawn, [&](AActor* CandidateActor) { ChildCreation.Defer(Actor, CandidateActor); });
                Replacement = Plan->World->SpawnActor(Candidate, &Plan->Transform, Spawn);
                if (Replacement) SpawnedActors.Add(CastChecked<AActor>(Replacement));
                if (!Replacement)
                { Error = TEXT("Deferred Actor replacement failed during candidate preparation; the previous generation is unchanged."); return false; }
            }
            else
            {
                UObject* Outer = Objects.FindRef(Previous->GetOuter());
                Replacement = NewObject<UObject>(Outer ? Outer : Previous->GetOuter(), Candidate, NAME_None, Flags);
            }
        }
        Objects.Add(Previous.Get(), Replacement);
        NewInstances.Emplace(Replacement);
        if (!ChildCreation.bSucceeded) return false;
    }
    // Deferred stock spawn has already constructed native default subobjects.
    // Complete the reference map before copying any business field or cycle.
    for (const auto& Plan : Pending.Actors)
        if (Plan.NativeRoot.IsValid())
        {
            AActor* Actor = CastChecked<AActor>(Objects.FindChecked(Plan.Previous.Get()));
            USceneComponent* Root = BlueprintNativeSceneRoot(Actor);
            if (!Root || !BlueprintNativeGraphMatches(Actor, CastChecked<AActor>(Actor->GetClass()->GetDefaultObject())))
            { Error = TEXT("Deferred stock spawn changed its admitted native root; the previous generation is unchanged."); return false; }
            for (const auto& State : Plan.NativeGraph)
            {
                auto* Component = FindObject<UActorComponent>(Actor, *State.Previous->GetName());
                if (!Component || Component->GetClass() != State.Previous->GetClass())
                { Error = TEXT("Deferred stock spawn changed a native graph identity; the previous generation is unchanged."); return false; }
                Objects.Add(State.Previous.Get(), Component);
                if (auto* Scene = Cast<USceneComponent>(Component); Scene && Scene != Root)
                    Scene->SetRelativeTransform(CastChecked<USceneComponent>(State.Previous.Get())->GetRelativeTransform());
            }
            Root->SetWorldTransform(Plan.NativeRoot->GetComponentTransform(), false, nullptr, ETeleportType::TeleportPhysics);
        }
    if (!Pending.Instancing->Allocate(Objects, Types, false, Error)) return false;
    // Every target exists before copying any typed reference or receiver cycle.
    for (const auto& Pair : Objects)
    {
        if (Pair.Key->IsA<UActorComponent>()) continue; // Component instance data is applied below.
        UClass* PreviousClass = Pair.Key->GetClass();
        if (!Schemas.Contains(PreviousClass)) continue; // Owned nodes use their saved reflected delta.
        for (const auto& Property : Schemas.FindChecked(PreviousClass).Properties)
            if ((!Pair.Key->IsA<AActor>() || (IsBlueprintActorStateField(Property.Key)
                    && !BlueprintActorComponentSlot(PreviousClass, Property.Key)))
                && !Property.Key->Identical_InContainer(Pair.Key, PreviousClass->GetDefaultObject()))
                Property.Value->CopyCompleteValue(Property.Value->ContainerPtrToValuePtr<void>(Pair.Value),
                    Property.Key->ContainerPtrToValuePtr<void>(Pair.Key));
        for (const auto& Receiver : Pending.Receivers)
            if (PreviousClass->IsChildOf(Receiver.Previous.Get()))
                for (const auto& Value : Receiver.Values)
                {
                    void* Before = reinterpret_cast<uint8*>(Pair.Key) + Value.PreviousOffset;
                    void* Default = reinterpret_cast<uint8*>(PreviousClass->GetDefaultObject()) + Value.PreviousOffset;
                    void* After = reinterpret_cast<uint8*>(Pair.Value) + Value.CandidateOffset;
                    if (!Value.Type.IsValueEqualForHotReload(Before, Default))
                    {
                        if (Value.Type.NeedCopy()) Value.Type.CopyValue(Before, After);
                        else FMemory::Memcpy(After, Before, Value.Type.GetValueSize());
                    }
                }
        for (TFieldIterator<FProperty> Field(Pair.Value->GetClass()); Field; ++Field)
            if ((!Pair.Value->IsA<AActor>() || (IsBlueprintActorStateField(*Field)
                    && !BlueprintActorComponentSlot(Pair.Value->GetClass(), *Field)))
                && !VisitBlueprintGenerationReferences(Pending, *Field, Field->ContainerPtrToValuePtr<void>(Pair.Value),
                [&](UObject*& Value, UClass* Constraint, bool bInterface)
                {
                    if (UObject* Replacement = Objects.FindRef(Value)) Value = Replacement;
                    return !Value || (bInterface ? Value->GetClass()->ImplementsInterface(Constraint) : Value->IsA(Constraint));
                }, true))
            {
                Error = TEXT("A candidate receiver field cannot accept its mapped instance generation after module swap.");
                return false;
            }
    }
    if (Pending.Actors.IsEmpty() && Pending.Instancing->HasMigration())
        if (!Pending.Instancing->Allocate(Objects, Types, true, Error)
            || !Pending.Instancing->Copy(Objects, VisitReferences, Error)) return false;
    auto ApplyReferenceWrites = [&]() -> bool
    {
        // Preflight every still-current native owner again before the first write.
        for (const auto& Reference : Pending.CurrentReferences)
            if (UObject* Owner = Reference.Owner.Get())
            {
                if (!IsBlueprintGenerationInstance(Owner) || FindFProperty<FProperty>(Owner->GetClass(), Reference.Property->GetFName()) != Reference.Property)
                {
                    Error = TEXT("A current native reference owner changed after receiver preparation.");
                    return false;
                }
                if (!VisitBlueprintGenerationReferences(Pending, Reference.Property, Reference.Property->ContainerPtrToValuePtr<void>(Owner),
                    [&](UObject*& Value, UClass* Constraint, bool bInterface)
                    {
                        UObject* Replacement = Objects.FindRef(Value);
                        return !Replacement || (bInterface ? Replacement->GetClass()->ImplementsInterface(Constraint) : Replacement->IsA(Constraint));
                    }))
                {
                    Error = TEXT("A current native reference no longer accepts the receiver generation.");
                    return false;
                }
            }
        for (const auto& Reference : Pending.CurrentReferences)
            if (UObject* Owner = Reference.Owner.Get())
            {
                auto Snapshot = MakeUnique<FBlueprintReceiverMigration::FPropertyWrite>(Owner, Reference.Property);
                if (!Snapshot->SparseBefore.IsSet() && !VisitBlueprintGenerationReferences(Pending, Reference.Property, Snapshot->Before,
                    [&](UObject*& Value, UClass*, bool) { if (Value) Snapshot->References.Emplace(Value); return true; }))
                { Error = TEXT("A current holder could not be saved for reversible publication."); return false; }
                Transaction.Properties.Add(MoveTemp(Snapshot));
            }
        for (const auto& Snapshot : Transaction.Properties)
        {
            Snapshot->bAttempted = true;
            if (!VisitBlueprintGenerationReferences(Pending, Snapshot->Property,
                Snapshot->Property->ContainerPtrToValuePtr<void>(Snapshot->Owner.Get()),
                [&](UObject*& Value, UClass*, bool)
                {
                    if (UObject* Replacement = Objects.FindRef(Value)) Value = Replacement;
                    return true;
                }, true))
            { Error = TEXT("A current holder rejected reversible reference publication."); return false; }
        }
        for (TObjectIterator<AActor> It; It; ++It)
            if (IsValid(*It) && !Objects.Contains(*It))
            {
                AActor* Owner = Cast<AActor>(Objects.FindRef(It->GetOwner()));
                APawn* Instigator = Cast<APawn>(Objects.FindRef(It->GetInstigator()));
                if (!Owner && !Instigator) continue;
                Transaction.Actors.Add({TStrongObjectPtr<AActor>(*It), TStrongObjectPtr<AActor>(It->GetOwner()),
                    TStrongObjectPtr<APawn>(It->GetInstigator())});
                if (Owner) It->SetOwner(Owner);
                if (Instigator) It->SetInstigator(Instigator);
            }
        return true;
    };
    if (!Pending.Actors.IsEmpty())
    {
        TMap<UObject*, UObject*> CacheMap = Pending.ActorTemplates;
        CacheMap.Append(Types);
        CacheMap.Append(Objects);
        auto Mapped = [&](UObject* Object) { UObject* Replacement = Objects.FindRef(Object); return Replacement ? Replacement : Object; };
        for (auto& Plan : Pending.Actors)
        {
            AActor* Actor = Cast<AActor>(Objects.FindRef(Plan.Previous.Get()));
            if (!Actor) continue; // CAC creates its child after the parent SCS boundary.
            Actor->SetOwner(Cast<AActor>(Mapped(Plan.Owner.Get())));
            Actor->SetInstigator(Cast<APawn>(Mapped(Plan.Instigator.Get())));
            FBlueprintActorCacheReferences References(Plan.CacheReferences, &CacheMap);
            if (Plan.Components)
            {
                Plan.Components->AddReferencedObjects(References);
                Plan.Components->FindAndReplaceInstances(Objects);
            }
            for (auto& State : Plan.NativeGraph)
            {
                State.Data->AddReferencedObjects(References);
                State.Data->FindAndReplaceInstances(Objects);
                // Restore editable archetype deltas as Construction inputs.
                // Stock excludes root transforms; deferred spawn handles those.
                State.Data->ApplyToComponent(
                    CastChecked<UActorComponent>(Objects.FindChecked(State.Previous.Get())),
                    ECacheApplyPhase::NonConstructionScript);
                UObject* Root = Objects.FindChecked(State.Previous.Get());
                for (FProperty* Property : State.Properties)
                {
                    Property->CopyCompleteValue_InContainer(Root, State.Previous.Get());
                    if (!VisitBlueprintGenerationReferences(Pending, Property, Property->ContainerPtrToValuePtr<void>(Root),
                        [&](UObject*& Value, UClass* Constraint, bool bInterface)
                        {
                            if (UObject* Replacement = CacheMap.FindRef(Value)) Value = Replacement;
                            return !Value || (bInterface ? Value->GetClass()->ImplementsInterface(Constraint) : Value->IsA(Constraint));
                        }, true))
                    { Error = TEXT("A native root business delta rejected its mapped generation after module swap."); return false; }
                }
            }
        }
        // Explicit roots only: do not serialize World/Level lists or the retired graph.
        for (const auto& Instance : NewInstances)
            if (!Instance->IsA<AActor>())
            {
                FArchiveReplaceObjectRef<UObject> Replace(Instance.Get(), Objects,
                    EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
            }
        struct FAttachmentState
        {
            AActor* Previous;
            USceneComponent* Parent;
            FName Socket;
            TSet<AActor*> Children;
        };
        TArray<FAttachmentState> Attachments;
        struct FAttachmentEdge { AActor* Child; USceneComponent* Parent; FName Socket; bool bRestored = false; };
        TArray<FAttachmentEdge> AttachmentEdges;
        auto OwnedChild = [](AActor* Child, AActor* Owner)
        { return Child && Child->GetParentComponent() && Child->GetParentComponent()->GetOwner() == Owner; };
        TMap<UWorld*, TSet<TWeakObjectPtr<AActor>>> BeforeConstruction;
        TSet<TWeakObjectPtr<AActor>> OwnedChildren;
        auto RecordOwnedChildren = [&](AActor* Parent)
        {
            TArray<AActor*> Parents{Parent};
            for (int32 Index = 0; Index < Parents.Num(); ++Index)
            {
                TInlineComponentArray<UChildActorComponent*> Components(Parents[Index]);
                for (UChildActorComponent* Component : Components)
                    if (AActor* Child = Component->GetChildActor(); Child
                        && Child->GetParentComponent() == Component && Component->GetOwner() == Parents[Index]
                        && !OwnedChildren.Contains(Child))
                    {
                        OwnedChildren.Add(Child);
                        Parents.Add(Child);
                    }
            }
        };
        for (const auto& Plan : Pending.Actors)
        {
            auto& State = Attachments.AddDefaulted_GetRef();
            State.Previous = Plan.Previous.Get();
            USceneComponent* Root = State.Previous->GetRootComponent();
            State.Parent = Root ? Root->GetAttachParent() : nullptr;
            State.Socket = Root ? Root->GetAttachSocketName() : NAME_None;
            if (State.Parent) AttachmentEdges.Add({State.Previous, State.Parent, State.Socket});
            TArray<AActor*> Children;
            State.Previous->GetAttachedActors(Children);
            for (AActor* Child : Children) if (!OwnedChild(Child, State.Previous))
            {
                State.Children.Add(Child);
                auto* ChildRoot = Child->GetRootComponent();
                AttachmentEdges.Add({Child, ChildRoot->GetAttachParent(), ChildRoot->GetAttachSocketName()});
            }
            RecordOwnedChildren(State.Previous);
            BeforeConstruction.FindOrAdd(Plan.World.Get());
        }
        auto MappedScene = [&](USceneComponent* Component) -> USceneComponent*
        {
            if (!Component) return nullptr;
            AActor* Owner = Cast<AActor>(Mapped(Component->GetOwner()));
            return Owner == Component->GetOwner() ? Component : FindObject<USceneComponent>(Owner, *Component->GetName());
        };
        auto RestoreAttachments = [&]()
        {
            for (auto& Edge : AttachmentEdges)
            {
                if (Edge.bRestored) continue;
                AActor* Child = CastChecked<AActor>(Mapped(Edge.Child));
                USceneComponent* Root = Child->GetRootComponent();
                USceneComponent* Parent = MappedScene(Edge.Parent);
                if (Root && Parent && Root->GetAttachParent() == Parent && Root->GetAttachSocketName() == Edge.Socket) Edge.bRestored = true;
                if (Root && Parent && !Edge.bRestored)
                {
                    Transaction.Attachments.Add({TStrongObjectPtr<USceneComponent>(Root),
                        TStrongObjectPtr<USceneComponent>(Root->GetAttachParent()), Root->GetAttachSocketName(),
                        Root->GetRelativeTransform(), Root->GetComponentTransform()});
                    Edge.bRestored = Root->AttachToComponent(Parent, FAttachmentTransformRules::KeepWorldTransform, Edge.Socket);
                }
            }
        };
        // Game's TActorIterator also finds component-owned Actor templates.
        // World membership is the Level actor lists on both targets.
        auto SnapshotWorldActors = [](UWorld* World, FString& LevelSummary)
        {
            TSet<TWeakObjectPtr<AActor>> Actors;
            TSet<ULevel*> Levels;
            Levels.Add(World->PersistentLevel);
            for (ULevel* Level : World->GetLevels()) Levels.Add(Level);
            for (ULevel* Level : Levels)
            {
                if (!LevelSummary.IsEmpty()) LevelSummary += TEXT(", ");
                LevelSummary += GetPathNameSafe(Level);
                if (!Level) continue;
                for (AActor* Actor : Level->Actors)
                    if (IsValid(Actor)) Actors.Add(Actor);
            }
            return Actors;
        };
        // Two World scans per batch: O(world Actors + replaced Actors + attachment edges).
        TMap<UWorld*, FString> BeforeConstructionLevels;
        for (auto& World : BeforeConstruction)
            World.Value = SnapshotWorldActors(World.Key, BeforeConstructionLevels.FindOrAdd(World.Key));
        TArray<int32> RootPlans;
        for (int32 Index = 0; Index < Pending.Actors.Num(); ++Index)
            if (!ChildCreation.IsChild(Pending.Actors[Index].Previous.Get())) RootPlans.Add(Index);
        int32 ConstructionIndex = 0;
        const bool bExpanded = Pending.Instancing->HasMigration();
        bool bInstanced = false;
        TSet<AActor*> Ready;
        TFunction<bool()> ConstructNext;
        TFunction<bool(AActor*)> CompleteGraphs;
        FScriptBlueprintConstruction Construction;
        auto CopyUnreflected = [&](AActor* Previous, AActor* Candidate)
        {
            for (const auto& Receiver : Pending.Receivers)
                if (Previous->GetClass()->IsChildOf(Receiver.Previous.Get()))
                    for (const auto& Value : Receiver.Values)
                    {
                        void* Before = reinterpret_cast<uint8*>(Previous) + Value.PreviousOffset;
                        void* Default = reinterpret_cast<uint8*>(Previous->GetClass()->GetDefaultObject()) + Value.PreviousOffset;
                        void* After = reinterpret_cast<uint8*>(Candidate) + Value.CandidateOffset;
                        if (!Value.Type.IsValueEqualForHotReload(Before, Default))
                        {
                            if (Value.Type.NeedCopy()) Value.Type.CopyValue(Before, After);
                            else FMemory::Memcpy(After, Before, Value.Type.GetValueSize());
                        }
                    }
        };
        CompleteGraphs = [&](AActor* Actor) -> bool
        {
            Ready.Add(Actor);
            if (ConstructionIndex < RootPlans.Num() && !ConstructNext()) return false;
            if (!Pending.Instancing->Allocate(Objects, Types, false, Error)) return false;
            Pending.Instancing->RestoreTransforms(Objects);
            bool Created = false;
            do
            {
                if (!ChildCreation.CreateNext(Ready, [&](AActor* Previous, AActor* Candidate)
                    {
                        NewInstances.Emplace(Candidate);
                        SpawnedActors.Add(Candidate);
                        CopyUnreflected(Previous, Candidate);
                        if (Types.Contains(Previous->GetClass())) return Construction.AddActor(Candidate, Error);
                        // Native children have no SCS; the CAC Customizer is their
                        // public pre-OnConstruction/BeginPlay graph boundary.
                        return CompleteGraphs(Candidate);
                    }, Created)) return false;
            } while (Created);
            if (!ChildCreation.AllReady(Ready))
            { Error = TEXT("The candidate child graph has no constructed parent slot."); return false; }
            if (bInstanced) return true;
            if (!Pending.Instancing->Allocate(Objects, Types, true, Error)
                || !Pending.Instancing->Copy(Objects, VisitReferences, Error)) return false;
            TMap<UObject*, UObject*> Templates = Pending.ActorTemplates;
            Templates.Append(Types);
            Pending.Instancing->ApplyComponentData(Objects, Templates, ECacheApplyPhase::NonConstructionScript);
            if (!Pending.Instancing->Allocate(Objects, Types, true, Error)
                || !Pending.Instancing->Copy(Objects, VisitReferences, Error)) return false;
            for (const auto& Plan : Pending.Actors)
            {
                auto* Candidate = CastChecked<AActor>(Objects.FindChecked(Plan.Previous.Get()));
                Candidate->SetOwner(Cast<AActor>(Mapped(Plan.Owner.Get())));
                Candidate->SetInstigator(Cast<APawn>(Mapped(Plan.Instigator.Get())));
            }
            Pending.Instancing->RegisterComponents(Objects);
            bInstanced = true;
            return true;
        };
        if (bExpanded)
        {
            TArray<AActor*> Candidates;
            for (int32 Index : RootPlans)
            {
                const auto& Plan = Pending.Actors[Index];
                if (Types.Contains(Plan.Previous->GetClass())) Candidates.Add(CastChecked<AActor>(Objects.FindChecked(Plan.Previous.Get())));
            }
            if (!Construction.Prepare(Candidates, CompleteGraphs, [&](AActor* Actor)
                {
                    TMap<UObject*, UObject*> Templates = Pending.ActorTemplates;
                    Templates.Append(Types);
                    Pending.Instancing->ApplyComponentData(Objects, Templates, ECacheApplyPhase::PostUserConstructionScript, Actor);
                    return Pending.Instancing->Allocate(Objects, Types, true, Error)
                        && Pending.Instancing->Rebind(Objects, VisitReferences, Error);
                }, Error)) return false;
        }
        ConstructNext = [&]() -> bool
        {
            const int32 PlanIndex = RootPlans[ConstructionIndex++];
            auto& Plan = Pending.Actors[PlanIndex];
            AActor* Actor = CastChecked<AActor>(Objects.FindChecked(Plan.Previous.Get()));
            // Business deltas are Construction inputs. Receiver-local outputs
            // win; do not restore old values over the stock Construction result.
            AActor* ConstructionOwner = Actor->GetOwner();
            APawn* ConstructionInstigator = Actor->GetInstigator();
            const auto& Attachment = Attachments[PlanIndex];
            const bool bDeferredRoot = !Actor->GetRootComponent();
            USceneComponent* ConstructionParent = Actor->GetRootComponent() ? Actor->GetRootComponent()->GetAttachParent() : nullptr;
            const FName ConstructionSocket = Actor->GetRootComponent() ? Actor->GetRootComponent()->GetAttachSocketName() : NAME_None;
            TArray<AActor*> Attached;
            Actor->GetAttachedActors(Attached);
            TSet<AActor*> ConstructionChildren;
            for (AActor* Child : Attached) if (!OwnedChild(Child, Actor)) ConstructionChildren.Add(Child);
            if (!Types.Contains(Plan.Previous->GetClass()) && !CompleteGraphs(Actor)) return false;
            Actor->FinishSpawning(Plan.Transform, true, Plan.Components.Get());
            if (bExpanded && Types.Contains(Plan.Previous->GetClass())
                && (!Construction.Entered(Actor) || !Construction.Succeeded()))
            { if (Error.IsEmpty()) Error = TEXT("The stock construction boundary did not complete its Blueprint instance map."); return false; }
            if (!IsValid(Actor) || Actor->IsActorBeingDestroyed())
            { Error = TEXT("Actor construction retired the replacement during candidate preparation; the previous generation is unchanged."); return false; }
            Attached.Reset();
            Actor->GetAttachedActors(Attached);
            TSet<AActor*> AfterChildren;
            for (AActor* Child : Attached) if (!OwnedChild(Child, Actor)) AfterChildren.Add(Child);
            // The stock SCS cache may itself restore the mapped parent of a newly created root.
            if (bDeferredRoot && Actor->GetRootComponent() && Actor->GetRootComponent()->GetAttachParent() == MappedScene(Attachment.Parent))
                ConstructionParent = Actor->GetRootComponent()->GetAttachParent();
            if (Actor->GetOwner() != Mapped(ConstructionOwner) || Actor->GetInstigator() != Mapped(ConstructionInstigator)
                || (Actor->GetRootComponent() && (Actor->GetRootComponent()->GetAttachParent() != ConstructionParent
                    || Actor->GetRootComponent()->GetAttachSocketName() != (bDeferredRoot && ConstructionParent ? Attachment.Socket : ConstructionSocket)))
                || ConstructionChildren.Num() != AfterChildren.Num() || !ConstructionChildren.Includes(AfterChildren))
            { Error = TEXT("Actor construction changed mapped ownership or external attachments during candidate preparation; external callback effects are outside the rollback contract."); return false; }
            if (!Plan.NativeGraph.IsEmpty()
                && !BlueprintNativeGraphMatches(Actor, CastChecked<AActor>(Actor->GetClass()->GetDefaultObject())))
            { Error = TEXT("Actor construction changed admitted native component identities or attachments during candidate preparation; external callback effects are outside the rollback contract."); return false; }
            return true;
        };
        while (ConstructionIndex < RootPlans.Num()) if (!ConstructNext()) return false;
        if (bExpanded && (!ChildCreation.bSucceeded || !Pending.Instancing->Validate(Objects, Error))) return false;
        for (const auto& Plan : Pending.Actors)
        {
            auto* Candidate = Cast<AActor>(Objects.FindRef(Plan.Previous.Get()));
            if (!IsValid(Candidate) || Candidate->IsActorBeingDestroyed()
                || Candidate->GetOwner() != Mapped(Plan.Owner.Get()) || Candidate->GetInstigator() != Mapped(Plan.Instigator.Get()))
            { Error = TEXT("A candidate child changed its mapped ownership during construction; the previous generation is unchanged."); return false; }
        }
        for (const auto& Plan : Pending.Actors)
        {
            auto* Actor = CastChecked<AActor>(Objects.FindChecked(Plan.Previous.Get()));
            RecordOwnedChildren(Actor);
        }
        for (const auto& World : BeforeConstruction)
        {
            FString CurrentLevels;
            TSet<TWeakObjectPtr<AActor>> After = SnapshotWorldActors(World.Key, CurrentLevels).Difference(OwnedChildren);
            TSet<TWeakObjectPtr<AActor>> Before = World.Value.Difference(OwnedChildren);
            if (Before.Num() != After.Num() || !Before.Includes(After))
            {
                Error = TEXT("Actor construction changed World Actor membership during candidate preparation; external callback effects are outside the rollback contract.");
                Error += FString::Printf(TEXT(" LevelsBefore='%s' LevelsAfter='%s'."),
                    *BeforeConstructionLevels.FindChecked(World.Key), *CurrentLevels);
                for (const auto& Added : After.Difference(Before))
                    if (AActor* Actor = Added.Get())
                        Error += FString::Printf(TEXT(" Added='%s' parent='%s'."), *Actor->GetPathName(), *GetPathNameSafe(Actor->GetParentComponent()));
                for (const auto& Removed : Before.Difference(After))
                    if (AActor* Actor = Removed.Get())
                        Error += FString::Printf(TEXT(" Removed='%s' parent='%s'."), *Actor->GetPathName(), *GetPathNameSafe(Actor->GetParentComponent()));
                return false;
            }
        }
        if (!ApplyReferenceWrites()) return false;
        // SCS roots only exist after FinishSpawning; restore their saved external edges now.
        RestoreAttachments();
        for (const auto& State : Attachments)
        {
            AActor* Actor = CastChecked<AActor>(Mapped(State.Previous));
            TArray<AActor*> Children;
            Actor->GetAttachedActors(Children);
            TSet<AActor*> Expected, Actual;
            for (AActor* Child : State.Children) Expected.Add(CastChecked<AActor>(Mapped(Child)));
            for (AActor* Child : Children) if (!OwnedChild(Child, Actor)) Actual.Add(Child);
            if ((Actor->GetRootComponent() && (Actor->GetRootComponent()->GetAttachParent() != MappedScene(State.Parent)
                    || Actor->GetRootComponent()->GetAttachSocketName() != State.Socket))
                || Expected.Num() != Actual.Num() || !Expected.Includes(Actual))
            { Error = TEXT("Actor construction changed mapped external attachments during candidate preparation; external callback effects are outside the rollback contract."); return false; }
            RecordOwnedChildren(Actor);
        }
        return true;
    }
    return ApplyReferenceWrites();
}

void LockBlueprintReceiverMigration(FPendingBlueprintGeneration& Pending)
{
    Pending.Instancing.Reset();
    if (!Pending.Migration) return;
    auto& Transaction = *Pending.Migration;
    Transaction.bIrreversible = true;
    // No rollback is possible beyond this point. Release value snapshots while
    // their FProperty metadata and VM are alive, not during late static teardown.
    Transaction.Properties.Reset();
    Transaction.Actors.Reset();
    Transaction.Attachments.Reset();
    Pending.PublishedInstances = Transaction.NewInstances;
}

void AppendPreparedBlueprintInstances(const FPendingBlueprintGeneration& Pending, TMap<UObject*, UObject*>& Objects)
{
    if (Pending.Migration) Objects.Append(Pending.Migration->Objects);
}

bool CommitBlueprintReceiverMigration(FPendingBlueprintGeneration& Pending, FString& Error)
{
    check(Pending.Migration);
    auto& Transaction = *Pending.Migration;
    LockBlueprintReceiverMigration(Pending);
    for (const auto& Plan : Pending.Actors)
        if (!Pending.ChildActors.ContainsByPredicate([&](const auto& Child) { return Child.Previous.Get() == Plan.Previous.Get(); })
            && !Plan.World->DestroyActor(Plan.Previous.Get(), false, false))
        {
            Error = TEXT("Stock Actor retirement failed after the generation commit boundary; published replacements are retained and restart is required.");
            return false;
        }
    return true;
}

}

#else
#endif
