#include "Shared/SoloASScriptBlueprint.h"
#include "Runtime/SoloASBridgeObjectLifecycle.h"
#include "UObject/CoreNet.h"

#if WITH_EDITOR
#include "Blueprint/BlueprintSupport.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/AssetRegistryTagsContext.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"
#endif

#include "HAL/PlatformTLS.h"
#include "Misc/ScopeRWLock.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoloASScriptBlueprint, Log, All);

namespace
{
struct FScriptClassRegistry
{
    FRWLock Lock;
    TSet<FObjectKey> Classes;
    TSet<FObjectKey> PublishedClasses;
    const UClass* ScriptMetaClass = nullptr;
    SoloASBridge::FOnScriptClassRegistered Registered;
    SoloASBridge::FScriptReplicationListProvider ReplicationListProvider;
};

FScriptClassRegistry& GetScriptClassRegistry()
{
    static FScriptClassRegistry Registry;
    return Registry;
}

class FScriptPersistentFrameCollector final : public FReferenceCollector
{
public:
    FScriptPersistentFrameCollector(
        FReferenceCollector& InCollector,
        uint8* InFrame,
        UObject* InInstance
    )
        : InnerCollector(InCollector)
#if !(UE_BUILD_TEST || UE_BUILD_SHIPPING)
        , Frame(InFrame)
#endif
        , Instance(InInstance)
    {
    }

    virtual void HandleObjectReference(
        UObject*& Object,
        const UObject* Referencer,
        const FProperty* ReferencingProperty
    ) override
    {
        check(Referencer != nullptr);
#if !(UE_BUILD_TEST || UE_BUILD_SHIPPING)
        if (Object != nullptr && !Object->IsValidLowLevelFast())
        {
            UE_LOG(
                LogSoloASScriptBlueprint,
                Error,
                TEXT(
                    "Invalid object %p in persistent frame %p for %s; "
                    "the reference was cleared."
                ),
                Object,
                Frame,
                *GetNameSafe(Instance)
            );
            Object = nullptr;
            return;
        }
#endif
        if (Object == nullptr)
        {
            return;
        }

        if (!Object->HasAnyFlags(RF_StrongRefOnFrame))
        {
            if (CastField<const FObjectProperty>(ReferencingProperty) != nullptr
                && InnerCollector.MarkWeakObjectReferenceForClearing(
                    &Object,
                    Instance
                ))
            {
                return;
            }
        }

        InnerCollector.AddReferencedObject(
            ObjectPtrWrap(Object),
            Referencer,
            ReferencingProperty
        );
    }

    virtual void HandleObjectReferences(
        UObject** Objects,
        int32 Count,
        const UObject* Referencer,
        const FProperty* ReferencingProperty
    ) override
    {
        const FProperty* InnerProperty = ReferencingProperty;
        if (const FArrayProperty* ArrayProperty =
            CastField<const FArrayProperty>(ReferencingProperty))
        {
            InnerProperty = ArrayProperty->Inner;
        }
        for (int32 Index = 0; Index < Count; ++Index)
        {
            HandleObjectReference(
                Objects[Index],
                Referencer,
                InnerProperty
            );
        }
    }

    virtual bool IsIgnoringArchetypeRef() const override
    {
        return InnerCollector.IsIgnoringArchetypeRef();
    }

    virtual bool IsIgnoringTransient() const override
    {
        return InnerCollector.IsIgnoringTransient();
    }

private:
    FReferenceCollector& InnerCollector;
#if !(UE_BUILD_TEST || UE_BUILD_SHIPPING)
    const uint8* Frame;
#endif
    UObject* Instance;
};

bool CollectFrame(
    UBlueprintGeneratedClass* BlueprintClass,
    UObject* Instance,
    FReferenceCollector& Collector
)
{
    if (BlueprintClass->UberGraphFramePointerProperty == nullptr)
    {
        return true;
    }
    UFunction* UbergraphFunction = BlueprintClass->UberGraphFunction;
    if (UbergraphFunction == nullptr)
    {
        UE_LOG(
            LogSoloASScriptBlueprint,
            Error,
            TEXT("Persistent frame property on %s has no ubergraph function."),
            *BlueprintClass->GetPathName()
        );
        return false;
    }

    FPointerToUberGraphFrame* Frame = BlueprintClass
        ->UberGraphFramePointerProperty
        ->ContainerPtrToValuePtr<FPointerToUberGraphFrame>(Instance);
    if (Frame == nullptr || Frame->RawPointer == nullptr)
    {
        return true;
    }

    FScriptPersistentFrameCollector FrameCollector(
        Collector,
        Frame->RawPointer,
        Instance
    );
    Collector.SetIsProcessingNativeReferences(false);
    FrameCollector.AddPropertyReferences(
        UbergraphFunction,
        Frame->RawPointer,
        UbergraphFunction
    );
    Collector.SetIsProcessingNativeReferences(true);
    return true;
}

#if WITH_EDITOR
void OverrideScriptNativeParentTag(UClass* Parent, FAssetRegistryTagsContext Context)
{
    for (UClass* Ancestor = Parent; Ancestor; Ancestor = Ancestor->GetSuperClass())
    {
        if (SoloASBridge::IsRegisteredScriptClass(Ancestor))
        {
            Context.AddTag(UObject::FAssetRegistryTag(FBlueprintTags::NativeParentClassPath,
                FObjectPropertyBase::GetExportPath(Ancestor), UObject::FAssetRegistryTag::TT_Alphabetical));
            return;
        }
    }
}

bool IsSupportedTerminalCarrierLayout(const USoloASScriptBlueprint& Blueprint,
    const UClass& Carrier, const UClass& Parent)
{
    // UE owns Blueprint fields and persistent frames. The AS owner keeps only
    // its inherited storage; metadata must outlive all actual receivers.
    if (Carrier.GetPropertiesSize() < Parent.GetPropertiesSize()
        || Carrier.GetMinAlignment() < Parent.GetMinAlignment()) return false;
    TSet<const FProperty*> Seen;
    for (const FField* Member = Carrier.ChildProperties; Member; Member = Member->Next)
    {
        const FProperty* Field = CastField<FProperty>(Member);
        if (!Field || Seen.Contains(Field) || Field->GetOwnerStruct() != &Carrier
            || Field->GetOffset_ForInternal() < Parent.GetPropertiesSize()
            || Field->GetSize() <= 0
            || int64(Field->GetOffset_ForInternal()) + Field->GetSize() > Carrier.GetPropertiesSize())
            return false;
        Seen.Add(Field);
    }
    return true;
}

bool IsSupportedBlueprintCarrier(
    const USoloASScriptBlueprint& Blueprint,
    const UClass* Carrier,
    bool bRequireUnreachable,
    const FObjectKey* CapturedDefaultKey,
    TArray<FObjectKey>& Dependencies)
{
    if (Carrier == nullptr
        || Carrier->GetClass() != USoloASScriptBlueprintGeneratedClass::StaticClass()
        || Carrier->ClassGeneratedBy != &Blueprint
        || Carrier->GetOutermost() != GetTransientPackage()
        || (bRequireUnreachable && !Carrier->IsUnreachable())
        || Carrier->HasAnyFlags(RF_FinishDestroyed)
        || Carrier->bLayoutChanging
        || Carrier->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Hidden)
        || Carrier->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedInitialization))
    {
        return false;
    }

    const UClass* Parent = Carrier->GetSuperClass();
    if (Parent == nullptr
        || Parent->HasAnyFlags(RF_FinishDestroyed)
        || !SoloASBridge::IsRegisteredScriptClass(Parent)
        || Parent->bLayoutChanging
        || !IsSupportedTerminalCarrierLayout(Blueprint, *Carrier, *Parent))
    {
        return false;
    }

    // A sibling CDO may already have completed the first finish pass. Its
    // original serial is authoritative; never dereference its stale CDO slot.
    if (CapturedDefaultKey != nullptr
        && CapturedDefaultKey->ResolveObjectPtrEvenIfUnreachable() == nullptr)
    {
        Dependencies.Add(FObjectKey(Carrier));
        Dependencies.Add(*CapturedDefaultKey);
        return true;
    }

    // Never create a CDO during destruction or use a name to recover a role.
    const UObject* Default = Carrier->GetDefaultObject(false);
    if (Default == nullptr
        || (CapturedDefaultKey != nullptr
            && CapturedDefaultKey->ResolveObjectPtrEvenIfUnreachable() != Default)
        || Default->GetClass() != Carrier
        || !Default->HasAnyFlags(RF_ClassDefaultObject)
        || (bRequireUnreachable && !Default->IsUnreachable()))
    {
        return false;
    }
    Dependencies.Add(FObjectKey(Carrier));
    Dependencies.Add(CapturedDefaultKey != nullptr
        ? *CapturedDefaultKey : FObjectKey(Default));
    return true;
}

bool CaptureBlueprintFinishDependencies(
    const USoloASScriptBlueprint& Blueprint,
    bool bRequireUnreachable,
    TArray<FObjectKey>& Dependencies,
    const TArray<FObjectKey>* CapturedDependencies = nullptr)
{
    Dependencies.Reset();
    if (!IsInGameThread()
        || (bRequireUnreachable && !Blueprint.IsUnreachable())
        || Blueprint.HasAnyFlags(RF_FinishDestroyed)
        || Blueprint.GetClass() != USoloASScriptBlueprint::StaticClass()
        || Blueprint.GetOutermost() != GetTransientPackage()
        || Blueprint.HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedInitialization))
    {
        return false;
    }

    const UClass* Generated = Blueprint.GeneratedClass.Get();
    const UClass* Skeleton = Blueprint.SkeletonGeneratedClass.Get();
    if (CapturedDependencies != nullptr
        && (CapturedDependencies->Num() != (Skeleton != nullptr ? 4 : 2)
            || (*CapturedDependencies)[0].ResolveObjectPtrEvenIfUnreachable() != Generated
            || (Skeleton != nullptr
                && (*CapturedDependencies)[2].ResolveObjectPtrEvenIfUnreachable() != Skeleton)))
    {
        return false;
    }
    if (!IsSupportedBlueprintCarrier(Blueprint, Generated, bRequireUnreachable,
            CapturedDependencies != nullptr ? &(*CapturedDependencies)[1] : nullptr,
            Dependencies)
        || (Skeleton != nullptr
            && (Skeleton == Generated
                || !IsSupportedBlueprintCarrier(Blueprint, Skeleton, bRequireUnreachable,
                    CapturedDependencies != nullptr ? &(*CapturedDependencies)[3] : nullptr,
                    Dependencies))))
    {
        Dependencies.Reset();
        return false;
    }
    return true;
}

bool HaveBlueprintFinishDependenciesCompleted(const TArray<FObjectKey>& Dependencies)
{
    for (const FObjectKey& Key : Dependencies)
    {
        // A BeginDestroyed/FinishDestroyed flag alone does not prove that the
        // original identity's native destruction returned. Serial reset does.
        if (Key.ResolveObjectPtrEvenIfUnreachable() != nullptr)
        {
            return false;
        }
    }
    return true;
}
#endif

UClass* SkipRegisteredScriptClasses(UClass* Class)
{
    while (Class != nullptr && SoloASBridge::IsRegisteredScriptClass(Class))
    {
        Class = Class->GetSuperClass();
    }
    return Class;
}
}

#if WITH_EDITOR
void USoloASScriptBlueprint::GetInstanceActions(
    FBlueprintActionDatabaseRegistrar& ActionRegistrar
) const
{
    Super::GetInstanceActions(ActionRegistrar);
    if (GeneratedClass == nullptr
        || !ActionRegistrar.IsOpenForRegistration(GeneratedClass))
    {
        return;
    }

    for (const UClass* ScriptClass = ParentClass;
         ScriptClass != nullptr && SoloASBridge::IsRegisteredScriptClass(ScriptClass);
         ScriptClass = ScriptClass->GetSuperClass())
    {
        for (TFieldIterator<UFunction> FunctionIt(
                 ScriptClass, EFieldIteratorFlags::ExcludeSuper);
             FunctionIt; ++FunctionIt)
        {
            UFunction* Function = *FunctionIt;
            if (Function == nullptr
                || ScriptClass->FindFunctionByName(Function->GetFName()) != Function
                || !FBlueprintActionDatabase::IsFunctionAllowed(
                    Function, FBlueprintActionDatabase::EPermissionsContext::Node)
                || !UEdGraphSchema_K2::CanUserKismetCallFunction(Function))
            {
                continue;
            }

            if (UBlueprintFunctionNodeSpawner* Spawner =
                UBlueprintFunctionNodeSpawner::Create(Function))
            {
                ActionRegistrar.AddBlueprintAction(GeneratedClass, Spawner);
            }
        }
    }
}
#endif

#if WITH_EDITOR
void USoloASScriptBlueprint::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
    Super::GetAssetRegistryTags(Context);
    OverrideScriptNativeParentTag(ParentClass.Get(), Context);
}

void USoloASScriptBlueprintGeneratedClass::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
    Super::GetAssetRegistryTags(Context);
    OverrideScriptNativeParentTag(GetSuperClass(), Context);
}

bool USoloASScriptBlueprint::CanOrderTerminalMetadataFinish(bool bRequireUnreachable) const
{
    if (bRequireUnreachable && HasAnyFlags(RF_BeginDestroyed)
        && !bFinishDestroyGateArmed)
    {
        return false;
    }
    TArray<FObjectKey> Dependencies;
    return CaptureBlueprintFinishDependencies(*this, bRequireUnreachable, Dependencies,
        bFinishDestroyGateArmed ? &FinishDestroyDependencies : nullptr);
}

bool USoloASScriptBlueprintGeneratedClass::CanOrderTerminalMetadataFinish(bool bRequireUnreachable) const
{
    if (bRequireUnreachable && HasAnyFlags(RF_BeginDestroyed)
        && !bFinishDestroyGateArmed)
    {
        return false;
    }
    const auto* Blueprint = Cast<USoloASScriptBlueprint>(ClassGeneratedBy);
    return Blueprint != nullptr
        && (Blueprint->GeneratedClass.Get() == this
            || Blueprint->SkeletonGeneratedClass.Get() == this)
        && Blueprint->CanOrderTerminalMetadataFinish(bRequireUnreachable);
}

void USoloASScriptBlueprint::BeginDestroy()
{
    // Capture roles while every group member still has intact metadata.
    bFinishDestroyGateArmed = CaptureBlueprintFinishDependencies(
        *this, true, FinishDestroyDependencies);
    bFinishDestroyGateBlockedLogged = false;
    bFinishDestroyGateReadyLogged = false;
    if (bFinishDestroyGateArmed)
    {
        UE_LOG(LogSoloASScriptBlueprint, Log,
            TEXT("SOLOASBRIDGE_BP_FINISH_GATE phase=begin role=blueprint object=%s dependencies=%d"),
            *GetName(), FinishDestroyDependencies.Num());
    }
    Super::BeginDestroy();
}

bool USoloASScriptBlueprint::IsReadyForFinishDestroy()
{
    if (!bFinishDestroyGateArmed)
    {
        return Super::IsReadyForFinishDestroy();
    }
    if (!HaveBlueprintFinishDependenciesCompleted(FinishDestroyDependencies))
    {
        if (!bFinishDestroyGateBlockedLogged)
        {
            bFinishDestroyGateBlockedLogged = true;
            UE_LOG(LogSoloASScriptBlueprint, Log,
                TEXT("SOLOASBRIDGE_BP_FINISH_GATE phase=blocked role=blueprint object=%s dependencies=%d"),
                *GetName(), FinishDestroyDependencies.Num());
        }
        return false;
    }
    if (!Super::IsReadyForFinishDestroy())
    {
        return false;
    }
    if (!bFinishDestroyGateReadyLogged)
    {
        bFinishDestroyGateReadyLogged = true;
        UE_LOG(LogSoloASScriptBlueprint, Log,
            TEXT("SOLOASBRIDGE_BP_FINISH_GATE phase=ready role=blueprint object=%s dependencies_serial_invalid=1"),
            *GetName());
    }
    return true;
}

void USoloASScriptBlueprint::FinishDestroy()
{
    if (bFinishDestroyGateArmed)
    {
        check(HaveBlueprintFinishDependenciesCompleted(FinishDestroyDependencies));
        UE_LOG(LogSoloASScriptBlueprint, Log,
            TEXT("SOLOASBRIDGE_BP_FINISH_GATE phase=finish role=blueprint object=%s dependencies_serial_invalid=1"),
            *GetName());
    }
    Super::FinishDestroy();
}

void USoloASScriptBlueprintGeneratedClass::BeginDestroy()
{
    TArray<FObjectKey> GroupDependencies;
    // Exit purge may already have finished the asset. Resolve its saved serial
    // before reading any metadata through ClassGeneratedBy.
    const auto* Blueprint = Cast<USoloASScriptBlueprint>(
        FinishDestroyBlueprintKey.ResolveObjectPtrEvenIfUnreachable());
    bFinishDestroyGateArmed = Blueprint != nullptr
        && (Blueprint->GeneratedClass.Get() == this
            || Blueprint->SkeletonGeneratedClass.Get() == this)
        && CaptureBlueprintFinishDependencies(*Blueprint, true, GroupDependencies);
    bFinishDestroyGateBlockedLogged = false;
    bFinishDestroyGateReadyLogged = false;
    FinishDestroyDefaultKey = bFinishDestroyGateArmed
        ? FObjectKey(GetDefaultObject(false)) : FObjectKey();
    FinishDestroyReceiverDependencies.Reset();
    if (bFinishDestroyGateArmed)
    {
        for (int32 RoleIndex = 0; RoleIndex < GroupDependencies.Num(); RoleIndex += 2)
        {
            UClass* RoleClass = Cast<UClass>(GroupDependencies[RoleIndex].ResolveObjectPtrEvenIfUnreachable());
            if (!RoleClass) { bFinishDestroyGateArmed = false; break; }
            TArray<UObject*> Receivers;
            GetObjectsOfClass(RoleClass, Receivers, false, RF_NoFlags, EInternalObjectFlags::None);
            for (UObject* Receiver : Receivers)
            {
                if (!Receiver || Receiver->GetClass() != RoleClass || !Receiver->IsUnreachable())
                {
                    bFinishDestroyGateArmed = false;
                    break;
                }
                FinishDestroyReceiverDependencies.AddUnique(FObjectKey(Receiver));
            }
            FinishDestroyReceiverDependencies.AddUnique(GroupDependencies[RoleIndex + 1]);
            if (!bFinishDestroyGateArmed) break;
        }
    }
    if (bFinishDestroyGateArmed)
    {
        UE_LOG(LogSoloASScriptBlueprint, Log,
            TEXT("SOLOASBRIDGE_BP_FINISH_GATE phase=begin role=carrier object=%s current_cdo=1"),
            *GetName());
    }
    Super::BeginDestroy();
}

bool USoloASScriptBlueprintGeneratedClass::IsReadyForFinishDestroy()
{
    // Reinstanced carriers are no longer the asset's current generated class.
    // Their receivers can also be unhashed by BeginDestroy before enumeration.
    if (SoloASBridge::HasRuntimeReceiversPendingFinishDestroy(this)) return false;
    if (!bFinishDestroyGateArmed)
    {
        return Super::IsReadyForFinishDestroy();
    }
    if (!HaveBlueprintFinishDependenciesCompleted(FinishDestroyReceiverDependencies))
    {
        if (!bFinishDestroyGateBlockedLogged)
        {
            bFinishDestroyGateBlockedLogged = true;
            UE_LOG(LogSoloASScriptBlueprint, Log,
                TEXT("SOLOASBRIDGE_BP_FINISH_GATE phase=blocked role=carrier object=%s all_receiver_serials_invalid=0"),
                *GetName());
        }
        return false;
    }
    if (!Super::IsReadyForFinishDestroy())
    {
        return false;
    }
    if (!bFinishDestroyGateReadyLogged)
    {
        bFinishDestroyGateReadyLogged = true;
        UE_LOG(LogSoloASScriptBlueprint, Log,
            TEXT("SOLOASBRIDGE_BP_FINISH_GATE phase=ready role=carrier object=%s all_receiver_serials_invalid=1"),
            *GetName());
    }
    return true;
}

void USoloASScriptBlueprintGeneratedClass::FinishDestroy()
{
    if (bFinishDestroyGateArmed)
    {
        check(HaveBlueprintFinishDependenciesCompleted(FinishDestroyReceiverDependencies));
        UE_LOG(LogSoloASScriptBlueprint, Log,
            TEXT("SOLOASBRIDGE_BP_FINISH_GATE phase=finish role=carrier object=%s all_receiver_serials_invalid=1"),
            *GetName());
    }
    Super::FinishDestroy();
}

EBlueprintCompileOptions USoloASScriptBlueprint::GetDefaultCompileOptions() const
{
    EBlueprintCompileOptions Options = Super::GetDefaultCompileOptions();
    UClass* Generated = GeneratedClass.Get();
    TSet<UClass*> Visited;
    for (UClass* Ancestor = Generated != nullptr
        ? Generated->GetSuperClass()
        : nullptr;
        Ancestor != nullptr && !Visited.Contains(Ancestor);
        Ancestor = Ancestor->GetSuperClass())
    {
        Visited.Add(Ancestor);
        if (!Ancestor->HasAnyClassFlags(CLASS_NewerVersionExists)
            || !SoloASBridge::IsRegisteredScriptClass(Ancestor))
        {
            continue;
        }

        UClass* Authoritative = Ancestor->GetAuthoritativeClass();
        if (Authoritative != nullptr
            && Authoritative != Ancestor
            && !Authoritative->HasAnyClassFlags(CLASS_NewerVersionExists)
            && SoloASBridge::IsRegisteredScriptClass(Authoritative))
        {
            return Options
                | EBlueprintCompileOptions::UseDeltaSerializationDuringReinstancing
                | EBlueprintCompileOptions::SkipNewVariableDefaultsDetection;
        }
    }
    return Options;
}
#endif

void USoloASScriptBlueprintGeneratedClass::Bind()
{
    Super::Bind();
#if WITH_EDITOR
    FinishDestroyBlueprintKey = FObjectKey(ClassGeneratedBy);
#endif
    CppClassStaticFunctions.SetAddReferencedObjects(
        &USoloASScriptBlueprintGeneratedClass::AddReferencedObjectsInScriptUbergraphFrames);
}

void USoloASScriptBlueprintGeneratedClass::DestroyPersistentUberGraphFrame(UObject* Object, bool bSkipSuperClass) const
{
    UClass* Parent = GetSuperClass();
    const bool bFinishRetiredParent = Object
        && (Object->HasAnyFlags(RF_BeginDestroyed) || GExitPurge)
        && !bSkipSuperClass && Parent && !IsValid(Parent)
        && SoloASBridge::HasRegisteredScriptClassInAncestry(Parent);
    Super::DestroyPersistentUberGraphFrame(Object, bSkipSuperClass);
    if (bFinishRetiredParent)
    {
        // UE skips invalid parents, although their deferred FinishDestroy keeps
        // metadata live for this receiver. Route each skipped frame once.
        while (Parent && !IsValid(Parent))
        {
            checkf(!Parent->HasAnyFlags(RF_FinishDestroyed),
                TEXT("Script carrier metadata finished before its receiver"));
            Parent->DestroyPersistentUberGraphFrame(Object, true);
            Parent = Parent->GetSuperClass();
        }
        if (Parent) Parent->DestroyPersistentUberGraphFrame(Object);
    }
}

void USoloASScriptBlueprintGeneratedClass::
    AddReferencedObjectsInScriptUbergraphFrames(
        UObject* Instance,
        FReferenceCollector& Collector
    )
{
    if (Instance == nullptr)
    {
        return;
    }
    SoloASBridge::CollectRuntimeOwnerReferences(Instance, Collector);
    UBlueprintGeneratedClass* BlueprintClass =
        Cast<UBlueprintGeneratedClass>(Instance->GetClass());
    if (BlueprintClass == nullptr)
    {
        UE_LOG(
            LogSoloASScriptBlueprint,
            Error,
            TEXT("Script Blueprint collector received a non-Blueprint class."));
        return;
    }

    while (BlueprintClass != nullptr)
    {
        if (!CollectFrame(BlueprintClass, Instance, Collector))
        {
            return;
        }

        UClass* Boundary = SkipRegisteredScriptClasses(
            BlueprintClass->GetSuperClass()
        );
        if (Boundary == nullptr)
        {
            UE_LOG(
                LogSoloASScriptBlueprint,
                Fatal,
                TEXT(
                    "SOLOASBRIDGE_SCRIPT_BLUEPRINT_NO_NATIVE_BOUNDARY "
                    "class=%s"
                ),
                *BlueprintClass->GetPathName()
            );
            return;
        }
        if (Boundary->HasAllClassFlags(CLASS_Native))
        {
            Boundary->CallAddReferencedObjects(Instance, Collector);
            return;
        }
        UBlueprintGeneratedClass* NextBlueprintClass =
            Cast<UBlueprintGeneratedClass>(Boundary);
        if (NextBlueprintClass == nullptr)
        {
            UE_LOG(
                LogSoloASScriptBlueprint,
                Fatal,
                TEXT(
                    "SOLOASBRIDGE_SCRIPT_BLUEPRINT_UNKNOWN_NONNATIVE_BOUNDARY "
                    "class=%s boundary=%s"
                ),
                *BlueprintClass->GetPathName(),
                *Boundary->GetPathName()
            );
            return;
        }
        BlueprintClass = NextBlueprintClass;
    }
}

bool USoloASScriptBlueprintGeneratedClass::IsFunctionImplementedInScript(FName Name) const
{
    const UFunction* Function = FindFunctionByName(Name);
    const UClass* Owner = Function ? Cast<UClass>(Function->GetOuter()) : nullptr;
    return Owner && (SoloASBridge::IsRegisteredScriptClass(Owner)
        ? Owner->IsFunctionImplementedInScript(Name) : Super::IsFunctionImplementedInScript(Name));
}

void USoloASScriptBlueprintGeneratedClass::GetLifetimeBlueprintReplicationList(TArray<FLifetimeProperty>& Out) const
{
    Super::GetLifetimeBlueprintReplicationList(Out);
    // The stock Blueprint walk ends at a UClass script parent. Append its full script ancestry once.
    for (const UClass* Parent = GetSuperClass(); Parent; Parent = Parent->GetSuperClass())
    {
        if (!SoloASBridge::IsRegisteredScriptClass(Parent)) continue;
        const auto& Provider = GetScriptClassRegistry().ReplicationListProvider;
        TArray<FLifetimeProperty> ScriptEntries;
        if (!Provider || !Provider(Parent, ScriptEntries))
        {
            UE_LOG(LogSoloASScriptBlueprint, Error, TEXT("ASR-BLUEPRINT-REPLICATION-PROVIDER-INVALID: %s"), *GetPathName());
            return;
        }
        for (const FLifetimeProperty& Entry : ScriptEntries) Out.AddUnique(Entry);
        break;
    }
}

namespace SoloASBridge
{
void SetScriptReplicationListProvider(FScriptReplicationListProvider Provider)
{
    GetScriptClassRegistry().ReplicationListProvider = MoveTemp(Provider);
}

static bool RegisterScriptClassIdentity(
    UClass* ScriptClass,
    const UClass* ScriptMetaClass,
    bool bPublish
)
{
    if (ScriptClass == nullptr
        || ScriptMetaClass == nullptr
        || ScriptMetaClass == UClass::StaticClass()
        || !ScriptMetaClass->IsChildOf(UClass::StaticClass())
        || !ScriptMetaClass->HasAllClassFlags(CLASS_Native)
        || !static_cast<const UObject*>(ScriptClass)->IsA(ScriptMetaClass)
        || ScriptClass->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic))
    {
        return false;
    }

    if (ScriptClass->GetClass() != ScriptMetaClass
        || !ScriptClass->HasAllClassFlags(CLASS_CompiledFromBlueprint)
        || ScriptClass->IsAsset() || ScriptClass->GetLinker() != nullptr
        || !ScriptClass->GetOutermost()->HasAnyPackageFlags(PKG_CompiledIn)
        || ScriptClass->HasAnyFlags(RF_WasLoaded | RF_NeedLoad | RF_NeedPostLoad
            | RF_NeedPostLoadSubobjects | RF_NeedInitialization)) return false;
#if WITH_EDITORONLY_DATA
    if (ScriptClass->ClassGeneratedBy != nullptr) return false;
#endif

    FScriptClassRegistry& Registry = GetScriptClassRegistry();
    bool bAdded = false;
    {
        FWriteScopeLock ScopeLock(Registry.Lock);
        if (Registry.ScriptMetaClass == nullptr)
        {
            Registry.ScriptMetaClass = ScriptMetaClass;
        }
        else if (Registry.ScriptMetaClass != ScriptMetaClass)
        {
            return false;
        }
        const FObjectKey Key(ScriptClass);
        Registry.Classes.Add(Key);
        bAdded = bPublish && !Registry.PublishedClasses.Contains(Key);
        if (bPublish) Registry.PublishedClasses.Add(Key);
    }
    if (bAdded)
    {
        Registry.Registered.Broadcast(ScriptClass);
    }
    return true;
}

bool PrepareScriptClassRegistration(UClass* ScriptClass, const UClass* ScriptMetaClass)
{
    return RegisterScriptClassIdentity(ScriptClass, ScriptMetaClass, false);
}

bool RegisterScriptClass(UClass* ScriptClass, const UClass* ScriptMetaClass)
{
    return RegisterScriptClassIdentity(ScriptClass, ScriptMetaClass, true);
}

bool IsRegisteredScriptClass(const UClass* Class)
{
    if (Class == nullptr)
    {
        return false;
    }
    FScriptClassRegistry& Registry = GetScriptClassRegistry();
    FReadScopeLock ScopeLock(Registry.Lock);
    return Registry.Classes.Contains(FObjectKey(Class));
}

bool HasRegisteredScriptClassInAncestry(const UClass* Class)
{
    FScriptClassRegistry& Registry = GetScriptClassRegistry();
    FReadScopeLock ScopeLock(Registry.Lock);
    for (const UClass* Cursor = Class; Cursor != nullptr;
         Cursor = Cursor->GetSuperClass())
    {
        if (Registry.Classes.Contains(FObjectKey(Cursor)))
        {
            return true;
        }
    }
    return false;
}

void GetRegisteredScriptClasses(TArray<UClass*>& OutClasses)
{
    OutClasses.Reset();
    FScriptClassRegistry& Registry = GetScriptClassRegistry();
    {
        FReadScopeLock ScopeLock(Registry.Lock);
        OutClasses.Reserve(Registry.PublishedClasses.Num());
        for (const FObjectKey& Key : Registry.PublishedClasses)
        {
            if (UClass* Class = Cast<UClass>(Key.ResolveObjectPtr()))
            {
                OutClasses.Add(Class);
            }
        }
    }
    OutClasses.Sort([](const UClass& Left, const UClass& Right)
    {
        return Left.GetPathName() < Right.GetPathName();
    });
}

FOnScriptClassRegistered& OnScriptClassRegistered()
{
    return GetScriptClassRegistry().Registered;
}

namespace Private
{
void PruneScriptClasses()
{
    FScriptClassRegistry& Registry = GetScriptClassRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    for (auto Iterator = Registry.Classes.CreateIterator(); Iterator; ++Iterator)
    {
        if (Iterator->ResolveObjectPtr() == nullptr)
        {
            Iterator.RemoveCurrent();
        }
    }
}

void ClearScriptClasses()
{
    FScriptClassRegistry& Registry = GetScriptClassRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    Registry.Classes.Reset();
    Registry.ScriptMetaClass = nullptr;
    Registry.Registered.Clear();
}
}
}
