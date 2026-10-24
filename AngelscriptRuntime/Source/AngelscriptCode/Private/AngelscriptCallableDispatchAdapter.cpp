// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptCallableDispatchAdapter.h"
#include "Runtime/SoloASBridgeObjectLifecycle.h"

#include "AngelscriptBinds.h"
#include "AngelscriptType.h"
#include "ClassGenerator/AngelscriptRuntimeGcMirror.h"
#include "AngelscriptExecutionQuiescence.h"
#include "Runtime/SoloASBridgeCallableDispatch.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Templates/Atomic.h"
#include "Templates/UniquePtr.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogAngelscriptCallableDispatch, Log, All);

namespace
{
struct FCallableDispatchKey
{
	FString OwnerPath;
	FName FunctionName;

	bool operator==(const FCallableDispatchKey& Other) const
	{
		return OwnerPath == Other.OwnerPath
			&& FunctionName == Other.FunctionName;
	}
};

uint32 GetTypeHash(const FCallableDispatchKey& Key)
{
	return HashCombine(GetTypeHash(Key.OwnerPath), GetTypeHash(Key.FunctionName));
}

struct FCallableDispatchRecord
{
	FCallableDispatchKey Key;
	uint64 BindEpoch = 0;
	int32 ArgumentCount = 0;
	FString DeclarationLayout;
	bool bStatic = false;
    FAngelscriptTypeUsage SkippedReturnType;
    int32 SkippedReturnSize = 0;
	TWeakObjectPtr<UFunction> ExitPurgeFunction;
	TWeakObjectPtr<UClass> ExitPurgeOwner;
	TWeakObjectPtr<UObject> ExitPurgeReceiver;
	TSharedPtr<const FSoloASBridgePreparedNativeCallable, ESPMode::ThreadSafe>
		PreparedNative;
	TAtomic<bool> bRetired{false};
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
    TSharedPtr<const FSoloASBridgeTerminalStaticNativeProofForTest, ESPMode::ThreadSafe> TerminalStaticProof;
#endif
};

struct FCallableDispatchRegistry
{
	FCriticalSection Mutex;
	TMap<FCallableDispatchKey, FCallableDispatchRecord*> RecordsByKey;
	TArray<TUniquePtr<FCallableDispatchRecord>> OwnedRecords;
	uint64 BindEpoch = 1;
	bool bShutDown = false;
    uint64 ExitPurgeStaticSkips = 0;
};

FCallableDispatchRegistry& GetCallableDispatchRegistry()
{
	static FCallableDispatchRegistry* Registry =
		new FCallableDispatchRegistry();
	return *Registry;
}

FString DescribeCallableLayout(const UFunction* Function)
{
    FString Layout = FString::Printf(TEXT("%u:%d:%d:%d:%d"),
        uint32(Function->FunctionFlags), int32(Function->ParmsSize),
        Function->GetStructureSize(), int32(Function->ReturnValueOffset),
        int32(Function->NumParms));
    for (TFieldIterator<FProperty> It(Function,
        EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        const FProperty* Property = *It;
        Layout += FString::Printf(TEXT("|%s:%s:%s:%llu:%d:%d:%d"),
            *Property->GetName(), *Property->GetClass()->GetName(),
            *Property->GetCPPType(), uint64(Property->GetPropertyFlags()),
            Property->GetOffset_ForInternal(), Property->GetSize(),
            Property->ArrayDim);
    }
    return Layout;
}

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
void ClearTerminalStaticQualification(FCallableDispatchRecord& Record)
{
    if (Record.TerminalStaticProof.IsValid())
        SoloASBridge::RetireTerminalStaticNativeProofForTest(*Record.TerminalStaticProof);
    Record.TerminalStaticProof.Reset();
}
#endif

int32 CountCallableArguments(const UFunction* Function)
{
	int32 ArgumentCount = 0;
	for (
		TFieldIterator<FProperty> It(
			Function,
			EFieldIteratorFlags::ExcludeSuper
		);
		It && It->HasAnyPropertyFlags(CPF_Parm);
		++It
	)
	{
		if (!It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			++ArgumentCount;
		}
	}
	return ArgumentCount;
}

void EncodeDispatchRecord(
	FCallableDispatchRecord* Record,
	asSFuncPtr& OutFunctionPointer
)
{
	static_assert(
		sizeof(Record) == sizeof(OutFunctionPointer.ptr.f.func),
		"AngelScript function storage must hold one dispatch-record pointer."
	);
	FMemory::Memcpy(
		&OutFunctionPointer.ptr.f.func,
		&Record,
		sizeof(Record)
	);
}

FCallableDispatchRecord* DecodeDispatchRecord(asFUNCTION_t EncodedRecord)
{
	FCallableDispatchRecord* Record = nullptr;
	static_assert(
		sizeof(Record) == sizeof(EncodedRecord),
		"AngelScript caller ABI must preserve one dispatch-record pointer."
	);
	FMemory::Memcpy(&Record, &EncodedRecord, sizeof(Record));
	return Record;
}

void DispatchReflectedCallable(
	asFUNCTION_t EncodedRecord,
	void** Parameters,
	void* ReturnValue
)
{
	FAngelscriptExecutionReadScope ExecutionReadScope;
	FCallableDispatchRecord* Record = DecodeDispatchRecord(EncodedRecord);
	if (Record == nullptr)
	{
		UE_LOG(
			LogAngelscriptCallableDispatch,
			Fatal,
			TEXT("AngelScript callable dispatch record is null.")
		);
		return;
	}
	if (Record->bRetired.Load())
	{
		UE_LOG(
			LogAngelscriptCallableDispatch,
			Fatal,
			TEXT(
				"AngelScript callable dispatch attempted to enter a "
				"retired record."
			)
		);
		return;
	}

    UFunction* ExitStatic = Record->bStatic && SoloASBridge::GetScriptFinishDestroyReceiver()
        ? Record->ExitPurgeFunction.GetEvenIfUnreachable() : nullptr;
    const bool bStaticProxy = ExitStatic && ExitStatic->HasAllFunctionFlags(FUNC_Static | FUNC_Native);
    if (Record->bStatic && SoloASBridge::GetScriptFinishDestroyReceiver() && !bStaticProxy)
    {
        // Only native static declarations may execute with the proxy context.
        if (ReturnValue && Record->SkippedReturnSize)
        {
            FMemory::Memzero(ReturnValue, Record->SkippedReturnSize);
            if (Record->SkippedReturnType.Type.IsValid() && Record->SkippedReturnType.NeedConstruct())
                Record->SkippedReturnType.ConstructValue(ReturnValue);
        }
        const uint64 Count = ++GetCallableDispatchRegistry().ExitPurgeStaticSkips;
        UE_LOG(LogAngelscriptCallableDispatch, Display,
            TEXT("EXIT_PURGE_STATIC_NATIVE_SKIPPED count=%llu owner=%s function=%s reason=non_native_or_unavailable"),
            Count, *Record->Key.OwnerPath, *Record->Key.FunctionName.ToString());
        return;
    }

	UObject* Target = nullptr;
	void** ArgumentStorage = Parameters;
	if (!Record->bStatic)
	{
		if (Parameters == nullptr)
		{
			UE_LOG(
				LogAngelscriptCallableDispatch,
				Fatal,
				TEXT("AngelScript instance callable has no target slot.")
			);
			return;
		}
		Target = static_cast<UObject*>(Parameters[0]);
		ArgumentStorage = Parameters + 1;
	}

	FSoloASBridgeCallableDispatchRequest Request;
	Request.OwnerPath = *Record->Key.OwnerPath;
	Request.FunctionName = Record->Key.FunctionName;
	Request.Target = Target;
	if (SoloASBridge::IsExitPurgeScriptNativeCall())
	{
        const EObjectFlags RejectedFlags = SoloASBridge::GetScriptFinishDestroyReceiver()
            ? RF_FinishDestroyed : EObjectFlags(RF_BeginDestroyed | RF_FinishDestroyed);
		UFunction* Function = Record->ExitPurgeFunction.GetEvenIfUnreachable();
		UClass* Owner = Record->ExitPurgeOwner.GetEvenIfUnreachable();
		bool bQualifiedStatic = false;
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
        const auto& Registry = GetCallableDispatchRegistry();
        bQualifiedStatic = Record->bStatic && Record->TerminalStaticProof.IsValid()
            && SoloASBridge::IsTerminalRuntimeDestroyCallbackForTest();
        if (bQualifiedStatic && (Registry.BindEpoch != Record->BindEpoch
            || !Function || Record->DeclarationLayout != DescribeCallableLayout(Function)))
            UE_LOG(LogAngelscriptCallableDispatch, Fatal, TEXT("TERMINAL_STATIC_BINDING_IDENTITY_INVALID"));
#endif
		UObject* Receiver = bStaticProxy ? SoloASBridge::GetScriptStaticCallContext()
            : (Record->bStatic ? Record->ExitPurgeReceiver.GetEvenIfUnreachable() : Target);
        if (bQualifiedStatic) Receiver = nullptr;
        if (!bQualifiedStatic && Record->bStatic && Receiver == nullptr && Record->ExitPurgeReceiver.IsExplicitlyNull() && Owner != nullptr
            && !Owner->HasAnyFlags(RejectedFlags))
        {
            TWeakObjectPtr<UObject> ExistingReceiver(Owner->GetDefaultObject(false));
            Receiver = ExistingReceiver.GetEvenIfUnreachable();
            Record->ExitPurgeReceiver = ExistingReceiver;
        }
        bool bTerminalRuntimeDestroyCallback = false;
        bool bTerminalRuntimeDestroyReceiverValid = false;
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
        bTerminalRuntimeDestroyCallback = !Record->bStatic
            && SoloASBridge::IsTerminalRuntimeDestroyCallbackForTest();
        if (bTerminalRuntimeDestroyCallback && Receiver != nullptr)
        {
            bTerminalRuntimeDestroyReceiverValid =
                SoloASBridge::IsTerminalRuntimeDestroyReceiverForTest(Receiver);
        }
#endif
        const bool bReceiverDestroyFlagsClear = Receiver != nullptr
            && (!Receiver->HasAnyFlags(RejectedFlags) || Receiver == SoloASBridge::GetScriptFinishDestroyReceiver());

		if (Function != nullptr && Owner != nullptr && (bQualifiedStatic || Receiver != nullptr)
			&& !Function->HasAnyFlags(RejectedFlags)
			&& !Owner->HasAnyFlags(RejectedFlags)
			&& Function->HasAnyFunctionFlags(FUNC_Native)
            && Function->HasAnyFunctionFlags(FUNC_Static) == Record->bStatic
			&& Function->GetOuterUClass() == Owner && Function->GetNativeFunc() != nullptr
            && Function == Owner->FindFunctionByName(Record->Key.FunctionName)
			&& Owner->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic)
			&& !Owner->HasAnyClassFlags(CLASS_Interface)
			&& (bQualifiedStatic || bStaticProxy || (Record->bStatic
                ? bReceiverDestroyFlagsClear && Receiver == Owner->GetDefaultObject(false)
                    && Receiver->GetClass() == Owner && Receiver->HasAnyFlags(RF_ClassDefaultObject)
                : Receiver == Target && !Receiver->HasAnyFlags(RF_ClassDefaultObject)
                    && Receiver->IsA(Owner) && (bTerminalRuntimeDestroyCallback
                        ? bTerminalRuntimeDestroyReceiverValid
                        : bReceiverDestroyFlagsClear && Receiver->IsUnreachable()))))
		{
			Request.DeclaredFunction = Function;
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
            if (bQualifiedStatic)
            {
                Request.TerminalStaticProof = Record->TerminalStaticProof.Get();
                Request.TerminalStaticBindEpoch = Record->BindEpoch;
            }
#endif
		}
		else
		{
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
            const int32 TerminalCallbackActive =
                SoloASBridge::IsTerminalRuntimeDestroyCallbackForTest() ? 1 : 0;
#else
            const int32 TerminalCallbackActive = -1;
#endif
            UE_LOG(LogAngelscriptCallableDispatch, Fatal,
                TEXT("EXIT_PURGE_NATIVE_CALLABLE_IDENTITY_INVALID function=%d owner=%d receiver=%d receiver_missing=%d "
                     "owner_path=%s function_name=%s static=%d target=%d cached_receiver_explicit_null=%d "
                     "cached_receiver_valid=%d cached_receiver_stale=%d terminal_callback_active=%d "
                     "function_destroyed=%d owner_destroyed=%d receiver_destroyed=%d"),
                Function != nullptr ? 1 : 0, Owner != nullptr ? 1 : 0, Receiver != nullptr ? 1 : 0,
                Record->ExitPurgeReceiver.IsExplicitlyNull() ? 1 : 0,
                *Record->Key.OwnerPath, *Record->Key.FunctionName.ToString(), Record->bStatic ? 1 : 0,
                Target != nullptr ? 1 : 0, Record->ExitPurgeReceiver.IsExplicitlyNull() ? 1 : 0,
                Record->ExitPurgeReceiver.IsValid(true, true) ? 1 : 0,
                Record->ExitPurgeReceiver.IsStale(false, true) ? 1 : 0, TerminalCallbackActive,
                Function != nullptr ? (Function->HasAnyFlags(RejectedFlags) ? 1 : 0) : -1,
                Owner != nullptr ? (Owner->HasAnyFlags(RejectedFlags) ? 1 : 0) : -1,
                Receiver != nullptr ? (Receiver->HasAnyFlags(RejectedFlags) ? 1 : 0) : -1);
		}
	}

	Request.Arguments = TConstArrayView<void*>(
		ArgumentStorage,
		Record->ArgumentCount
	);
	Request.ReturnValue = ReturnValue;

	const ESoloASBridgeCallableDispatchResult Result =
        !IsInGameThread() && Record->PreparedNative.IsValid()
        ? SoloASBridge::InvokePreparedNativeCallable(
            *Record->PreparedNative, Request.Arguments, ReturnValue)
        : SoloASBridge::InvokeReflectedCallable(Request);
    if (Result == ESoloASBridgeCallableDispatchResult::Succeeded && Record->bStatic
        && IsInGameThread() && !GExitPurge)
    {
        if (UClass* Owner = Record->ExitPurgeOwner.Get())
        {
            Record->ExitPurgeReceiver = Owner->GetDefaultObject(false);
        }
    }
	if (Result != ESoloASBridgeCallableDispatchResult::Succeeded)
	{
		UE_LOG(
			LogAngelscriptCallableDispatch,
			Fatal,
			TEXT(
				"Bridge rejected AngelScript callable %s.%s "
				"(result %u)."
			),
			*Record->Key.OwnerPath,
			*Record->Key.FunctionName.ToString(),
			static_cast<uint32>(Result)
		);
	}
}
}

                                                                                                 
bool IsAngelscriptCallableDispatchCaller(const asFunctionCaller& Caller)
{
	return Caller.type == 1 && Caller.FunctionCaller == &DispatchReflectedCallable;
}
                                                                                               

bool RegisterAngelscriptCallableDispatch(
	const UFunction* Function,
	FASBindFunctionPointers& OutFunctionPointers
)
{
	if (
		Function == nullptr
		|| !Function->HasAnyFunctionFlags(FUNC_Native)
		|| !SoloASBridge::CanInvokeReflectedCallable(Function)
	)
	{
		return false;
	}

    if (!IsInGameThread())
    {
        return false;
    }
    FAngelscriptExecutionWriteScope ExecutionWriteScope;
    SoloASBridge::InitializeScriptStaticCallContext(&AngelscriptRuntimeGcMirror::HasRuntimeObjectsPendingFinishDestroy);
    auto PreparedNative = SoloASBridge::PrepareNativeCallable(Function);

	const UClass* OwnerClass = Function->GetOuterUClass();
	if (OwnerClass == nullptr)
	{
		return false;
	}

	FCallableDispatchKey Key{
		OwnerClass->GetPathName(),
		Function->GetFName()
	};
	if (!Key.OwnerPath.StartsWith(TEXT("/Script/")))
	{
		return false;
	}

    const FString DeclarationLayout = DescribeCallableLayout(Function);
	const int32 ArgumentCount = CountCallableArguments(Function);
	const bool bStatic = Function->HasAnyFunctionFlags(FUNC_Static);

	FCallableDispatchRegistry& Registry = GetCallableDispatchRegistry();
	FScopeLock RegistryLock(&Registry.Mutex);
	checkf(
		!Registry.bShutDown,
		TEXT("AngelScript callable registration occurred after shutdown.")
	);
	if (Registry.bShutDown)
	{
		return false;
	}

	FCallableDispatchRecord* Record = Registry.RecordsByKey.FindRef(Key);
	if (Record == nullptr)
	{
		TUniquePtr<FCallableDispatchRecord> NewRecord =
			MakeUnique<FCallableDispatchRecord>();
		NewRecord->Key = MoveTemp(Key);
		NewRecord->BindEpoch = Registry.BindEpoch;
		NewRecord->ArgumentCount = ArgumentCount;
        NewRecord->DeclarationLayout = DeclarationLayout;
		NewRecord->bStatic = bStatic;
		Record = NewRecord.Get();
		Registry.RecordsByKey.Add(Record->Key, Record);
		Registry.OwnedRecords.Add(MoveTemp(NewRecord));
	}
	else
	{
        if (Record->DeclarationLayout != DeclarationLayout
            || (Record->PreparedNative.IsValid() && !PreparedNative.IsValid()))
        {
            return false;
        }
		const bool bDescriptorMatches =
			Record->BindEpoch == Registry.BindEpoch
			&& Record->ArgumentCount == ArgumentCount
			&& Record->bStatic == bStatic
			&& !Record->bRetired.Load();
		checkf(
			bDescriptorMatches,
			TEXT(
				"AngelScript callable re-registration descriptor "
				"mismatch for %s.%s."
			),
			*Record->Key.OwnerPath,
			*Record->Key.FunctionName.ToString()
		);
		if (!bDescriptorMatches)
		{
			return false;
		}
	}

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
    ClearTerminalStaticQualification(*Record);
#endif
    Record->PreparedNative = MoveTemp(PreparedNative);
    if (FProperty* ReturnProperty = Function->GetReturnProperty())
    {
        Record->SkippedReturnType = FAngelscriptTypeUsage::FromProperty(ReturnProperty);
        Record->SkippedReturnSize = ReturnProperty->GetSize();
    }
    if (OwnerClass->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic)
        && !OwnerClass->HasAnyClassFlags(CLASS_Interface))
    {
        Record->ExitPurgeFunction = const_cast<UFunction*>(Function);
        Record->ExitPurgeOwner = const_cast<UClass*>(OwnerClass);
        if (bStatic)
        {
            Record->ExitPurgeReceiver = OwnerClass->GetDefaultObject(false);
        }
        else
        {
            Record->ExitPurgeReceiver.Reset();
        }
    }
    else
    {
        Record->ExitPurgeFunction.Reset();
        Record->ExitPurgeOwner.Reset();
        Record->ExitPurgeReceiver.Reset();
    }


	OutFunctionPointers.FunctionPointer = asSFuncPtr(
		Record->bStatic ? 2 : 3
	);
	EncodeDispatchRecord(
		Record,
		OutFunctionPointers.FunctionPointer
	);
	OutFunctionPointers.FunctionCaller = asFunctionCaller(
		static_cast<asFunctionCaller::FunctionCallerPtr>(
			&DispatchReflectedCallable
		)
	);
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
bool QualifyContextIndependentStaticNativeCallablesForTest(TConstArrayView<const UFunction*> Functions)
{
    if (!IsInGameThread() || GExitPurge || Functions.IsEmpty()) return false;
    TOptional<FAngelscriptExecutionWriteScope> Writer;
    if (!IsAngelscriptExecutionWriterHeldByCurrentThread()) Writer.Emplace();
    auto& Registry = GetCallableDispatchRegistry();
    FScopeLock Lock(&Registry.Mutex);
    if (Registry.bShutDown) return false;
    TArray<FCallableDispatchRecord*> Selected;
    TArray<TSharedPtr<const FSoloASBridgeTerminalStaticNativeProofForTest, ESPMode::ThreadSafe>> Proofs;
    for (const UFunction* Function : Functions)
    {
        if (!Function) return false;
        const UClass* Owner = Function->GetOuterUClass();
        if (!Owner) return false;
        const FCallableDispatchKey Key{Owner->GetPathName(), Function->GetFName()};
        auto* Record = Registry.RecordsByKey.FindRef(Key);
        if (!Record || !Record->bStatic || Record->bRetired.Load() || Record->BindEpoch != Registry.BindEpoch
            || Selected.Contains(Record) || Record->TerminalStaticProof.IsValid()
            || Record->ExitPurgeFunction.Get() != Function || Record->ExitPurgeOwner.Get() != Owner
            || Record->DeclarationLayout != DescribeCallableLayout(Function)) return false;
        auto Proof = SoloASBridge::CreateTerminalStaticNativeProofForTest(
            Record->ExitPurgeFunction, Record->ExitPurgeOwner, Record->BindEpoch);
        if (!Proof.IsValid()) return false;
        Selected.Add(Record);
        Proofs.Add(MoveTemp(Proof));
    }
    for (int32 Index = 0; Index != Selected.Num(); ++Index)
        Selected[Index]->TerminalStaticProof = MoveTemp(Proofs[Index]);
    UE_LOG(LogTemp, Display, TEXT("SOLOAS_TERMINAL_STATIC_QUALIFY functions=%d binding_epoch=%llu source_review_assertion=1"),
        Selected.Num(), Registry.BindEpoch);
    return true;
}

void ClearContextIndependentStaticNativeQualificationsForTest()
{
    check(IsInGameThread());
    TOptional<FAngelscriptExecutionWriteScope> Writer;
    if (!IsAngelscriptExecutionWriterHeldByCurrentThread()) Writer.Emplace();
    auto& Registry = GetCallableDispatchRegistry();
    FScopeLock Lock(&Registry.Mutex);
    int32 Cleared = 0;
    for (auto& Record : Registry.OwnedRecords)
    {
        Cleared += Record->TerminalStaticProof.IsValid() ? 1 : 0;
        ClearTerminalStaticQualification(*Record);
    }
    UE_LOG(LogTemp, Display, TEXT("SOLOAS_TERMINAL_STATIC_CLEAR functions=%d binding_epoch=%llu qualifications=0"), Cleared, Registry.BindEpoch);
}
#endif

void ShutdownAngelscriptCallableDispatch()
{
	if (!IsAngelscriptExecutionWriterHeldByCurrentThread())
	{
		UE_LOG(
			LogAngelscriptCallableDispatch,
			Fatal,
			TEXT(
				"AngelScript callable records may retire only after "
				"execution quiescence."
			)
		);
		return;
	}

	FCallableDispatchRegistry& Registry = GetCallableDispatchRegistry();
	FScopeLock RegistryLock(&Registry.Mutex);
	Registry.bShutDown = true;
	for (const TUniquePtr<FCallableDispatchRecord>& Record :
		Registry.OwnedRecords)
	{
		Record->bRetired.Store(true);
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
        ClearTerminalStaticQualification(*Record);
#endif
		Record->PreparedNative.Reset();
	}
	SoloASBridge::ReleaseScriptStaticCallContext();
	Registry.RecordsByKey.Reset();
	++Registry.BindEpoch;
}
