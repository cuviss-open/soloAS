// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#ifndef SOLOAS_LEGACY_TERMINAL_LIFECYCLE
#define SOLOAS_LEGACY_TERMINAL_LIFECYCLE 0
#endif

#include "CoreMinimal.h"
#include "AngelscriptManager.h"

ANGELSCRIPTCODE_API void RegisterAngelscriptRuntimeGcLifecycle();
ANGELSCRIPTCODE_API void ShutdownAngelscriptRuntimeGcLifecycle();

class UASClass;
class asITypeInfo;
class asIScriptModule;

void RebuildRuntimeTerminalClassChain(UASClass* Class);
void RetireRuntimeTerminalClassChain(UASClass* Class);
bool IsRuntimeTerminalClassReadyForFinish(const UASClass* Class);

// Only the generator's actual removed-class transition opens registration.
void RetireRemovedRuntimeClass(UClass* Class);
void RecordRemovedRuntimeReceiver(UObject* Object, asITypeInfo* Type, asIScriptModule* Module, uint64 Sequence);
bool IsRetiredRuntimeReceiverIdentity(UObject* Object, asITypeInfo* Type, asIScriptModule* Module,
    const UASClass* Class, uint64 Sequence);
void ForgetRemovedRuntimeReceiver(UObject* Object, asITypeInfo* Type);
// Final ordinary callback fence after Dispatching claim and all callouts; consumes once.
bool ValidateOrdinaryRemovedRuntimeCallback(UObject* Object, UASClass* Owner, asITypeInfo* Type);
// Silent admission fence; the plan owns the module lease throughout completion and typed cleanup.
bool ValidateRuntimeTerminalSourceAssociation(void* Object, asITypeInfo* Type, uint64 PlanId, uint64 SourceGeneration);

// Synchronous observation during a witnessed literal LogDisplay call. Identities are
// scalar values, not permission to dereference a destroyed receiver or released payload.
struct FAngelscriptTerminalResourceView
{
    UPTRINT Receiver = 0;
    UPTRINT FreshStorage = 0;
    UPTRINT Resource = 0;
    UPTRINT ValueType = 0;
    UPTRINT LocalStorage = 0;
    uint64 FreshEpoch = 0;
    uint64 AllocationGeneration = 0;
    uint64 ValueGeneration = 0;
    uint32 ProgramPosition = 0;
    uint8 ResourcePhase = 0;
    bool bHasInitializedLocal = false;
    double Components[3] = {};
};
ANGELSCRIPTCODE_API bool ReadActiveAngelscriptTerminalResourceView(FAngelscriptTerminalResourceView& Out);

// Bounded development experiment only. The ticket names an independently placement-
// constructed complete int outside the native UObject prefix, not an arbitrary AS member.
struct FAngelscriptTerminalFinalInt32Ticket
{
    int32* Storage = nullptr;
    uint64 ProviderEpoch = 0;
    uint64 SlotEpoch = 0;
};

class FIntProperty;
class FStructProperty;
class UScriptStruct;
struct FAngelscriptTerminalFinalInt32Registration
{
    UObject* Receiver = nullptr;
    const FIntProperty* Scalar = nullptr;
    const FStructProperty* Owned = nullptr;
    const UScriptStruct* PayloadType = nullptr;
    const void* Payload = nullptr;
    const void* ProviderIdentity = nullptr;
    const uint64* DeclaredEpoch = nullptr;
    const void* const* DeclaredProvider = nullptr;
    uint64 ProviderEpoch = 0;
    int32 InitialValue = 0;
    bool bRuntimeOwnedOriginal = false;
};
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
// Explicit opt-in requires a provider for every selected scalar, including negatives.
ANGELSCRIPTCODE_API bool RequireAngelscriptTerminalFinalInt32ForExperiment(bool bStaleCaptureControl = false);
// Exact ordinary native assignment leaves the complete int live; Runtime owns its
// typed end after the owned field's native DestroyValueInternal call returns.
ANGELSCRIPTCODE_API bool RequireAngelscriptTerminalRuntimeOwnedInt32ForExperiment(bool bStaleCaptureControl = false);
ANGELSCRIPTCODE_API bool RegisterAngelscriptTerminalFinalInt32ForExperiment(
    const FAngelscriptTerminalFinalInt32Registration& Registration, FAngelscriptTerminalFinalInt32Ticket& Out);
// Native entry consumes its exact payload identity once before any typed write.
ANGELSCRIPTCODE_API bool BeginAngelscriptTerminalFinalInt32WriterForExperiment(
    const FAngelscriptTerminalFinalInt32Ticket& Ticket, const void* ProviderIdentity, const void* NativePayload);
// The exact native provider writes, publishes BY VALUE, then explicitly ends its
// complete int and acknowledges the end. These functions never read old scalar storage.
ANGELSCRIPTCODE_API bool PublishAngelscriptTerminalFinalInt32ForExperiment(
    const FAngelscriptTerminalFinalInt32Ticket& Ticket, const void* ProviderIdentity, int32 FinalValue);
ANGELSCRIPTCODE_API bool EndAngelscriptTerminalFinalInt32ForExperiment(
    const FAngelscriptTerminalFinalInt32Ticket& Ticket, const void* ProviderIdentity);
#endif

// Excludes terminal preparation before compiler callouts and class/type mutations.
// Initial compilation may run on its initialization thread; reloads require the game thread.
class ANGELSCRIPTCODE_API FAngelscriptRuntimeGcCompilationScope
{
public:
    explicit FAngelscriptRuntimeGcCompilationScope(bool bInitialCompile);
    ~FAngelscriptRuntimeGcCompilationScope() noexcept;
    bool IsAdmitted() const { return bAdmitted; }

    FAngelscriptRuntimeGcCompilationScope(const FAngelscriptRuntimeGcCompilationScope&) = delete;
    FAngelscriptRuntimeGcCompilationScope& operator=(const FAngelscriptRuntimeGcCompilationScope&) = delete;

private:
    bool bAdmitted = false;
};

// Only the registered editor hook around synchronous reinstance full-purge GC may open this scope.
// It borrows the admitted compiler epoch without releasing its barrier or retaining another module.
class ANGELSCRIPTCODE_API FAngelscriptRuntimeReinstanceGcScope
{
public:
    explicit FAngelscriptRuntimeReinstanceGcScope(const TMap<UClass*, UClass*>& ReloadClasses,
        bool bBeforeReinstance = false);
    ~FAngelscriptRuntimeReinstanceGcScope() noexcept;
    FAngelscriptRuntimeReinstanceGcScope(const FAngelscriptRuntimeReinstanceGcScope&) = delete;
    FAngelscriptRuntimeReinstanceGcScope& operator=(const FAngelscriptRuntimeReinstanceGcScope&) = delete;

private:
    uint64 Epoch = 0;
};

// Game-thread ordinary reload requests; the barrier lock never spans filename or compiler work.
ANGELSCRIPTCODE_API bool IsAngelscriptRuntimeGcCompilationAvailable();
ANGELSCRIPTCODE_API void QueueAngelscriptRuntimeGcCompilationRetry();
ANGELSCRIPTCODE_API bool HasAngelscriptRuntimeGcCompilationRetry();
ANGELSCRIPTCODE_API bool ConsumeAngelscriptRuntimeGcCompilationRetry();

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
class FStructProperty;

enum class EAngelscriptDelayedFieldOperationForTest : uint8
{
    DestroyValue,
    FinishDestroy,
};

// ValueAddress is an identity only after DestroyValue; its value lifetime has ended.
using FAngelscriptDelayedFieldObserverForTest = void (*)(const FStructProperty& Property,
    const void* ValueAddress, EAngelscriptDelayedFieldOperationForTest Operation, uint32 OriginalCalls);

// Game-thread only. The observing fixture owns registration, lifetime and admission.
// The process starts with a null observer; no automatic activation is provided.
ANGELSCRIPTCODE_API void SetAngelscriptDelayedFieldObserverForTest(
    FAngelscriptDelayedFieldObserverForTest Observer);

// Independent opt-in authority; observation alone cannot defer or dispatch destruction.
ANGELSCRIPTCODE_API bool ArmRuntimeTerminalDestroyForTest(
    UObject* const (&Objects)[2], const FStructProperty& Property, uint64 ArmEpoch);
ANGELSCRIPTCODE_API void DisarmRuntimeTerminalDestroyForTest();

enum class EAngelscriptTerminalFieldFaultForTest : uint8
{
    DuplicateCompletion,
    WrongPropertyCompletion,
};

// Explicit one-shot fault at the admitted FIRST observer, after its real original Destroy.
// Ends the proven fresh typed-empty wrapper once, then submits the selected completion identity.
ANGELSCRIPTCODE_API bool InjectRuntimeTerminalFieldFaultForTest(UObject* Receiver,
    const FStructProperty& FirstProperty, const void* ValueAddress, uint64 ArmEpoch,
    EAngelscriptTerminalFieldFaultForTest Fault);
#endif


#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
// Exact direct AS class with one native FInstancedStruct tail, one local owned
// wrapper and one local scalar. NativePrefixBytes must equal the UHT native size.
struct FAngelscriptTerminalNativeTailRegistrationForTest
{
    UASClass* Class = nullptr;
    UClass* NativeClass = nullptr;
    FStructProperty* Owned = nullptr;
    const FStructProperty* NativeTail = nullptr;
    const FIntProperty* Scalar = nullptr;
    int32 NativePrefixBytes = 0;
    // Optional bounded inherited experiment: the two exact empty direct AS
    // siblings must already be compiled. Both null retains the direct route.
    UASClass* EmptyChildren[2]{};
};

// Default disabled. Caller creates this after class build and before instances,
// requires the Runtime-owned int opt-in, holds it through synchronous GC and
// native cleanup, then releases after join. Rooted unregistered controls may
// survive release and subsequently use the restored ordinary native chain.
// It owns only a stable off-list destructor node; no instance storage is added.
class ANGELSCRIPTCODE_API FAngelscriptTerminalNativeTailScopeForTest
{
public:
    explicit FAngelscriptTerminalNativeTailScopeForTest(
        const FAngelscriptTerminalNativeTailRegistrationForTest& Registration, bool bEnable = false);
    ~FAngelscriptTerminalNativeTailScopeForTest();
    bool IsAdmitted() const { return Epoch != 0; }
    FAngelscriptTerminalNativeTailScopeForTest(const FAngelscriptTerminalNativeTailScopeForTest&) = delete;
    FAngelscriptTerminalNativeTailScopeForTest& operator=(const FAngelscriptTerminalNativeTailScopeForTest&) = delete;
private:
    uint64 Epoch = 0;
};

// Uses the existing exact native payload declaration and independent complete
// int ticket. Its late capture and typed end occur after native FinishDestroy.
ANGELSCRIPTCODE_API bool RegisterAngelscriptTerminalNativeTailInt32ForTest(
    const FAngelscriptTerminalFinalInt32Registration& Registration, FAngelscriptTerminalFinalInt32Ticket& Out);
// Only the active qualified AS callback can observe its fresh scalar. Ticket
// addresses are identities outside this call; Runtime ends both scalar lifetimes.
ANGELSCRIPTCODE_API bool ReadActiveAngelscriptTerminalNativeTailInt32ForTest(UObject* Receiver,
    const FAngelscriptTerminalFinalInt32Ticket& Ticket, int32& OutValue);
#endif
