#include "Runtime/SoloASBridgeCallableDispatch.h"
#include "Compat/SoloASRuntimeCompat.h"
#include "Runtime/SoloASBridgeStaticCallContext.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "Runtime/SoloASBridgeObjectLifecycle.h"
#include "Runtime/SoloASBridgeCallableDispatchSupport.h"
#include "Runtime/SoloASBridgeScriptMetadata.h"

#include "HAL/ThreadingBase.h"
#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/ScopeExit.h"
#include "Misc/CoreDelegates.h"
#include "UObject/GCObject.h"
#include "UObject/StructOnScope.h"
#include "Templates/AlignmentTemplates.h"
#include "Templates/Atomic.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/Stack.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectThreadContext.h"

// Installed modular 5.6-5.8 Engines omit this defaulted constructor from their exports.
// Keep the exact C++ copy construction used by the fork's typed return caller.
// The module rules reject an SDK that exports it, so this definition never replaces an engine export.
                                                                                                                                           
#if !IS_MONOLITHIC && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
#include "Kismet/ImportanceSamplingLibrary.h"
#if PLATFORM_WINDOWS
// Export from the linker without changing the engine header's C++ DLL linkage.
#pragma comment(linker, "/EXPORT:??0FImportanceTexture@@QEAA@AEBU0@@Z")
#endif
// CS.Bridge.CallableDispatch.ImportanceCopyMacExport: keep the exact defaulted copy
// visible to modular editor callers without changing the installed Engine.
#if PLATFORM_MAC && WITH_EDITOR
__attribute__((visibility("default")))
#endif
FImportanceTexture::FImportanceTexture(const FImportanceTexture& Other) = default;
#endif
// SOLOAS-COMPAT-END

namespace
{
class FReflectedReturnCopies final : public FGCObject
{
public:
    TArray<TUniquePtr<FStructOnScope>> Frames;
    void BeforeCall(bool bWithoutEndFrame)
    {
        if (bWithoutEndFrame) Frames.Reset();
    }
    virtual void AddReferencedObjects(FReferenceCollector& Collector) override
    {
        for (const TUniquePtr<FStructOnScope>& Frame : Frames)
        {
            UStruct* Function = const_cast<UStruct*>(Frame->GetStructPtr().GetEvenIfUnreachable());
            Collector.AddReferencedObject(Function);
            Collector.AddPropertyReferences(Function, Frame->GetStructMemory());
        }
    }
    virtual FString GetReferencerName() const override { return TEXT("SoloASBridgeReflectedReturnCopies"); }
};
TUniquePtr<FReflectedReturnCopies> ReflectedReturnCopies;
FDelegateHandle ReflectedReturnEndFrameHandle;

thread_local UObject* ScriptFinishDestroyReceiver = nullptr;
thread_local UObject* ScriptDefaultConstructionObject = nullptr;
TWeakObjectPtr<USoloASBridgeStaticCallContext> StaticCallContext;
SoloASBridge::FPendingScriptFinishDestroy PendingScriptObjects = nullptr;

struct FCallableParameterBuffer
{
    explicit FCallableParameterBuffer(UFunction& InFunction, bool bInBorrowReferences = false)
        : Function(InFunction)
        , Data(
              FMemory::Malloc(
                  FMath::Max<int32>(1, Function.ParmsSize),
                  FMath::Max<uint32>(1U, Function.GetMinAlignment())
              )
          )
        , bBorrowReferences(bInBorrowReferences)
    {
        FMemory::Memzero(Data, FMath::Max<int32>(1, Function.ParmsSize));
        for (
            TFieldIterator<FProperty> It(
                &Function,
                EFieldIteratorFlags::ExcludeSuper
            );
            It;
            ++It
        )
        {
            FProperty* Property = *It;
            if (Property->HasAnyPropertyFlags(CPF_Parm)
                && !(bBorrowReferences && !Property->HasAnyPropertyFlags(CPF_ReturnParm)
                    && Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm)))
            {
                Property->InitializeValue_InContainer(Data);
            }
        }
    }

    ~FCallableParameterBuffer()
    {
        for (
            TFieldIterator<FProperty> It(
                &Function,
                EFieldIteratorFlags::ExcludeSuper
            );
            It;
            ++It
        )
        {
            FProperty* Property = *It;
            if (Property->HasAnyPropertyFlags(CPF_Parm)
                && !(bBorrowReferences && !Property->HasAnyPropertyFlags(CPF_ReturnParm)
                    && Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm)))
            {
                Property->DestroyValue_InContainer(Data);
            }
        }
        FMemory::Free(Data);
    }

    FCallableParameterBuffer(const FCallableParameterBuffer&) = delete;
    FCallableParameterBuffer& operator=(const FCallableParameterBuffer&) =
        delete;

    UFunction& Function;
    void* Data;
    bool bBorrowReferences;
};
}

SoloASBridge::FScopedScriptDefaultConstruction::FScopedScriptDefaultConstruction(UObject* Object)
{
    const FObjectInitializer* Initializer = FUObjectThreadContext::Get().TopInitializer();
    bActive = Object && !GExitPurge
        && !Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) && !Object->IsUnreachable()
        && (Object->HasAnyFlags(RF_NeedInitialization) || (Initializer && Initializer->GetObj() == Object));
    Previous = ScriptDefaultConstructionObject;
    ScriptDefaultConstructionObject = bActive ? Object : nullptr;
}

SoloASBridge::FScopedScriptDefaultConstruction::~FScopedScriptDefaultConstruction()
{
    ScriptDefaultConstructionObject = Previous;
}

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
struct FSoloASBridgeTerminalStaticNativeProofForTest
{
    TWeakObjectPtr<UFunction> Function;
    TWeakObjectPtr<UClass> Owner;
    FNativeFuncPtr Entry = nullptr;
    FString Layout;
    TArray<const FProperty*> Properties;
    uint64 BindEpoch = 0;
    uint32 Alignment = 0;
    mutable TAtomic<bool> bRetired{false};
};

namespace
{
FString DescribeTerminalStaticLayout(const UFunction& Function)
{
    FString Layout = FString::Printf(TEXT("%u:%d:%d:%d:%d"), uint32(Function.FunctionFlags),
        int32(Function.ParmsSize), Function.GetStructureSize(), int32(Function.ReturnValueOffset), int32(Function.NumParms));
    for (TFieldIterator<FProperty> It(&Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        const FProperty& Property = **It;
        Layout += FString::Printf(TEXT("|%s:%s:%s:%llu:%d:%d:%d"), *Property.GetName(),
            *Property.GetClass()->GetName(), *Property.GetCPPType(), uint64(Property.GetPropertyFlags()),
            Property.GetOffset_ForInternal(), Property.GetSize(), Property.ArrayDim);
    }
    return Layout;
}

bool HasTerminalStaticIdentity(const UFunction* Function, const UClass* Owner)
{
    return Function && Owner && !Function->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
        && !Owner->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
        && Function->HasAllFunctionFlags(FUNC_Native | FUNC_Static)
        && !Function->HasAnyFunctionFlags(FUNC_Event | FUNC_BlueprintEvent | FUNC_Net | FUNC_Delegate | FUNC_MulticastDelegate)
        && Function->Script.IsEmpty() && Function->GetNativeFunc() != nullptr
        && Owner->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic) && !Owner->HasAnyClassFlags(CLASS_Interface)
        && Function->GetOuterUClass() == Owner && Owner->FindFunctionByName(Function->GetFName()) == Function
        && Function->GetStructureSize() >= Function->ParmsSize && SoloASBridge::Private::HasSupportedSignature(*Function);
}

UFunction* ValidateTerminalStaticProof(const FSoloASBridgeCallableDispatchRequest& Request, UClass*& Owner)
{
    const auto& Proof = *Request.TerminalStaticProof;
    if (!SoloASBridge::IsTerminalRuntimeDestroyCallbackForTest() || Proof.bRetired.Load()
        || Request.Target != nullptr || Proof.BindEpoch == 0 || Proof.BindEpoch != Request.TerminalStaticBindEpoch) return nullptr;
    UFunction* Function = Proof.Function.GetEvenIfUnreachable();
    Owner = Proof.Owner.GetEvenIfUnreachable();
    if (!HasTerminalStaticIdentity(Function, Owner) || Function != Request.DeclaredFunction
        || (!Request.FunctionName.IsNone() && Request.FunctionName != Function->GetFName())
        || (Request.OwnerPath && Owner->GetPathName() != Request.OwnerPath)
        || Function->GetNativeFunc() != Proof.Entry || DescribeTerminalStaticLayout(*Function) != Proof.Layout
        || Function->GetMinAlignment() != Proof.Alignment) return nullptr;
    int32 Index = 0;
    for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        if (Index == Proof.Properties.Num() || Proof.Properties[Index++] != *It) return nullptr;
    }
    if (Index != Proof.Properties.Num()) return nullptr;
    return Function;
}
}

TSharedPtr<const FSoloASBridgeTerminalStaticNativeProofForTest, ESPMode::ThreadSafe>
SoloASBridge::CreateTerminalStaticNativeProofForTest(const TWeakObjectPtr<UFunction>& FunctionKey,
    const TWeakObjectPtr<UClass>& OwnerKey, uint64 BindEpoch)
{
    if (!IsInGameThread() || GExitPurge || !BindEpoch) return nullptr;
    UFunction* Function = FunctionKey.Get();
    UClass* Owner = OwnerKey.Get();
    if (!HasTerminalStaticIdentity(Function, Owner) || Function->GetMinAlignment() <= 0
        || !FMath::IsPowerOfTwo(uint32(Function->GetMinAlignment()))) return nullptr;
    auto Proof = MakeShared<FSoloASBridgeTerminalStaticNativeProofForTest, ESPMode::ThreadSafe>();
    Proof->Function = FunctionKey;
    Proof->Owner = OwnerKey;
    Proof->BindEpoch = BindEpoch;
    Proof->Entry = Function->GetNativeFunc();
    Proof->Layout = DescribeTerminalStaticLayout(*Function);
    Proof->Alignment = Function->GetMinAlignment();
    for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
        Proof->Properties.Add(*It);
    return Proof;
}

void SoloASBridge::RetireTerminalStaticNativeProofForTest(const FSoloASBridgeTerminalStaticNativeProofForTest& Proof)
{
    check(IsInGameThread());
    Proof.bRetired.Store(true);
}
#endif

SoloASBridge::FScopedScriptFinishDestroy::FScopedScriptFinishDestroy(UObject* Object)
{
    bActive = IsInGameThread() && GExitPurge && Object;
    if (bActive)
    {
        Previous = ScriptFinishDestroyReceiver;
        ScriptFinishDestroyReceiver = Object;
    }
}

SoloASBridge::FScopedScriptFinishDestroy::~FScopedScriptFinishDestroy()
{
    if (bActive) ScriptFinishDestroyReceiver = Previous;
}

UObject* SoloASBridge::GetScriptFinishDestroyReceiver()
{
    return IsInGameThread() && GExitPurge ? ScriptFinishDestroyReceiver : nullptr;
}

bool SoloASBridge::IsExitPurgeScriptNativeCall()
{
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    return IsExitPurgeRuntimeDestroyCallback();
#else
    return GetScriptFinishDestroyReceiver() != nullptr;
#endif
}

void SoloASBridge::InitializeScriptStaticCallContext(FPendingScriptFinishDestroy PendingObjects)
{
    check(IsInGameThread());
    PendingScriptObjects = PendingObjects;
    if (!StaticCallContext.IsValid())
    {
        auto* Context = NewObject<USoloASBridgeStaticCallContext>(GetTransientPackage());
        Context->AddToRoot();
        StaticCallContext = Context;
    }
}

void SoloASBridge::ReleaseScriptStaticCallContext()
{
    check(IsInGameThread());
    PendingScriptObjects = nullptr;
    if (auto* Context = StaticCallContext.GetEvenIfUnreachable())
        if (Context->IsRooted()) Context->RemoveFromRoot();
    StaticCallContext.Reset();
}

UObject* SoloASBridge::GetScriptStaticCallContext()
{
    auto* Context = GetScriptFinishDestroyReceiver() ? StaticCallContext.GetEvenIfUnreachable() : nullptr;
    return Context && !Context->HasAnyFlags(RF_FinishDestroyed) ? Context : nullptr;
}

bool SoloASBridge::HasPendingScriptFinishDestroyObjects()
{
    return PendingScriptObjects && PendingScriptObjects();
}

// === BEGIN CUVISS STUDIO EXTENSION: CS.Bridge.CallableDispatch.NativeReferenceReturn | by Cuviss Studio ===
SoloASBridge::FNativeReferenceCaller SoloASBridge::GetNativeReferenceCaller(const UFunction* Function)
{
    const UClass* Owner = Function ? Function->GetTypedOuter<UClass>() : nullptr;
    if (!Owner) return nullptr;
    const FString OwnerName = FString(Owner->GetPrefixCPP()) + Owner->GetName();
    const FString FunctionName = Function->GetName();
    const auto Metadata = GetScriptMetadata();
    int32 First = 0;
    int32 Last = Metadata->FunctionTraits.Num();
    while (First < Last)
    {
        const int32 Middle = First + (Last - First) / 2;
        const auto& Row = Metadata->FunctionTraits[Middle];
        int32 Order = OwnerName.Compare(ANSI_TO_TCHAR(Row.OwnerName), ESearchCase::CaseSensitive);
        if (Order == 0)
            Order = FunctionName.Compare(ANSI_TO_TCHAR(Row.FunctionName), ESearchCase::CaseSensitive);
        if (Order == 0) return Row.NativeReferenceCaller;
        if (Order < 0) Last = Middle;
        else First = Middle + 1;
    }
    return nullptr;
}
// === END CUVISS STUDIO EXTENSION: CS.Bridge.CallableDispatch.NativeReferenceReturn | by Cuviss Studio ===

bool SoloASBridge::CanInvokeReflectedCallable(const UFunction* Function)
{
    return Function != nullptr
        && Private::HasSupportedSignature(*Function);
}

ESoloASBridgeCallableDispatchResult SoloASBridge::InvokeReflectedCallable(
    const FSoloASBridgeCallableDispatchRequest& Request
)
{
    const bool bConstructionCall = !IsInGameThread() && ScriptDefaultConstructionObject != nullptr;
    if (!IsInGameThread() && !bConstructionCall)
    {
        return ESoloASBridgeCallableDispatchResult::OffGameThread;
    }

    ESoloASBridgeCallableDispatchResult Failure =
        ESoloASBridgeCallableDispatchResult::InvalidRequest;
    UClass* Owner = nullptr;
    UFunction* DeclaredFunction = nullptr;
    bool bQualifiedStatic = false;
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
    if (Request.TerminalStaticProof != nullptr)
    {
        DeclaredFunction = ValidateTerminalStaticProof(Request, Owner);
        if (!DeclaredFunction) return ESoloASBridgeCallableDispatchResult::InvalidRequest;
        bQualifiedStatic = true;
    }
#endif
    if (!bQualifiedStatic) DeclaredFunction = Private::ResolveDeclaredFunction(Request, Owner, Failure);
    if (DeclaredFunction == nullptr)
    {
        return Failure;
    }
    if (!DeclaredFunction->HasAnyFunctionFlags(FUNC_Native))
    {
        return ESoloASBridgeCallableDispatchResult::NonNativeFunction;
    }
    if (!Private::HasSupportedSignature(*DeclaredFunction))
    {
        return ESoloASBridgeCallableDispatchResult::UnsupportedSignature;
    }

    const bool bStaticProxy = GetScriptFinishDestroyReceiver()
        && DeclaredFunction->HasAllFunctionFlags(FUNC_Static | FUNC_Native);
    UObject* Receiver = bStaticProxy ? GetScriptStaticCallContext() : Request.Target;
    if (!bQualifiedStatic && !bStaticProxy && DeclaredFunction->HasAnyFunctionFlags(FUNC_Static))
    {
        Receiver = Owner->GetDefaultObject(!SoloASBridge::IsExitPurgeScriptNativeCall() && !bConstructionCall);
    }
    else if (!bQualifiedStatic && Receiver == nullptr)
    {
        return ESoloASBridgeCallableDispatchResult::TargetRequired;
    }
    if (!bQualifiedStatic && (Receiver == nullptr || (!bStaticProxy && !Private::IsTargetCompatible(*Receiver, *Owner))))
    {
        return ESoloASBridgeCallableDispatchResult::TargetTypeMismatch;
    }

    UFunction* ActualFunction = (bQualifiedStatic || bStaticProxy) ? DeclaredFunction
        : Receiver->FindFunction(DeclaredFunction->GetFName());
    if (ActualFunction == nullptr)
    {
        return ESoloASBridgeCallableDispatchResult::FunctionNotFound;
    }
    if (!Private::HaveCompatibleSignatures(*DeclaredFunction, *ActualFunction))
    {
        return ESoloASBridgeCallableDispatchResult::UnsupportedSignature;
    }
    if (!Private::HasSupportedParameterLayout(*ActualFunction))
    {
        return ESoloASBridgeCallableDispatchResult::UnsupportedSignature;
    }

    if (bConstructionCall)
    {
        const EObjectFlags DestroyFlags = RF_BeginDestroyed | RF_FinishDestroyed;
        const FObjectInitializer* Initializer = FUObjectThreadContext::Get().TopInitializer();
        const bool bInitializationActive = ScriptDefaultConstructionObject->HasAnyFlags(RF_NeedInitialization)
            || (Initializer && Initializer->GetObj() == ScriptDefaultConstructionObject);
        const bool bScopedReceiver = ActualFunction->HasAnyFunctionFlags(FUNC_Static)
            ? Request.Target == nullptr && Receiver == Owner->GetDefaultObject(false)
                && Receiver->GetClass() == Owner && Receiver->HasAnyFlags(RF_ClassDefaultObject)
                && !Receiver->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad)
            : Receiver == ScriptDefaultConstructionObject || Receiver->IsIn(ScriptDefaultConstructionObject);
        if (GExitPurge || !bInitializationActive || !bScopedReceiver
            || ScriptDefaultConstructionObject->HasAnyFlags(DestroyFlags) || ScriptDefaultConstructionObject->IsUnreachable()
            || Receiver->HasAnyFlags(DestroyFlags) || Receiver->IsUnreachable()
            || ActualFunction != DeclaredFunction || ActualFunction->GetOuterUClass() != Owner
            || !Owner->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic) || Owner->HasAnyClassFlags(CLASS_Interface)
            || Owner->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | DestroyFlags)
            || ActualFunction->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | DestroyFlags)
            || ActualFunction->HasAnyFunctionFlags(FUNC_Event | FUNC_BlueprintEvent | FUNC_Net | FUNC_Delegate | FUNC_MulticastDelegate)
            || !ActualFunction->Script.IsEmpty() || ActualFunction->GetNativeFunc() == nullptr)
            return ESoloASBridgeCallableDispatchResult::OffGameThread;
    }

    const ESoloASBridgeCallableDispatchResult SlotResult =
        Private::ValidateSlots(
            *ActualFunction,
            Request.Arguments,
            Request.ReturnValue
        );
    if (SlotResult != ESoloASBridgeCallableDispatchResult::Succeeded)
    {
        return SlotResult;
    }

    // A: preserve native reference identity; never initialize/copy a value into this pointer slot.
    if (Request.bNativeReferenceReturn)
    {
        const FNativeReferenceCaller Caller = GetNativeReferenceCaller(ActualFunction);
        if (!Caller) return ESoloASBridgeCallableDispatchResult::UnsupportedSignature;
        Caller(Receiver, const_cast<void**>(Request.Arguments.GetData()), Request.ReturnValue);
        return ESoloASBridgeCallableDispatchResult::Succeeded;
    }

    bool bBorrowedNativeReferences = false;
    if (!SoloASBridge::IsExitPurgeScriptNativeCall() && ActualFunction == DeclaredFunction
        && ActualFunction->HasAnyFunctionFlags(FUNC_Native)
        && !ActualFunction->HasAnyFunctionFlags(FUNC_Event | FUNC_BlueprintEvent | FUNC_Net
            | FUNC_Delegate | FUNC_MulticastDelegate)
        && ActualFunction->Script.IsEmpty() && ActualFunction->GetNativeFunc() != nullptr
        && Owner->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic)
        && !Owner->HasAnyClassFlags(CLASS_Interface))
    {
        for (TFieldIterator<FProperty> It(ActualFunction, EFieldIteratorFlags::ExcludeSuper); It; ++It)
            bBorrowedNativeReferences |= It->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm)
                && It->HasAnyPropertyFlags(CPF_Parm) && !It->HasAnyPropertyFlags(CPF_ReturnParm);
    }
    const bool bBorrowedReferences = bBorrowedNativeReferences
        || SoloASBridge::IsExitPurgeScriptNativeCall() || bConstructionCall;
    FCallableParameterBuffer Buffer(*ActualFunction, bBorrowedReferences);
    Private::CopyInputs(*ActualFunction, Request.Arguments, Buffer.Data, bBorrowedReferences);
    if (SoloASBridge::IsExitPurgeScriptNativeCall() || bConstructionCall)
    {
        if (!bConstructionCall)
        {
            const EObjectFlags RejectedFlags = GetScriptFinishDestroyReceiver()
                ? RF_FinishDestroyed : EObjectFlags(RF_BeginDestroyed | RF_FinishDestroyed);
            bool bTerminalRuntimeDestroyCallback = false;
            bool bTerminalRuntimeDestroyReceiverValid = false;
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
            bTerminalRuntimeDestroyCallback = !ActualFunction->HasAnyFunctionFlags(FUNC_Static)
                && SoloASBridge::IsTerminalRuntimeDestroyCallbackForTest();
            if (bTerminalRuntimeDestroyCallback && Receiver != nullptr)
            {
                bTerminalRuntimeDestroyReceiverValid =
                    SoloASBridge::IsTerminalRuntimeDestroyReceiverForTest(Receiver);
            }
#endif
            const bool bReceiverDestroyFlagsClear = Receiver != nullptr
                && (!Receiver->HasAnyFlags(RejectedFlags) || Receiver == GetScriptFinishDestroyReceiver());
            if (Request.DeclaredFunction != ActualFunction
                || !ActualFunction->HasAnyFunctionFlags(FUNC_Native)
                || ActualFunction->HasAnyFunctionFlags(FUNC_Event | FUNC_BlueprintEvent | FUNC_Net
                    | FUNC_Delegate | FUNC_MulticastDelegate)
                || !ActualFunction->Script.IsEmpty() || ActualFunction->GetNativeFunc() == nullptr
                || ActualFunction->HasAnyFlags(RejectedFlags)
                || Owner->HasAnyFlags(RejectedFlags)
                || !Owner->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic)
                || Owner->HasAnyClassFlags(CLASS_Interface)
                || ActualFunction->GetOuterUClass() != Owner
                || ActualFunction != Owner->FindFunctionByName(DeclaredFunction->GetFName()))
            {
                return ESoloASBridgeCallableDispatchResult::InvalidRequest;
            }
            if (ActualFunction->HasAnyFunctionFlags(FUNC_Static))
            {
                if (!bQualifiedStatic && !bStaticProxy)
                {
                    if (!bReceiverDestroyFlagsClear || Receiver != Owner->GetDefaultObject(false)
                        || Receiver->GetClass() != Owner
                        || !Receiver->HasAnyFlags(RF_ClassDefaultObject))
                    {
                        return ESoloASBridgeCallableDispatchResult::InvalidRequest;
                    }
                }
            }
            else if (Receiver != Request.Target || Receiver->HasAnyFlags(RF_ClassDefaultObject)
                || !Receiver->IsA(Owner) || (bTerminalRuntimeDestroyCallback
                    ? !bTerminalRuntimeDestroyReceiverValid
                    : !bReceiverDestroyFlagsClear || !Receiver->IsUnreachable()))
            {
                return ESoloASBridgeCallableDispatchResult::InvalidRequest;
            }
        }
        TArray<FOutParmRec, TInlineAllocator<8>> OutParameters;
        int32 ArgumentIndex = 0;
        for (TFieldIterator<FProperty> It(ActualFunction, EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property->HasAnyPropertyFlags(CPF_Parm) || Property->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
            if (Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm))
            {
                FOutParmRec& Out = OutParameters.AddDefaulted_GetRef();
                Out.Property = Property;
                Out.PropAddr = static_cast<uint8*>(Request.Arguments[ArgumentIndex]);
                Out.NextOutParm = nullptr;
            }
            ++ArgumentIndex;
        }
        for (int32 Index = 1; Index < OutParameters.Num(); ++Index)
        {
            OutParameters[Index - 1].NextOutParm = &OutParameters[Index];
        }
        FFrame Frame(Receiver, ActualFunction, Buffer.Data, nullptr, ActualFunction->ChildProperties);
        Frame.Code = nullptr;
        Frame.OutParms = OutParameters.IsEmpty() ? nullptr : &OutParameters[0];
        Frame.CurrentNativeFunction = ActualFunction;
        void* Result = ActualFunction->ReturnValueOffset == MAX_uint16 ? nullptr
            : static_cast<uint8*>(Buffer.Data) + ActualFunction->ReturnValueOffset;
        ActualFunction->GetNativeFunc()(Receiver, Frame, Result);
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
        if (bQualifiedStatic)
            UE_LOG(LogTemp, Display,
                TEXT("SOLOAS_TERMINAL_STATIC_CALL owner=%s function=%s context_null=1 exact_proof=1 "
                     "binding_epoch=%llu entry_match=1 layout_match=1"),
                *Owner->GetPathName(), *ActualFunction->GetName(), Request.TerminalStaticBindEpoch);
#endif
    }
    else if (bBorrowedNativeReferences)
    {
        // Native exec thunks receive the caller's reference storage through OutParms.
        // Value arguments remain in the owned parameter buffer.
        TArray<FOutParmRec, TInlineAllocator<8>> OutParameters;
        int32 ArgumentIndex = 0;
        for (TFieldIterator<FProperty> It(ActualFunction, EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property->HasAnyPropertyFlags(CPF_Parm) || Property->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
            if (Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm))
            {
                FOutParmRec& Out = OutParameters.AddDefaulted_GetRef();
                Out.Property = Property;
                Out.NextOutParm = nullptr;
                Out.PropAddr = static_cast<uint8*>(Request.Arguments[ArgumentIndex]);
            }
            ++ArgumentIndex;
        }
        for (int32 Index = 1; Index < OutParameters.Num(); ++Index)
            OutParameters[Index - 1].NextOutParm = &OutParameters[Index];
        FFrame Frame(Receiver, ActualFunction, Buffer.Data, nullptr, ActualFunction->ChildProperties);
        Frame.Code = nullptr;
        Frame.OutParms = OutParameters.IsEmpty() ? nullptr : &OutParameters[0];
        Frame.CurrentNativeFunction = ActualFunction;
        void* Result = ActualFunction->ReturnValueOffset == MAX_uint16 ? nullptr
            : static_cast<uint8*>(Buffer.Data) + ActualFunction->ReturnValueOffset;
        ActualFunction->Invoke(Receiver, Frame, Result);
    }
    else
    {
        Receiver->ProcessEvent(ActualFunction, Buffer.Data);
    }
    Private::CopyOutputs(
        *ActualFunction,
        Request.Arguments,
        Request.ReturnValue,
        Buffer.Data,
        bBorrowedReferences
    );
    return ESoloASBridgeCallableDispatchResult::Succeeded;
}

namespace
{
struct FPreparedNativeScalarLayout
{
    int32 Size = 0;
    uint32 Alignment = 0;
};

bool GetPreparedNativeScalarLayout(
    const FProperty& Property,
    FPreparedNativeScalarLayout& OutLayout
)
{
    if (Property.ArrayDim != 1)
    {
        return false;
    }

    const FFieldClass* PropertyClass = Property.GetClass();
    if (PropertyClass == FBoolProperty::StaticClass())
    {
        const FBoolProperty& BoolProperty =
            *CastFieldChecked<FBoolProperty>(&Property);
        if (!BoolProperty.IsNativeBool())
        {
            return false;
        }
    }
    if (PropertyClass == FByteProperty::StaticClass())
    {
        const FByteProperty& ByteProperty = *CastFieldChecked<FByteProperty>(&Property);
        if (ByteProperty.Enum != nullptr)
        {
            return false;
        }
    }
    struct FScalarType
    {
        const FFieldClass* Class;
        int32 Size;
        uint32 Alignment;
    };
    const FScalarType Types[] = {
        {FBoolProperty::StaticClass(), sizeof(bool), alignof(bool)},
        {FByteProperty::StaticClass(), sizeof(uint8), alignof(uint8)},
        {FInt8Property::StaticClass(), sizeof(int8), alignof(int8)},
        {FInt16Property::StaticClass(), sizeof(int16), alignof(int16)},
        {FIntProperty::StaticClass(), sizeof(int32), alignof(int32)},
        {FInt64Property::StaticClass(), sizeof(int64), alignof(int64)},
        {FUInt16Property::StaticClass(), sizeof(uint16), alignof(uint16)},
        {FUInt32Property::StaticClass(), sizeof(uint32), alignof(uint32)},
        {FUInt64Property::StaticClass(), sizeof(uint64), alignof(uint64)},
        {FFloatProperty::StaticClass(), sizeof(float), alignof(float)},
        {FDoubleProperty::StaticClass(), sizeof(double), alignof(double)},
    };
    for (const FScalarType& Type : Types)
    {
        if (PropertyClass != Type.Class) continue;
        OutLayout.Size = Type.Size;
        OutLayout.Alignment = Type.Alignment;
        return SoloASBridge::Compat::GetPropertyElementSize(&Property) == OutLayout.Size
            && Property.GetSize() == OutLayout.Size;
    }
    return false;
}

bool ParameterRangesOverlap(int32 LeftOffset, int32 LeftSize, int32 RightOffset, int32 RightSize)
{
    if (LeftOffset <= RightOffset)
    {
        return RightOffset - LeftOffset < LeftSize;
    }
    return LeftOffset - RightOffset < RightSize;
}

bool ExternalSlotsOverlap(const void* Left, SIZE_T LeftSize, const void* Right, SIZE_T RightSize)
{
    if (
        Left == nullptr
        || Right == nullptr
        || LeftSize == 0
        || RightSize == 0
    )
    {
        return false;
    }

    const UPTRINT LeftAddress = reinterpret_cast<UPTRINT>(Left);
    const UPTRINT RightAddress = reinterpret_cast<UPTRINT>(Right);
    if (LeftAddress <= RightAddress)
    {
        return RightAddress - LeftAddress < LeftSize;
    }
    return LeftAddress - RightAddress < RightSize;
}
}


struct FSoloASBridgePreparedNativeCallable
{
    struct FSlot
    {
        FProperty* Property = nullptr;
        int32 Offset = 0;
        int32 Size = 0;
        bool bBorrowedReference = false;
        bool bIdentitySensitive = false;
    };
    TStrongObjectPtr<UFunction> Function;
    TStrongObjectPtr<UObject> Receiver;
    FNativeFuncPtr Entry = nullptr;
    TArray<FSlot> Arguments;
    int32 ReturnOffset = INDEX_NONE;
    int32 ReturnSize = 0;
    int32 ParameterSize = 0;
    uint32 ParameterAlignment = 0;
};

TSharedPtr<const FSoloASBridgePreparedNativeCallable, ESPMode::ThreadSafe>
SoloASBridge::PrepareNativeCallable(const UFunction* Function)
{
    if (!IsInGameThread() || Function == nullptr
        || !Function->HasAllFunctionFlags(FUNC_Native | FUNC_Static)
        || Function->HasAnyFunctionFlags(
            FUNC_Event | FUNC_BlueprintEvent | FUNC_Net | FUNC_Delegate
            | FUNC_MulticastDelegate)
        || Function->GetNativeFunc() == nullptr || !Function->Script.IsEmpty()
        || Function->GetStructureSize() < Function->ParmsSize
        || !Private::HasSupportedParameterLayout(*Function))
    {
        return nullptr;
    }
    UClass* Owner = Function->GetOuterUClass();
    if (Owner == nullptr || !Owner->HasAnyClassFlags(CLASS_Native)
        || Owner->HasAnyClassFlags(CLASS_Interface)
        || !Owner->GetPathName().StartsWith(TEXT("/Script/")))
    {
        return nullptr;
    }
    const int32 ParameterAlignment = Function->GetMinAlignment();
    if (ParameterAlignment <= 0 || !FMath::IsPowerOfTwo(uint32(ParameterAlignment)))
    {
        return nullptr;
    }
    uint32 RequiredAlignment = uint32(ParameterAlignment);
    auto Prepared = MakeShared<
        FSoloASBridgePreparedNativeCallable, ESPMode::ThreadSafe>();
    for (TFieldIterator<FProperty> It(Function,
        EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        FProperty* Property = *It;
        const bool bReturn = Property->HasAnyPropertyFlags(CPF_ReturnParm);
        const int32 Offset = Property->GetOffset_ForInternal();
        FPreparedNativeScalarLayout ScalarLayout;
        if (!Property->HasAnyPropertyFlags(CPF_Parm)
            || !GetPreparedNativeScalarLayout(*Property, ScalarLayout)
            || ScalarLayout.Size <= 0
            || ScalarLayout.Size > Function->ParmsSize
            || Offset < 0
            || Offset > Function->ParmsSize - ScalarLayout.Size
            || ScalarLayout.Alignment == 0
            || Offset % int32(ScalarLayout.Alignment) != 0
            || (bReturn && (Prepared->ReturnOffset != INDEX_NONE
                || Property->HasAnyPropertyFlags(CPF_ReferenceParm))))
        {
            return nullptr;
        }

        RequiredAlignment = FMath::Max(RequiredAlignment, ScalarLayout.Alignment);
        if (Prepared->ReturnOffset != INDEX_NONE
            && ParameterRangesOverlap(
                Offset,
                ScalarLayout.Size,
                Prepared->ReturnOffset,
                Prepared->ReturnSize
            ))
        {
            return nullptr;
        }
        for (const FSoloASBridgePreparedNativeCallable::FSlot& ExistingSlot :
             Prepared->Arguments)
        {
            if (ParameterRangesOverlap(
                    Offset,
                    ScalarLayout.Size,
                    ExistingSlot.Offset,
                    ExistingSlot.Size
                ))
            {
                return nullptr;
            }
        }

        if (bReturn)
        {
            Prepared->ReturnOffset = Offset;
            Prepared->ReturnSize = ScalarLayout.Size;
            continue;
        }
        FSoloASBridgePreparedNativeCallable::FSlot& Slot =
            Prepared->Arguments.AddDefaulted_GetRef();
        Slot.Property = Property;
        Slot.Offset = Offset;
        Slot.Size = ScalarLayout.Size;
        Slot.bBorrowedReference = Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm);
        Slot.bIdentitySensitive = Property->HasAnyPropertyFlags(
            CPF_OutParm | CPF_ReferenceParm | CPF_ConstParm);
    }
    if (Function->GetStructureSize() != int32(Align(uint32(Function->ParmsSize), RequiredAlignment))) return nullptr;
    UObject* Receiver = Owner->GetDefaultObject();
    const FNativeFuncPtr Entry = Function->GetNativeFunc();
    if (Receiver == nullptr || Entry == nullptr)
    {
        return nullptr;
    }
    Prepared->Function.Reset(const_cast<UFunction*>(Function));
    Prepared->Receiver.Reset(Receiver);
    Prepared->Entry = Entry;
    Prepared->ParameterSize = Function->GetStructureSize();
    Prepared->ParameterAlignment = FMath::Max(8U, RequiredAlignment);
    return Prepared;
}


namespace SoloASBridge::Private
{
void InitializeReflectedReturnCopies()
{
    ReflectedReturnCopies = MakeUnique<FReflectedReturnCopies>();
    ReflectedReturnEndFrameHandle = FCoreDelegates::OnEndFrame.AddLambda([]()
    {
        ReflectedReturnCopies->Frames.Reset();
    });
}
void ShutdownReflectedReturnCopies()
{
    FCoreDelegates::OnEndFrame.Remove(ReflectedReturnEndFrameHandle);
    ReflectedReturnEndFrameHandle.Reset();
    ReflectedReturnCopies.Reset();
}
}

void* SoloASBridge::InvokeReflectedReturnCopy(UObject* Target, UFunction* Function, void** Arguments)
{
    check(IsInGameThread());
    check(Target && Function && Function->GetReturnProperty() && ReflectedReturnCopies);
    // Commandlets/cook need not broadcast OnEndFrame. Their previous call expires here.
    ReflectedReturnCopies->BeforeCall(IsRunningCommandlet() || !GIsRunning);
    TUniquePtr<FStructOnScope> Frame = MakeUnique<FStructOnScope>(Function);
    FStructOnScope* Storage = Frame.Get();
    ReflectedReturnCopies->Frames.Add(MoveTemp(Frame));
    Private::CopyInputs(*Function, MakeArrayView(Arguments, int32(Function->NumParms) - 1), Storage->GetStructMemory());
    Target->ProcessEvent(Function, Storage->GetStructMemory());
    return Function->GetReturnProperty()->ContainerPtrToValuePtr<void>(Storage->GetStructMemory());
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UObject/ObjectRedirector.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoloASReflectedReturnCopyTest,
    "SoloAS.ReflectedReturnCopyLifetime", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSoloASReflectedReturnCopyTest::RunTest(const FString&)
{
    UClass* Owner = UKismetSystemLibrary::StaticClass();
    UFunction* Function = Owner->FindFunctionByName(TEXT("Conv_SoftObjectReferenceToObject"));
    if (!TestNotNull(TEXT("Original native thunk"), Function)) return false;
    UObject* Object = NewObject<UObjectRedirector>();
    TWeakObjectPtr<UObject> WeakObject = Object;
    TSoftObjectPtr<UObject> SoftObject(Object);
    void* Arguments[] = {&SoftObject};
    void* First = SoloASBridge::InvokeReflectedReturnCopy(Owner->GetDefaultObject(), Function, Arguments);
    TestEqual(TEXT("Original thunk result"), *static_cast<UObject**>(First), Object);
    if (!IsRunningCommandlet() && GIsRunning)
    {
        void* Second = SoloASBridge::InvokeReflectedReturnCopy(Owner->GetDefaultObject(), Function, Arguments);
        TestTrue(TEXT("Each invocation owns distinct storage"), First != Second);
        TestEqual(TEXT("Independent copies keep the same value"), *static_cast<UObject**>(Second), Object);
    }
    CollectGarbage(RF_NoFlags);
    TestTrue(TEXT("Frame properties keep returned objects alive"), WeakObject.IsValid());
    ReflectedReturnCopies->Frames.Reset();
    CollectGarbage(RF_NoFlags);
    TestFalse(TEXT("Releasing frames releases object references"), WeakObject.IsValid());
    FReflectedReturnCopies NoFramePool;
    for (int32 Index = 0; Index < 1024; ++Index)
    {
        NoFramePool.BeforeCall(true);
        NoFramePool.Frames.Add(MakeUnique<FStructOnScope>(Function));
    }
    TestEqual(TEXT("No-EndFrame calls retain only the current frame"), NoFramePool.Frames.Num(), 1);
    return true;
}
#endif

ESoloASBridgeCallableDispatchResult SoloASBridge::InvokePreparedNativeCallable(
    const FSoloASBridgePreparedNativeCallable& Prepared,
    TConstArrayView<void*> Arguments,
    void* ReturnValue
)
{
    if (Arguments.Num() != Prepared.Arguments.Num())
    {
        return ESoloASBridgeCallableDispatchResult::ArgumentCountMismatch;
    }
    if (Prepared.ReturnOffset != INDEX_NONE && ReturnValue == nullptr)
    {
        return ESoloASBridgeCallableDispatchResult::MissingReturnStorage;
    }
    for (int32 Index = 0; Index < Arguments.Num(); ++Index)
    {
        if (Arguments[Index] == nullptr)
        {
            return ESoloASBridgeCallableDispatchResult::MissingArgumentStorage;
        }
        if (Prepared.ReturnOffset != INDEX_NONE)
        {
            if (ExternalSlotsOverlap(
                    Arguments[Index],
                    Prepared.Arguments[Index].Size,
                    ReturnValue,
                    Prepared.ReturnSize
                ))
            {
                return ESoloASBridgeCallableDispatchResult::UnsupportedSignature;
            }
        }
        if (!Prepared.Arguments[Index].bIdentitySensitive)
        {
            continue;
        }
        for (int32 Other = 0; Other < Index; ++Other)
        {
            if (!Prepared.Arguments[Other].bIdentitySensitive)
            {
                continue;
            }
            if (ExternalSlotsOverlap(
                    Arguments[Index],
                    Prepared.Arguments[Index].Size,
                    Arguments[Other],
                    Prepared.Arguments[Other].Size
                ))
            {
                return ESoloASBridgeCallableDispatchResult::UnsupportedSignature;
            }
        }
    }
    TArray<uint64, TInlineAllocator<16>> Storage;
    uint8* AlignedStorage = nullptr;
    ON_SCOPE_EXIT { FMemory::Free(AlignedStorage); };
    uint8* Data = nullptr;
    if (Prepared.ParameterAlignment > alignof(uint64))
    {
        const SIZE_T ByteCount = SIZE_T(FMath::Max(1, Prepared.ParameterSize));
        AlignedStorage = static_cast<uint8*>(FMemory::Malloc(ByteCount, Prepared.ParameterAlignment));
        FMemory::Memzero(AlignedStorage, ByteCount);
        Data = AlignedStorage;
    }
    else
    {
        const int32 WordSize = int32(sizeof(uint64));
        Storage.SetNumZeroed(FMath::Max(1, Prepared.ParameterSize / WordSize
            + (Prepared.ParameterSize % WordSize == 0 ? 0 : 1)));
        Data = reinterpret_cast<uint8*>(Storage.GetData());
    }
    TArray<FOutParmRec, TInlineAllocator<8>> OutParameters;
    OutParameters.Reserve(Prepared.Arguments.Num());
    for (int32 Index = 0; Index < Arguments.Num(); ++Index)
    {
        const auto& Slot = Prepared.Arguments[Index];
        if (!Slot.bBorrowedReference)
        {
            FMemory::Memcpy(Data + Slot.Offset, Arguments[Index], Slot.Size);
        }
        if (Slot.bBorrowedReference)
        {
            FOutParmRec& Out = OutParameters.AddDefaulted_GetRef();
            Out.Property = Slot.Property;
            Out.PropAddr = static_cast<uint8*>(Arguments[Index]);
            Out.NextOutParm = nullptr;
        }
    }
    for (int32 Index = 1; Index < OutParameters.Num(); ++Index)
    {
        OutParameters[Index - 1].NextOutParm = &OutParameters[Index];
    }
    UFunction* Function = Prepared.Function.Get();
    FFrame Frame(Prepared.Receiver.Get(), Function, Data, nullptr,
        Function->ChildProperties);
    Frame.Code = nullptr;
    Frame.OutParms = OutParameters.IsEmpty() ? nullptr : &OutParameters[0];
    Frame.CurrentNativeFunction = Function;
    void* Result = Prepared.ReturnOffset == INDEX_NONE
        ? nullptr : Data + Prepared.ReturnOffset;
    Prepared.Entry(Prepared.Receiver.Get(), Frame, Result);
    if (Result != nullptr)
    {
        FMemory::Memcpy(ReturnValue, Result, Prepared.ReturnSize);
    }
    return ESoloASBridgeCallableDispatchResult::Succeeded;
}
