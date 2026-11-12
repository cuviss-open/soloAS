// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptExecutionBudget.h"
                                                                                                          
#include "AngelscriptBinds.h"
                                                                                                        
#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeRWLock.h"
#include "StaticJIT/AngelscriptStaticJIT.h"
#include "StartAngelscriptHeaders.h"
#include "as_context.h"
#include "as_builder.h"
#include "as_bytecode.h"
#include "as_module.h"
#include "as_scriptengine.h"
#include "as_scriptfunction.h"
#include "EndAngelscriptHeaders.h"

                                                                                                  
namespace AngelscriptCoreExtensionPoints
{
namespace Private
{
std::atomic<bool> GExecutionBudgetRegistered { false };
}
namespace
{
constexpr uint32 BudgetJITVersion = 1;
FExecutionBudgetProvider Provider;
FDelegateHandle ProviderHandle;
TMap<void*, asCScriptFunction*> RawFunctions;
FRWLock RawFunctionsLock;
TSet<asCContext*> PendingUnwinds;
TMap<asCModule*, FModuleGenerationId> BoundGenerations;
FRWLock PendingUnwindsLock;
std::atomic<uint32> TargetModules { 0 };
bool bStopped = false;
thread_local bool bInCallback = false;

struct FInterval
{
    uint32 Depth = 0;
    uint32 JITDepth = 0;
    double Started = 0;
    double Deadline = 0;
    FExecutionBudgetEvent Event;
    bool bFailed = false;
    bool bExecutionFailed = false;
    TArray<FModuleGenerationId> CompletionModules;
    const ANSICHAR* Error = nullptr;
    Private::FExecutionBudgetCleanupSet Cleanups;
    asCScriptFunction* EntryFunction = nullptr;
};
thread_local FInterval Interval;

uint64 Query(asCModule* Module)
{
    if (!Module) return 0;
    Private::FNativeCompileCallbackScope NoNativePermit(nullptr);
    TGuardValue<bool> Guard(bInCallback, true);
    return Provider.QueryBudgetNs({ANSI_TO_TCHAR(Module->baseModuleName.GetLength()
        ? Module->baseModuleName.AddressOf() : Module->GetName()), Private::GetModuleGeneration(Module)});
}

void RetainTarget(asCModule* Module)
{
    if (!Module->bExecutionBudgetTarget.exchange(true, std::memory_order_relaxed))
        TargetModules.fetch_add(1, std::memory_order_relaxed);
}

bool Fail(const ANSICHAR* Error)
{
    Interval.bFailed = true;
    Interval.Error = Error;
    return false;
}

bool HasUninstrumentedJIT(asCScriptFunction* Function)
{
    return (Function->jitFunction || Function->jitFunction_Raw || Function->jitFunction_ParmsEntry)
        && Function->ExecutionBudgetVersion != BudgetJITVersion;
}

bool MissingDestructorCleanup(asCScriptFunction* Function)
{
    Private::RestoreExecutionBudgetCleanup(Function);
    return (Function->traits.GetTrait(asTRAIT_DESTRUCTOR) || (Function->name.GetLength() && Function->name[0] == '~'))
        && Function->ExecutionBudgetCleanupOffset < 0;
}
}

FDelegateHandle RegisterExecutionBudget(FExecutionBudgetProvider InProvider)
{
    if (!IsInGameThread() || bInCallback || ProviderHandle.IsValid() || bStopped
        || !InProvider.QueryBudgetNs || !InProvider.OnExceeded
        || bool(InProvider.BindGeneration) != bool(InProvider.ReleaseGeneration)
        || (FAngelscriptManager::IsInitialized() && FAngelscriptManager::Get().bIsCompilingModules)) return {};
    FAngelscriptExecutionWriteScope Writer(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Writer.IsAcquired()) return {};
    Provider = MoveTemp(InProvider);
    // A provider cannot be installed over old target JIT. This preflight also
    // covers a non-target JIT caller whose original bytes cannot gain a fence.
    asCScriptEngine* Engine = FAngelscriptManager::IsInitialized()
        ? static_cast<asCScriptEngine*>(FAngelscriptManager::Get().GetScriptEngine()) : nullptr;
    if (Engine)
    {
        for (asUINT I = 0; I < Engine->scriptFunctions.GetLength(); ++I)
        {
            auto* Function = Engine->scriptFunctions[I];
            if (Function && Query(Function->module)
                && (HasUninstrumentedJIT(Function) || MissingDestructorCleanup(Function)
                    || !Private::ValidateExecutionBudgetFunction(Function, true)))
            {
                UE_LOG(Angelscript, Warning, TEXT("Execution budget registration rejected: target JIT/lifecycle closure requires normal recompilation."));
                Provider = {};
                return {};
            }
        }
        for (asUINT I = 0; I < Engine->scriptFunctions.GetLength(); ++I)
        {
            auto* Function = Engine->scriptFunctions[I];
            if (!Function) continue;
            if (Query(Function->module)) RetainTarget(Function->module);
            if (Function->jitFunction_Raw)
            {
                FWriteScopeLock Lock(RawFunctionsLock);
                RawFunctions.Add((void*)Function->jitFunction_Raw, Function);
            }
        }
    }
    ProviderHandle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
    Private::GExecutionBudgetRegistered.store(true, std::memory_order_release);
    return ProviderHandle;
}

ERemoveResult UnregisterExecutionBudget(FDelegateHandle Handle)
{
    if (!IsInGameThread() || bInCallback
        || (FAngelscriptManager::IsInitialized() && FAngelscriptManager::Get().bIsCompilingModules)) return ERemoveResult::Busy;
    if (!Handle.IsValid() || Handle != ProviderHandle) return ERemoveResult::Unknown;
    FAngelscriptExecutionWriteScope Writer(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Writer.IsAcquired() || TargetModules.load(std::memory_order_relaxed)) return ERemoveResult::Busy;
    {
        FReadScopeLock Lock(PendingUnwindsLock);
        if (!PendingUnwinds.IsEmpty()) return ERemoveResult::Busy;
    }
    for (const auto& Function : FJITDatabase::Get().Functions)
        if (Function.Value.ExecutionBudgetVersion) return ERemoveResult::Busy;
    Private::GExecutionBudgetRegistered.store(false, std::memory_order_release);
    if (Provider.ReleaseGeneration)
    {
        Private::FNativeCompileCallbackScope NoNativePermit(nullptr);
        TGuardValue<bool> Guard(bInCallback, true);
        for (const auto& Entry : BoundGenerations) Provider.ReleaseGeneration(Entry.Value);
        BoundGenerations.Empty();
    }
    {
        FWriteScopeLock Lock(RawFunctionsLock);
        RawFunctions.Empty();
    }
    Provider = {};
    ProviderHandle.Reset();
    return ERemoveResult::Removed;
}

namespace Private
{
FExecutionBudgetInterval::FExecutionBudgetInterval()
{
    if (!GExecutionBudgetRegistered.load(std::memory_order_acquire)) return;
    bAcquired = true;
    BeginExecutionBudgetInterval();
}

FExecutionBudgetInterval::~FExecutionBudgetInterval()
{
    if (bAcquired) EndExecutionBudgetInterval();
}

void BeginExecutionBudgetInterval()
{
    if (Interval.Depth == 0) Interval = {};
    ++Interval.Depth;
}

void EndExecutionBudgetInterval()
{
    check(Interval.Depth != 0);
    if (Provider.OnCompleted && !Interval.CompletionModules.IsEmpty())
    {
        auto* Execution = asCThreadManager::GetLocalData()->activeExecution;
        if (Execution && Execution->bExceptionThrown) Interval.bExecutionFailed = true;
    }
    if (--Interval.Depth != 0) return;
    if (Provider.OnCompleted && !Interval.CompletionModules.IsEmpty())
    {
        Private::FNativeCompileCallbackScope NoNativePermit(nullptr);
        TGuardValue<bool> Guard(bInCallback, true);
        for (const auto& Module : Interval.CompletionModules)
            Provider.OnCompleted(Module, !Interval.bFailed && !Interval.bExecutionFailed);
    }
    Interval = {};
}

FExecutionBudgetCleanupScope::FExecutionBudgetCleanupScope(asCContext* InContext)
    : Context(InContext)
{
    if (!Context->bExecutionBudgetUnwind || !GExecutionBudgetRegistered.load(std::memory_order_acquire)) return;
    bActive = true;
    bPreviousFailure = Interval.bFailed;
    PreviousError = Interval.Error;
    BeginExecutionBudgetInterval();
    Interval.bFailed = true;
    Interval.Cleanups.Append(Context->ExecutionBudgetCleanups);
    if (!Interval.Error) Interval.Error = "Script execution budget exceeded.";
}

FExecutionBudgetCleanupScope::~FExecutionBudgetCleanupScope()
{
    if (!bActive) return;
    Context->ExecutionBudgetCleanups = Interval.Cleanups;
    Interval.bFailed = bPreviousFailure;
    Interval.Error = PreviousError;
    EndExecutionBudgetInterval();
}

FExecutionBudgetContextExit::FExecutionBudgetContextExit(asCContext* InContext, bool bInActive)
    : Context(InContext), bActive(bInActive) {}

FExecutionBudgetContextExit::~FExecutionBudgetContextExit()
{
    if (!bActive) return;
    if (Provider.OnCompleted && Context->GetState() != asEXECUTION_FINISHED) Interval.bExecutionFailed = true;
    if (!Interval.bFailed) return;
    RetainExecutionBudgetUnwind(Context);
    Context->ExecutionBudgetCleanups = Interval.Cleanups;
}

bool BeginExecutionBudgetDestructorCleanup(asCScriptFunction* Function, void* Object)
{
    const TPair<asCScriptFunction*, void*> Key(Function, Object);
    if (Interval.Cleanups.Contains(Key)) return false;
    Interval.Cleanups.Add(Key);
    return true;
}

void RetainExecutionBudgetUnwind(asCContext* Context)
{
    if (Context->bExecutionBudgetUnwind) return;
    FWriteScopeLock Lock(PendingUnwindsLock);
    Context->bExecutionBudgetUnwind = true;
    PendingUnwinds.Add(Context);
}

void ReleaseExecutionBudgetUnwind(asCContext* Context)
{
    if (!Context->bExecutionBudgetUnwind) return;
    FWriteScopeLock Lock(PendingUnwindsLock);
    PendingUnwinds.Remove(Context);
    Context->bExecutionBudgetUnwind = false;
    Context->ExecutionBudgetCleanups.Reset();
}

bool EnterExecutionBudget(asCScriptFunction* Function)
{
    if (Interval.bFailed) return IsExecutionBudgetCleanup(Function);
    if (!Function) return PollExecutionBudget();
    const uint64 Budget = Query(Function->module);
    if (Budget)
    {
        RetainTarget(Function->module);
        if (Provider.OnCompleted)
        {
            const FModuleGenerationId Id {ANSI_TO_TCHAR(Function->module->baseModuleName.GetLength()
                ? Function->module->baseModuleName.AddressOf() : Function->module->GetName()), GetModuleGeneration(Function->module)};
            if (!Interval.CompletionModules.ContainsByPredicate([&](const auto& Existing) { return Existing.Generation == Id.Generation && Existing.ModuleName == Id.ModuleName; }))
                Interval.CompletionModules.Add(Id);
        }
    }
    if (Budget || Interval.Deadline)
    {
        bool bAllowed = ValidateExecutionBudgetFunction(Function, Budget && !Interval.Deadline);
        if (bAllowed && Budget && !Interval.Deadline)
        {
            if (Interval.EntryFunction) bAllowed = ValidateExecutionBudgetFunction(Interval.EntryFunction, false);
            auto* TLD = asCThreadManager::GetLocalData();
            auto* Context = TLD->activeContext;
            if (!Context && TLD->activeExecution) Context = TLD->activeExecution->prevContext;
            if (Context)
                for (asUINT I = 0; bAllowed && I < Context->GetCallstackSize(); ++I)
                    bAllowed = ValidateExecutionBudgetFunction(static_cast<asCScriptFunction*>(Context->GetFunction(I)), false);
        }
        if (!bAllowed)
        {
            Interval.Error = "Execution budget lifecycle dependency needs target compilation and cleanup facts.";
            // Before admission, ordinary caller cleanup keeps its original
            // policy. After admission every owned value has been audited.
            if (Interval.Deadline) Interval.bFailed = true;
            return false;
        }
    }
    if (Budget)
    {
        RetainTarget(Function->module);
        if (HasUninstrumentedJIT(Function) || MissingDestructorCleanup(Function))
            return Fail("Execution budget requires regenerated JIT with safe points.");
        const double Now = FPlatformTime::Seconds();
        const double Deadline = Now + double(Budget) / 1.e9;
        if (!Interval.Deadline || Deadline < Interval.Deadline)
        {
            Interval.Started = Now;
            Interval.Deadline = Deadline;
            Interval.Event.Module = {ANSI_TO_TCHAR(Function->module->GetName()), GetModuleGeneration(Function->module)};
            Interval.Event.BudgetNs = Budget;
        }
    }
    // Once a target enters, a non-target or nested context cannot reset the
    // interval. Native calls are cooperative: their first return safe point polls.
    if (Interval.Deadline && HasUninstrumentedJIT(Function))
        return Fail("Execution budget call chain contains uninstrumented JIT.");
    return PollExecutionBudget();
}

bool PollExecutionBudget()
{
    if (Interval.bFailed) return false;
    if (!Interval.Deadline) return true;
    const double Now = FPlatformTime::Seconds();
    if (Now < Interval.Deadline) return true;
    Fail("Script execution budget exceeded.");
    Interval.Event.ElapsedNs = uint64((Now - Interval.Started) * 1.e9);
    TGuardValue<bool> Guard(bInCallback, true);
    Provider.OnExceeded(Interval.Event);
    return false;
}

const ANSICHAR* ExecutionBudgetError()
{
    return Interval.Error ? Interval.Error : "Script execution budget exceeded.";
}

bool PollExecutionBudgetVM(asCContext* Context)
{
    const auto* Function = Context->m_currentFunction;
    const auto Offset = Function && Function->scriptData
        ? Context->m_regs.programPointer - Function->scriptData->byteCode.AddressOf() : -1;
    const bool bInTail = Function && Function->ExecutionBudgetCleanupOffset >= 0
        && (Offset >= Function->ExecutionBudgetCleanupOffset
            || (Function->ExecutionBudgetNormalTailOffset >= 0 && Offset >= Function->ExecutionBudgetNormalTailOffset));
    if (Interval.bFailed && bInTail) return true;
    if (PollExecutionBudget()) return true;
    RetainExecutionBudgetUnwind(Context);
    if (bInTail)
    {
        BeginExecutionBudgetDestructorCleanup(Context->m_currentFunction, *(void**)Context->m_regs.stackFramePointer);
        return true;
    }
    Context->SetException(ExecutionBudgetError());
    return false;
}

bool CheckExecutionBudgetJIT(FScriptExecution& Execution, asCScriptFunction* Function)
{
    if (!GExecutionBudgetRegistered.load(std::memory_order_acquire))
    {
        Fail("Budgeted JIT requires its execution budget provider; regenerate for off mode.");
        Execution.bExceptionThrown = true;
        FAngelscriptManager::HandleExceptionFromJIT(ExecutionBudgetError());
        return false;
    }
    if (!Interval.EntryFunction && Interval.Depth == 1) Interval.EntryFunction = Function;
    if (EnterExecutionBudget(Function))
        return !Execution.bExceptionThrown || (Interval.bFailed && IsExecutionBudgetCleanup(Function));
    if (!Execution.bExceptionThrown)
        FAngelscriptManager::HandleExceptionFromJIT(ExecutionBudgetError());
    Execution.bExceptionThrown = true;
    return false;
}

bool CheckExecutionBudgetRaw(FScriptExecution& Execution, void* RawFunction)
{
    FReadScopeLock Lock(RawFunctionsLock);
    auto* Function = RawFunctions.Find(RawFunction);
    // An unbound raw pointer cannot certify its target generation or safe points.
    if (Function) return CheckExecutionBudgetJIT(Execution, *Function);
    Fail("Execution budget cannot identify raw JIT; reload same-source generated code.");
    Execution.bExceptionThrown = true;
    FAngelscriptManager::HandleExceptionFromJIT(ExecutionBudgetError());
    return false;
}

bool EnterExecutionBudgetJITFrame(FScriptExecution& Execution, asCScriptFunction* Function)
{
    if (!CheckExecutionBudgetJIT(Execution, Function)) return false;
    // Generated recursion consumes the native stack, unlike VM recursion. A
    // deadline alone can lose to stack exhaustion, so budgeted JIT has a stricter
    // fixed guard. It is never emitted for ordinary/non-target JIT.
    if (Interval.JITDepth == 256 && !Interval.bFailed)
    {
        Fail("Stack overflow: execution budget JIT recursion limit (256 frames).");
        Interval.Event.Failure = EExecutionBudgetFailure::RecursionLimit;
        Interval.Event.ElapsedNs = Interval.Started ? uint64((FPlatformTime::Seconds() - Interval.Started) * 1.e9) : 0;
        {
            TGuardValue<bool> Guard(bInCallback, true);
            Provider.OnExceeded(Interval.Event);
        }
        return CheckExecutionBudgetJIT(Execution, nullptr);
    }
    ++Interval.JITDepth;
    return true;
}

void LeaveExecutionBudgetJITFrame()
{
    check(Interval.JITDepth != 0);
    --Interval.JITDepth;
}

bool ShouldInstrumentExecutionBudget(asCScriptFunction* Function)
{
    if (!GExecutionBudgetRegistered.load(std::memory_order_acquire) || !Function || !Query(Function->module)) return false;
    RetainTarget(Function->module);
    return true;
}

bool ValidateExecutionBudgetFunction(asCScriptFunction* Function, bool bCallClosure)
{
    TSet<asCTypeInfo*> Types;
    auto ValueType = [&](auto&& Self, asCTypeInfo* Info) -> bool
    {
        auto* Type = CastToObjectType(Info);
        if (!Type || !(Type->flags & asOBJ_VALUE) || Types.Contains(Type)) return true;
        Types.Add(Type);
        for (asUINT I = 0; I < Type->templateSubTypes.GetLength(); ++I)
        {
            const auto& Sub = Type->templateSubTypes[I];
            if (!Sub.IsReference() && !Sub.IsObjectHandle() && !Self(Self, Sub.GetTypeInfo())) return false;
        }
        if (!(Type->flags & asOBJ_SCRIPT_OBJECT)) return true;
        if (Type->beh.destruct)
        {
            auto* Destructor = Type->engine->scriptFunctions[Type->beh.destruct];
            if (!Destructor) return false;
            RestoreExecutionBudgetCleanup(Destructor);
            if (!Query(Destructor->module) || !IsExecutionBudgetCleanup(Destructor)) return false;
        }
        for (asUINT I = 0; I < Type->properties.GetLength(); ++I)
        {
            const auto& Member = Type->properties[I]->type;
            if (!Member.IsReference() && !Member.IsObjectHandle() && !Self(Self, Member.GetTypeInfo())) return false;
        }
        return true;
    };
    TArray<asCScriptFunction*> Pending { Function };
    TSet<asCScriptFunction*> Seen;
    while (!Pending.IsEmpty())
    {
        auto* Current = Pending.Pop(EAllowShrinking::No);
        if (!Current || Seen.Contains(Current)) continue;
        Seen.Add(Current);
        const bool bLifecycle = Current->traits.GetTrait(asTRAIT_CONSTRUCTOR) || Current->traits.GetTrait(asTRAIT_DESTRUCTOR)
            || (Current->name.GetLength() && Current->name[0] == '~');
        if (bLifecycle && Current->objectType)
        {
            if (!ValueType(ValueType, Current->objectType)) return false;
            for (asUINT I = 0; I < Current->objectType->properties.GetLength(); ++I)
            {
                const auto& Member = Current->objectType->properties[I]->type;
                if (!Member.IsReference() && !Member.IsObjectHandle() && !ValueType(ValueType, Member.GetTypeInfo())) return false;
            }
        }
        if (!Current->returnType.IsReference() && !Current->returnType.IsObjectHandle()
            && !ValueType(ValueType, Current->returnType.GetTypeInfo())) return false;
        for (asUINT I = 0; I < Current->parameterTypes.GetLength(); ++I)
        {
            const auto& Param = Current->parameterTypes[I];
            if (!Param.IsReference() && !Param.IsObjectHandle() && !ValueType(ValueType, Param.GetTypeInfo())) return false;
        }
        if (!Current->scriptData)
        {
            if (Current->funcType == asFUNC_SCRIPT && Current->ExecutionBudgetVersion != BudgetJITVersion) return false;
            continue;
        }
        for (asUINT I = 0; I < Current->scriptData->objVariableTypes.GetLength(); ++I)
            if (!ValueType(ValueType, Current->scriptData->objVariableTypes[I])) return false;
        if (!bCallClosure) continue;
        auto& Code = Current->scriptData->byteCode;
        for (asUINT I = 0; I < Code.GetLength(); I += asBCTypeSize[asBCInfo[*(asBYTE*)&Code[I]].type])
        {
            const auto Op = *(asBYTE*)&Code[I];
            if (Op == asBC_CALL || Op == asBC_CALLINTF)
                Pending.Add(Current->engine->scriptFunctions[asBC_INTARG(&Code[I])]);
            else if (Op == asBC_CALLSYS || Op == asBC_Thiscall1)
                Pending.Add((asCScriptFunction*)asBC_PTRARG(&Code[I]));
            else if (Op == asBC_ALLOC || Op == asBC_FREE)
            {
                if (!ValueType(ValueType, (asCTypeInfo*)asBC_PTRARG(&Code[I]))) return false;
            }
            else if (Op == asBC_CALLBND)
            {
                const int Id = Current->engine->importedFunctions[asBC_INTARG(&Code[I]) & ~FUNC_IMPORTED]->boundFunctionId;
                if (Id > 0) Pending.Add(Current->engine->scriptFunctions[Id]);
            }
        }
    }
    return true;
}

bool IsExecutionBudgetFailed() { return Interval.bFailed; }

bool IsExecutionBudgetCleanup(asCScriptFunction* Function)
{
    return Function && Function->ExecutionBudgetCleanupOffset >= 0
        && !HasUninstrumentedJIT(Function);
}

uint32 ExecutionBudgetEntryOffset(asCScriptFunction* Function)
{
    return Interval.bFailed && IsExecutionBudgetCleanup(Function) ? uint32(Function->ExecutionBudgetCleanupOffset) : 0u;
}

void AppendExecutionBudgetCleanup(asCScriptFunction* Function, int32 NormalTailOffset, const void* Code, uint32 Count)
{
    auto& ByteCode = Function->scriptData->byteCode;
    const uint32 Marker = ByteCode.GetLength();
    const uint32 Entry = Marker + 1 + AS_PTR_SIZE;
    ByteCode.SetLength(Entry + Count);
    ByteCode[Marker] = asBC_JitEntry;
    *(asPWORD*)&ByteCode[Marker + 1] = asPWORD(0x4353424700000000ull | uint32(NormalTailOffset + 1));
    const auto* Words = static_cast<const asDWORD*>(Code);
    for (uint32 I = 0; I < Count; ++I) ByteCode[Entry + I] = Words[I];
    Function->ExecutionBudgetNormalTailOffset = NormalTailOffset;
    Function->ExecutionBudgetCleanupOffset = Entry;
}

void RestoreExecutionBudgetCleanup(asCScriptFunction* Function)
{
    if (!Function->scriptData || Function->ExecutionBudgetCleanupOffset >= 0) return;
    auto& Code = Function->scriptData->byteCode;
    for (uint32 I = 0; I < Code.GetLength(); I += asBCTypeSize[asBCInfo[*(asBYTE*)&Code[I]].type])
    {
        if (*(asBYTE*)&Code[I] != asBC_JitEntry) continue;
        const uint64 Tag = asBC_PTRARG(&Code[I]);
        if ((Tag & 0xffffffff00000000ull) != 0x4353424700000000ull) continue;
        Function->ExecutionBudgetNormalTailOffset = int32(uint32(Tag)) - 1;
        Function->ExecutionBudgetCleanupOffset = I + 1 + AS_PTR_SIZE;
        return;
    }
}

void CompleteExecutionBudgetDestructor(asCContext* Context)
{
    auto* Function = Context->m_currentFunction;
    if (!Interval.bFailed || !IsExecutionBudgetCleanup(Function) || !Function->scriptData
        || !Context->m_regs.programPointer) return;
    const auto Offset = Context->m_regs.programPointer - Function->scriptData->byteCode.AddressOf();
    if (Offset >= Function->ExecutionBudgetCleanupOffset
        || (Function->ExecutionBudgetNormalTailOffset >= 0 && Offset >= Function->ExecutionBudgetNormalTailOffset)) return;
    auto* Cleanup = static_cast<asCContext*>(Function->engine->CreateContext());
    Cleanup->Prepare(Function);
    Cleanup->SetObject(*(void**)Context->m_regs.stackFramePointer);
    Cleanup->Execute();
    Cleanup->Unprepare();
    Cleanup->Release();
}

void BindExecutionBudgetJIT(asCScriptFunction* Function)
{
    if (GExecutionBudgetRegistered.load(std::memory_order_acquire) && Function->jitFunction_Raw)
    {
        FWriteScopeLock Lock(RawFunctionsLock);
        RawFunctions.Add((void*)Function->jitFunction_Raw, Function);
    }
}

void ForgetExecutionBudgetFunction(asCScriptFunction* Function)
{
    if (!GExecutionBudgetRegistered.load(std::memory_order_acquire) || !Function->jitFunction_Raw) return;
    FWriteScopeLock Lock(RawFunctionsLock);
    auto* Entry = RawFunctions.Find((void*)Function->jitFunction_Raw);
    if (Entry && *Entry == Function) RawFunctions.Remove((void*)Function->jitFunction_Raw);
}

void ForgetExecutionBudgetModule(asCModule* Module)
{
    if (Module->bExecutionBudgetTarget.exchange(false, std::memory_order_relaxed))
        TargetModules.fetch_sub(1, std::memory_order_relaxed);
    FModuleGenerationId Identity;
    if (BoundGenerations.RemoveAndCopyValue(Module, Identity))
    {
        Private::FNativeCompileCallbackScope NoNativePermit(nullptr);
        TGuardValue<bool> Guard(bInCallback, true);
        Provider.ReleaseGeneration(Identity);
    }
}

bool BindExecutionBudgetGeneration(const TSharedRef<FAngelscriptModuleDesc>& Desc, FString& Error)
{
    if (!GExecutionBudgetRegistered.load(std::memory_order_acquire) || !Provider.BindGeneration) return true;
    auto* Module = static_cast<asCModule*>(Desc->ScriptModule);
    if (!IsInGameThread() || bInCallback || !Module || BoundGenerations.Contains(Module)) return false;
    const FModuleGenerationId Identity {Desc->ModuleName, GetModuleGeneration(Module)};
    Private::FNativeCompileCallbackScope NoNativePermit(nullptr);
    TGuardValue<bool> Guard(bInCallback, true);
    if (!Provider.BindGeneration(Identity, *Desc, Error)) return false;
    BoundGenerations.Add(Module, Identity);
    return true;
}

bool ValidateExecutionBudgetModules(TConstArrayView<TSharedRef<FAngelscriptModuleDesc>> Modules, FString& Error)
{
    if (!GExecutionBudgetRegistered.load(std::memory_order_acquire)) return true;
    for (const auto& Desc : Modules)
    {
        auto* Module = static_cast<asCModule*>(Desc->ScriptModule);
        if (!Module || !Query(Module)) continue;
        RetainTarget(Module);
        for (asUINT I = 0; I < Module->scriptFunctions.GetLength(); ++I)
        {
            if (!Module->scriptFunctions[I]) continue;
            if (HasUninstrumentedJIT(Module->scriptFunctions[I]) || MissingDestructorCleanup(Module->scriptFunctions[I])
                || !ValidateExecutionBudgetFunction(Module->scriptFunctions[I], true))
            {
                Error = TEXT("Execution budget requires instrumented JIT and target lifecycle cleanup facts before globals.");
                return false;
            }
        }
    }
    return true;
}

void ShutdownExecutionBudget()
{
    check(IsAngelscriptExecutionWriterHeldByCurrentThread() && IsAngelscriptExecutionShutdownStarted());
    TArray<asCContext*> Drain;
    {
        FReadScopeLock Lock(PendingUnwindsLock);
        for (auto* Context : PendingUnwinds)
        {
            Context->AddRef();
            Drain.Add(Context);
        }
    }
    for (auto* Context : Drain)
    {
        Context->Unprepare();
        Context->Release();
    }
    GExecutionBudgetRegistered.store(false, std::memory_order_release);
    if (Provider.ReleaseGeneration)
    {
        TGuardValue<bool> Guard(bInCallback, true);
        for (const auto& Entry : BoundGenerations) Provider.ReleaseGeneration(Entry.Value);
        BoundGenerations.Empty();
    }
    {
        FWriteScopeLock Lock(RawFunctionsLock);
        RawFunctions.Empty();
    }
    Provider = {};
    ProviderHandle.Reset();
    bStopped = true;
}
}
}
                                                                                                
