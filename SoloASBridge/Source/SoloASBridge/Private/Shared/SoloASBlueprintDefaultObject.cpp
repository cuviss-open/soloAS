#include "Shared/SoloASScriptBlueprint.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Serialization/AsyncLoadingEvents.h"
#include "UObject/ObjectSaveContext.h"
#include "Serialization/AsyncLoadingEvents.h"

#if WITH_EDITOR
bool USoloASScriptBlueprintGeneratedClass::AttachDefaultObjectScope(FSoloASBlueprintDefaultObjectScope* Scope)
{
    if (!IsInGameThread() || Scope == nullptr || DefaultObjectScope != nullptr)
        return false;
    DefaultObjectScope = Scope;
    return true;
}
void USoloASScriptBlueprintGeneratedClass::DetachDefaultObjectScope(FSoloASBlueprintDefaultObjectScope* Scope)
{
    if (DefaultObjectScope == Scope)
        DefaultObjectScope = nullptr;
}
#endif

// Validated against the 5.8.3 UClass/FObjectInitializer construction path.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8 && ENGINE_PATCH_VERSION == 3 \
    && USE_CIRCULAR_DEPENDENCY_LOAD_DEFERRING
#include "Shared/SoloASPrivateAccess.h"
#include "AutoRTFM.h"
#include "Misc/ConfigAccessTracking.h"
#include "Misc/ScopeLock.h"
#include "Misc/TransactionallySafeCriticalSection.h"
#include "UObject/ObjectInstancingGraph.h"
#include "UObject/OverridableManager.h"
#include "UObject/Package.h"
#include "UObject/PropertyStateTracking.h"
#include "UObject/SparseDelegate.h"
#include "UObject/UObjectThreadContext.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoloASBlueprintDefaultObject, Log, All);

namespace SoloASBridge::PrivateAccess
{
struct FLastObjectTag { using Type = UObject* FObjectInitializer::*; friend Type Resolve(FLastObjectTag) noexcept; };
struct FInitializeTag { using Type = bool FObjectInitializer::*; friend Type Resolve(FInitializeTag) noexcept; };
struct FDeferredTag { using Type = bool FObjectInitializer::*; friend Type Resolve(FDeferredTag) noexcept; };
struct FGraphTag
{ using Type = FObjectInstancingGraph* FObjectInitializer::*; friend Type Resolve(FGraphTag) noexcept; };
struct FCallbackTag
{ using Type = TFunction<void()> FObjectInitializer::*; friend Type Resolve(FCallbackTag) noexcept; };
struct FPostCallbacksTag
{ using Type = TArray<TFunction<void()>> FObjectInitializer::*; friend Type Resolve(FPostCallbacksTag) noexcept; };
struct FInitPropertyTag
{ using Type = bool (*)(FProperty*, UObject*); friend Type Resolve(FInitPropertyTag) noexcept; };
struct FBlueprintSerializeAndPostLoadCriticalTag
{
    using Type = FTransactionallySafeCriticalSection UBlueprintGeneratedClass::*;
    friend Type Resolve(FBlueprintSerializeAndPostLoadCriticalTag) noexcept;
};
template struct TMember<FLastObjectTag, &FObjectInitializer::LastConstructedObject>;
template struct TMember<FInitializeTag, &FObjectInitializer::bShouldInitializePropsFromArchetype>;
template struct TMember<FDeferredTag, &FObjectInitializer::bIsDeferredInitializer>;
template struct TMember<FGraphTag, &FObjectInitializer::InstanceGraph>;
template struct TMember<FCallbackTag, &FObjectInitializer::PropertyInitCallback>;
template struct TMember<FPostCallbacksTag, &FObjectInitializer::PropertyPostInitCallbacks>;
template struct TMember<FInitPropertyTag, &FObjectInitializer::InitNonNativeProperty>;
template struct TMember<FBlueprintSerializeAndPostLoadCriticalTag,
    &UBlueprintGeneratedClass::SerializeAndPostLoadCritical>;
#if WITH_EDITORONLY_DATA
struct FGCHandleTag { using Type = FDelegateHandle FObjectInitializer::*; friend Type Resolve(FGCHandleTag) noexcept; };
struct FGCObjectsTag
{ using Type = TArray<FGCObject*> FObjectInitializer::*; friend Type Resolve(FGCObjectsTag) noexcept; };
template struct TMember<FGCHandleTag, &FObjectInitializer::OnGCObjectCreatedHandle>;
template struct TMember<FGCObjectsTag, &FObjectInitializer::CreatedGCObjects>;
#endif
}

namespace
{
class FBlueprintSerializeAndPostLoadScope final
{
public:
    explicit FBlueprintSerializeAndPostLoadScope(UBlueprintGeneratedClass& Class)
        : Lock(SoloASBridge::PrivateAccess::Get<
            SoloASBridge::PrivateAccess::FBlueprintSerializeAndPostLoadCriticalTag>(Class))
    {
    }

private:
    UE::TScopeLock<FTransactionallySafeCriticalSection> Lock;
};

FName NativePropertyOverrideId(const FProperty& Property)
{
    const UClass* OwnerClass = Property.GetOwnerClass();
    return OwnerClass != nullptr
        ? FName(*(OwnerClass->GetPathName() + TEXT(":") + Property.GetName()))
        : NAME_None;
}

bool IsNativeInheritedDefaultProperty(const FProperty& Property, const UClass& Parent)
{
    const UClass* OwnerClass = Property.GetOwnerClass();
    const bool bConfig = Property.HasAnyPropertyFlags(CPF_Config)
        && OwnerClass != nullptr
        && !OwnerClass->HasAnyClassFlags(CLASS_PerObjectConfig);
    const bool bTransient = Property.HasAnyPropertyFlags(
        CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient);
    return OwnerClass != nullptr
        && OwnerClass->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic)
        && !bConfig
        && (!bTransient || !Property.ContainsInstancedObjectProperty())
        && Property.IsInContainer(&Parent);
}

void RequireConstructionState(bool bValid)
{
    if (!bValid)
    {
        UE_LOG(LogSoloASBlueprintDefaultObject, Fatal, TEXT("SOLOASBRIDGE_BP_CDO_INITIALIZER_STATE_INVALID"));
    }
}

// The binding layer never exposes initializer fields to the factory or Runtime.
struct FInitializerAccess final
{
    static UObject* PreviousObject(const FObjectInitializer& Initializer)
    { return SoloASBridge::PrivateAccess::Get<SoloASBridge::PrivateAccess::FLastObjectTag>(Initializer); }
    static bool HasOwnedState(const FObjectInitializer& Initializer)
    {
        using namespace SoloASBridge::PrivateAccess;
        bool bValid = !Get<FInitializeTag>(Initializer) && !Get<FDeferredTag>(Initializer)
            && Get<FGraphTag>(Initializer) == nullptr;
#if WITH_EDITORONLY_DATA
        bValid &= !Get<FGCHandleTag>(Initializer).IsValid() && Get<FGCObjectsTag>(Initializer).IsEmpty();
#endif
        return bValid;
    }
    static bool InitNonNativeProperty(FProperty* Property, UObject* Object)
    { return SoloASBridge::PrivateAccess::CallStatic<SoloASBridge::PrivateAccess::FInitPropertyTag>(Property, Object); }
    static void PostConstruct(FObjectInitializer& Initializer)
    { FScriptIntegrationObjectHelper::PostConstructInitObject(Initializer); }
    static void InstanceSubobjects(const FObjectInitializer& Initializer, UClass* Class, bool bRoot, bool bSubobjects)
    {
        FScriptIntegrationObjectHelper::InstanceSubobjects(Initializer, Class, bRoot, bSubobjects);
    }
    static void MarkConsumed(FObjectInitializer& Initializer)
    { SoloASBridge::PrivateAccess::Get<SoloASBridge::PrivateAccess::FDeferredTag>(Initializer) = true; }

    static void InstallRootInstancing(
        FObjectInitializer& Initializer, UClass& Class, FObjectInstancingGraph& OwnedGraph)
    {
        if (!Class.HasAnyClassFlags(CLASS_HasInstancedReference))
            return;
        RequireConstructionState(HasOwnedState(Initializer) && !OwnedGraph.HasDestinationRoot());
        TFunction<void()> Original = MoveTemp(Callback(Initializer));
        Callback(Initializer) = [&Initializer, &Class, &OwnedGraph, Original = MoveTemp(Original)]() mutable
        {
            if (Original)
                Original();
            RequireConstructionState(Graph(Initializer) == nullptr
                && Initializer.GetObj()->HasAnyFlags(RF_ClassDefaultObject)
                && !Initializer.GetObj()->HasAnyFlags(RF_NeedLoad | RF_ImmutableDefaultObject));
            FObjectInstancingGraph* OriginalGraph = Graph(Initializer);
            Graph(Initializer) = &OwnedGraph;
            // Keep this graph through the stock DSO phase. Restore before user
            // post-init callbacks and Class::PostInitInstance see the initializer.
            TFunction<void()> RestoreGraph = [&Initializer, OriginalGraph]() { Graph(Initializer) = OriginalGraph; };
            PostCallbacks(Initializer).Insert(MoveTemp(RestoreGraph), 0);
            InstanceSubobjects(Initializer, &Class, true, false);
        };
    }

private:
    static FObjectInstancingGraph*& Graph(FObjectInitializer& Initializer)
    { return SoloASBridge::PrivateAccess::Get<SoloASBridge::PrivateAccess::FGraphTag>(Initializer); }
    static TFunction<void()>& Callback(FObjectInitializer& Initializer)
    { return SoloASBridge::PrivateAccess::Get<SoloASBridge::PrivateAccess::FCallbackTag>(Initializer); }
    static TArray<TFunction<void()>>& PostCallbacks(FObjectInitializer& Initializer)
    { return SoloASBridge::PrivateAccess::Get<SoloASBridge::PrivateAccess::FPostCallbacksTag>(Initializer); }
};

void InitializeFromScriptParent(UClass& Class, UObject* Object, UClass* Parent, UObject* ParentDefault)
{
    if (FOverridableManager* Manager = FOverridableManager::TryGet())
        Manager->InheritEnabledFrom(Object, ParentDefault);
#if WITH_EDITORONLY_DATA
    UE::FInitializedPropertyValueState InitializedState(Object);
    UE::FInitializedPropertyValueState ParentState(ParentDefault);
#endif
    // Match ordinary Blueprint CDO inheritance: construction establishes live
    // storage, then every inherited parent field receives the parent CDO value.
    // Blueprint-owned fields have no storage in the parent and are skipped.
    bool bNeedInitialize = true;
    for (FProperty* Property = Class.PropertyLink; Property;
        Property = Property->PropertyLinkNext)
    {
        if (bNeedInitialize)
            bNeedInitialize = FInitializerAccess::InitNonNativeProperty(Property, Object);
        const bool bTransient = Property->HasAnyPropertyFlags(
            CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient);
        if ((!bTransient || !Property->ContainsInstancedObjectProperty())
            && Property->IsInContainer(Parent))
        {
            Property->CopyCompleteValue_InContainer(Object, ParentDefault);
#if WITH_EDITORONLY_DATA
            if (InitializedState.IsTracking())
            {
                for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
                {
                    const bool bInitialized = !Property->HasAllPropertyFlags(CPF_RequiredParm)
                        || (ParentState.IsTracking() && ParentState.IsSet(Property, Index));
                    InitializedState.SetTo(bInitialized, Property, Index);
                }
            }
#endif
        }
    }
}

void ConstructScriptParentDefault(UClass& Class, UObject* Object, UClass* Parent, UObject* ParentDefault)
{
    FUObjectThreadContext& Context = FUObjectThreadContext::Get();
    const int32 OuterConstructorDepth = Context.IsInConstructor;
    UObject* OuterObject = Context.ConstructedObject;
    const FObjectInitializer* OuterInitializer = Context.TopInitializer();
    FObjectInstancingGraph OwnedGraph;
    FObjectInitializer Initializer(Object, ParentDefault, EObjectInitializerOptions::None);
    Class.ClassConstructor(Initializer);
    RequireConstructionState(!AutoRTFM::IsTransactional() && FInitializerAccess::HasOwnedState(Initializer)
        && Context.TopInitializer() == &Initializer && Context.IsInConstructor == OuterConstructorDepth + 1
        && Context.ConstructedObject == Object && FInitializerAccess::PreviousObject(Initializer) == OuterObject
        && Initializer.GetArchetype() == ParentDefault);

    // Match the engine destructor's native-construction boundary before any copy.
    AutoRTFM::PopOnAbortHandler(&Context.IsInConstructor);
    UE_AUTORTFM_OPEN
    {
        --Context.IsInConstructor;
        Context.ConstructedObject = OuterObject;
    };
    Object->ClearInternalFlags(EInternalObjectFlags::PendingConstruction);
    {
        UE::ConfigAccessTracking::FIgnoreScope IgnoreConfigTracking;
        InitializeFromScriptParent(Class, Object, Parent, ParentDefault);
        FInitializerAccess::InstallRootInstancing(Initializer, Class, OwnedGraph);
        FInitializerAccess::PostConstruct(Initializer);
    }
    RequireConstructionState(Initializer.GetObj() == nullptr && !Object->HasAnyFlags(RF_NeedInitialization)
        && Context.TopInitializer() == &Initializer && Context.IsInConstructor == OuterConstructorDepth
        && Context.ConstructedObject == OuterObject);
    // Consume both abort registrations, not just the visible TLS stack entry.
    AutoRTFM::PopOnAbortHandler(&Initializer);
    UE_AUTORTFM_OPEN
    {
        Context.PopInitializer();
    };
    RequireConstructionState(Context.TopInitializer() == OuterInitializer);
    FInitializerAccess::MarkConsumed(Initializer);
}
}
#endif

UObject* USoloASScriptBlueprintGeneratedClass::CreateDefaultObject()
{
    if (UObject* Existing = GetDefaultObject(false))
        return Existing;
    const auto PublishConstructed = [this](UObject* Object)
    {
#if WITH_EDITOR
        if (Object != nullptr && Object == GetDefaultObject(false) && DefaultObjectScope != nullptr)
        {
            DefaultObjectScope->OnDefaultCreated(this, Object);
            // A tool callback can compile again and replace the just-created default.
            Object = GetDefaultObject(false);
        }
#endif
        return Object;
    };
    const auto ConstructWithSuper = [this, &PublishConstructed]()
    {
        // Parent construction can reenter this class before the fallback.
        if (UObject* Existing = GetDefaultObject(false))
            return Existing;
        return PublishConstructed(Super::CreateDefaultObject());
    };
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8 && ENGINE_PATCH_VERSION == 3 \
    && USE_CIRCULAR_DEPENDENCY_LOAD_DEFERRING
    UClass* Parent = GetSuperClass();
    if (HasAnyFlags(RF_ClassDefaultObject | RF_NeedLoad | RF_NeedPostLoad | RF_NeedInitialization)
        || !IsInGameThread() || AutoRTFM::IsTransactional() || GIsDuplicatingClassForReinstancing
        || bLayoutChanging
        || (GetLinker() != nullptr && HasAnyFlags(RF_WasLoaded))
        || ClassConstructor == nullptr
        || !HasAnyClassFlags(CLASS_CompiledFromBlueprint)
        || HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic | CLASS_NewerVersionExists)
        || !SoloASBridge::IsRegisteredScriptClass(Parent) || Parent->GetLinker() != nullptr
        || Parent->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedInitialization))
        return ConstructWithSuper();
    UObjectForceRegistration(Parent);
    UObject* ParentDefault = Parent->GetDefaultObject();
    if (ParentDefault == nullptr || ParentDefault->HasAnyFlags(RF_NeedLoad | RF_NeedInitialization))
        return ConstructWithSuper();
    if (UObject* ReentrantDefault = GetDefaultObject(false))
        return ReentrantDefault;

    UObject* Object = StaticAllocateObject(this, GetOuter(), NAME_None,
        RF_Public | RF_ClassDefaultObject | RF_ArchetypeObject);
    std::atomic_thread_fence(std::memory_order_release);
    TSAN_BEFORE(Object);
    SetDefaultObject(Object);
    for (TFieldIterator<FMulticastSparseDelegateProperty> It(this,
        EFieldIteratorFlags::ExcludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
    {
        const FSparseDelegate& Delegate = It->GetPropertyValue_InContainer(Object);
        USparseDelegateFunction* Function = CastChecked<USparseDelegateFunction>(It->SignatureFunction);
        FSparseDelegateStorage::RegisterDelegateOffset(Object, Function->DelegateName,
            reinterpret_cast<const uint8*>(&Delegate) - reinterpret_cast<const uint8*>(Object));
    }
    ConstructScriptParentDefault(*this, Object, Parent, ParentDefault);
    if (GetOutermost()->HasAnyPackageFlags(PKG_CompiledIn)
        && !GetOutermost()->HasAnyPackageFlags(PKG_RuntimeGenerated))
    {
        NotifyRegistrationEvent(GetOutermost()->GetFName(), GetDefaultObjectName(),
            ENotifyRegistrationType::NRT_ClassCDO, ENotifyRegistrationPhase::NRP_Finished, nullptr, false, Object);
    }
    Object->PostCDOContruct();
    return PublishConstructed(Object);
#else
    return ConstructWithSuper();
#endif
}

void USoloASScriptBlueprintGeneratedClass::PostLoadDefaultObject(UObject* Object)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8 && ENGINE_PATCH_VERSION == 3 \
    && USE_CIRCULAR_DEPENDENCY_LOAD_DEFERRING
    FBlueprintSerializeAndPostLoadScope SerializeAndPostLoadScope(*this);
#endif
    Super::PostLoadDefaultObject(Object);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8 && ENGINE_PATCH_VERSION == 3 \
    && USE_CIRCULAR_DEPENDENCY_LOAD_DEFERRING
    UClass* Parent = GetSuperClass();
    UObject* ParentDefault = Parent != nullptr ? Parent->GetDefaultObject(false) : nullptr;
    if (Object == nullptr || Object != GetDefaultObject(false)
        || !bNativePropertyOverrideIdsValid || ParentDefault == nullptr
        || !SoloASBridge::IsRegisteredScriptClass(Parent)
        || Parent->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedInitialization))
    {
        return;
    }

    for (FProperty* Property = Parent->PropertyLink; Property;
        Property = Property->PropertyLinkNext)
    {
        if (IsNativeInheritedDefaultProperty(*Property, *Parent)
            && !NativePropertyOverrideIds.Contains(NativePropertyOverrideId(*Property)))
        {
            Property->CopyCompleteValue_InContainer(Object, ParentDefault);
        }
    }
    UpdateCustomPropertyListForPostConstruction();
#endif
}

#if WITH_EDITOR
void USoloASScriptBlueprintGeneratedClass::PreSave(FObjectPreSaveContext SaveContext)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8 && ENGINE_PATCH_VERSION == 3 \
    && USE_CIRCULAR_DEPENDENCY_LOAD_DEFERRING
    FBlueprintSerializeAndPostLoadScope SerializeAndPostLoadScope(*this);
#endif
    Super::PreSave(SaveContext);
    NativePropertyOverrideIds.Reset();
    bNativePropertyOverrideIdsValid = false;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8 && ENGINE_PATCH_VERSION == 3 \
    && USE_CIRCULAR_DEPENDENCY_LOAD_DEFERRING
    UClass* Parent = GetSuperClass();
    UObject* BlueprintDefault = GetDefaultObject(false);
    UObject* ParentDefault = Parent != nullptr ? Parent->GetDefaultObject(false) : nullptr;
    if (BlueprintDefault == nullptr || ParentDefault == nullptr
        || !SoloASBridge::IsRegisteredScriptClass(Parent))
    {
        return;
    }

    for (FProperty* Property = Parent->PropertyLink; Property;
        Property = Property->PropertyLinkNext)
    {
        if (IsNativeInheritedDefaultProperty(*Property, *Parent)
            && !Property->Identical_InContainer(BlueprintDefault, ParentDefault))
        {
            NativePropertyOverrideIds.Add(NativePropertyOverrideId(*Property));
        }
    }
    NativePropertyOverrideIds.Sort([](FName Left, FName Right)
    {
        return Left.LexicalLess(Right);
    });
    bNativePropertyOverrideIdsValid = true;
#endif
}
#endif

void USoloASScriptBlueprintGeneratedClass::InitPropertiesFromCustomList(
    uint8* DataPtr,
    const uint8* DefaultDataPtr
)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8 && ENGINE_PATCH_VERSION == 3 \
    && USE_CIRCULAR_DEPENDENCY_LOAD_DEFERRING
    FBlueprintSerializeAndPostLoadScope SerializeAndPostLoadScope(*this);
#endif
    // Blueprint-owned values were handled by the ordinary post-construction
    // loop. The stock BPGC custom list uses the first native ancestor and
    // would replay script-parent values that Haze intentionally leaves to the
    // inherited script defaults. Bridge owns the exact native override list.
    UClass::InitPropertiesFromCustomList(DataPtr, DefaultDataPtr);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8 && ENGINE_PATCH_VERSION == 3 \
    && USE_CIRCULAR_DEPENDENCY_LOAD_DEFERRING
    UClass* Parent = GetSuperClass();
    UObject* BlueprintDefault = GetDefaultObject(false);
    UObject* ParentDefault = Parent != nullptr ? Parent->GetDefaultObject(false) : nullptr;
    if (DataPtr == nullptr || DefaultDataPtr == nullptr
        || BlueprintDefault == nullptr || ParentDefault == nullptr
        || DefaultDataPtr != reinterpret_cast<const uint8*>(BlueprintDefault)
        || !SoloASBridge::IsRegisteredScriptClass(Parent))
    {
        return;
    }

    // Haze builds their native custom list against the script parent CDO. A
    // native field authored on this Blueprint CDO after that list was built
    // must still override the script constructor/defaults on new instances.
    for (FProperty* Property = Parent->PropertyLink; Property;
        Property = Property->PropertyLinkNext)
    {
        if (!IsNativeInheritedDefaultProperty(*Property, *Parent)
            || (bNativePropertyOverrideIdsValid
                && !NativePropertyOverrideIds.Contains(NativePropertyOverrideId(*Property))))
        {
            continue;
        }

        const void* BlueprintValue = Property->ContainerPtrToValuePtr<void>(
            BlueprintDefault);
        const void* ParentValue = Property->ContainerPtrToValuePtr<void>(
            ParentDefault);
        if (!Property->Identical(BlueprintValue, ParentValue))
        {
            Property->CopyCompleteValue(
                Property->ContainerPtrToValuePtr<void>(DataPtr),
                BlueprintValue);
        }
    }
#endif
}
