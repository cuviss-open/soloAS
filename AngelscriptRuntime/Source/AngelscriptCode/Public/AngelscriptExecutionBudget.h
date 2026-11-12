// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "AngelscriptCoreAdmission.h"
#include "AngelscriptModuleGeneration.h"
#include <atomic>
class asCModule;
class asCScriptFunction;
class asCContext;
struct FScriptExecution;

                                                                                                  
namespace AngelscriptCoreExtensionPoints
{
enum class EExecutionBudgetFailure : uint8 { DeadlineExceeded, RecursionLimit };
struct FExecutionBudgetEvent
{
    FModuleGenerationId Module;
    uint64 ElapsedNs = 0;
    uint64 BudgetNs = 0;
    EExecutionBudgetFailure Failure = EExecutionBudgetFailure::DeadlineExceeded;
};

// Callbacks run on the executing thread. They own immutable, thread-safe policy
// snapshots and must not touch UObjects, block, execute script or unload modules.
// A generation's target membership is fixed until it is unloaded; budgets may
// tighten at an entry safe point, but cannot extend an active interval.
struct FExecutionBudgetProvider
{
    TFunction<uint64(const FModuleGenerationId&)> QueryBudgetNs;
    TFunction<void(const FExecutionBudgetEvent&)> OnExceeded;
    // Optional: once per targeted generation at the outermost synchronous
    // execution interval's exit (nested VM/JIT entries share that interval).
    // OnExceeded precedes OnCompleted(false). False also covers ordinary script
    // exceptions/abort/suspension; correlate OnExceeded to count timeouts only.
    // Non-target execution and cleanup-only intervals emit no completion.
    // Same executing-thread/no-script/no-mutation restrictions as OnExceeded.
    TFunction<void(const FModuleGenerationId&, bool bSucceeded)> OnCompleted;
    // Optional paired GT lifecycle callbacks. Bind receives the actual staged
    // descriptor before the first compiler/JIT budget query. Source trust stays
    // with the provider; names alone never authenticate a candidate.
    TFunction<bool(const FModuleGenerationId&, const FAngelscriptModuleDesc&, FString&)> BindGeneration;
    TFunction<void(const FModuleGenerationId&)> ReleaseGeneration;
};
ANGELSCRIPTCODE_API FDelegateHandle RegisterExecutionBudget(FExecutionBudgetProvider Provider);
ANGELSCRIPTCODE_API ERemoveResult UnregisterExecutionBudget(FDelegateHandle Handle);

namespace Private
{
using FExecutionBudgetCleanupSet = TSet<TPair<asCScriptFunction*, void*>>;
// Runtime entry points only. Callers hold an execution read/write scope.
ANGELSCRIPTCODE_API extern std::atomic<bool> GExecutionBudgetRegistered;
ANGELSCRIPTCODE_API void BeginExecutionBudgetInterval();
ANGELSCRIPTCODE_API void EndExecutionBudgetInterval();
struct ANGELSCRIPTCODE_API FExecutionBudgetCleanupScope
{
    asCContext* Context = nullptr;
    bool bActive = false;
    bool bPreviousFailure = false;
    const ANSICHAR* PreviousError = nullptr;
    explicit FExecutionBudgetCleanupScope(asCContext* InContext);
    ~FExecutionBudgetCleanupScope();
};
struct ANGELSCRIPTCODE_API FExecutionBudgetContextExit
{
    asCContext* Context;
    bool bActive;
    FExecutionBudgetContextExit(asCContext* InContext, bool bInActive);
    ~FExecutionBudgetContextExit();
};
ANGELSCRIPTCODE_API bool BeginExecutionBudgetDestructorCleanup(asCScriptFunction* Function, void* Object);
ANGELSCRIPTCODE_API void RetainExecutionBudgetUnwind(asCContext* Context);
ANGELSCRIPTCODE_API void ReleaseExecutionBudgetUnwind(asCContext* Context);
struct ANGELSCRIPTCODE_API FExecutionBudgetInterval
{
    bool bAcquired = false;
    FExecutionBudgetInterval();
    ~FExecutionBudgetInterval();
    FExecutionBudgetInterval(const FExecutionBudgetInterval&) = delete;
    FExecutionBudgetInterval& operator=(const FExecutionBudgetInterval&) = delete;
};
ANGELSCRIPTCODE_API bool EnterExecutionBudget(asCScriptFunction* Function);
ANGELSCRIPTCODE_API bool PollExecutionBudget();
ANGELSCRIPTCODE_API bool PollExecutionBudgetVM(asCContext* Context);
ANGELSCRIPTCODE_API bool CheckExecutionBudgetJIT(FScriptExecution& Execution, asCScriptFunction* Function);
ANGELSCRIPTCODE_API bool EnterExecutionBudgetJITFrame(FScriptExecution& Execution, asCScriptFunction* Function);
ANGELSCRIPTCODE_API void LeaveExecutionBudgetJITFrame();
ANGELSCRIPTCODE_API bool CheckExecutionBudgetRaw(FScriptExecution& Execution, void* RawFunction);
ANGELSCRIPTCODE_API const ANSICHAR* ExecutionBudgetError();
bool ShouldInstrumentExecutionBudget(asCScriptFunction* Function);
void BindExecutionBudgetJIT(asCScriptFunction* Function);
void ForgetExecutionBudgetFunction(asCScriptFunction* Function);
void ForgetExecutionBudgetModule(asCModule* Module);
bool BindExecutionBudgetGeneration(const TSharedRef<FAngelscriptModuleDesc>& Module, FString& Error);
void AppendExecutionBudgetCleanup(asCScriptFunction* Function, int32 NormalTailOffset, const void* Code, uint32 Count);
void RestoreExecutionBudgetCleanup(asCScriptFunction* Function);
bool ValidateExecutionBudgetFunction(asCScriptFunction* Function, bool bCallClosure);
ANGELSCRIPTCODE_API bool IsExecutionBudgetFailed();
ANGELSCRIPTCODE_API bool IsExecutionBudgetCleanup(asCScriptFunction* Function);
ANGELSCRIPTCODE_API uint32 ExecutionBudgetEntryOffset(asCScriptFunction* Function);
ANGELSCRIPTCODE_API void CompleteExecutionBudgetDestructor(asCContext* Context);
bool ValidateExecutionBudgetModules(TConstArrayView<TSharedRef<FAngelscriptModuleDesc>> Modules, FString& Error);
void ShutdownExecutionBudget();
}
}
                                                                                                
