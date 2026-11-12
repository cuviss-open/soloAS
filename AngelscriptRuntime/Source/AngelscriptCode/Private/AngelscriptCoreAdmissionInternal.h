// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "AngelscriptCoreAdmission.h"
namespace AngelscriptCoreExtensionPoints::Private
{
                                                                                               
bool HasLoadedSourceValidator();
bool ValidateLoadedSource(FStringView AbsolutePath, FStringView RelativePath,
    FStringView LoadedText, bool bDeleted, FString& Error);
                                                                                             
                                                                                                  
bool ValidateModuleAdmission(TConstArrayView<TSharedRef<FAngelscriptModuleDesc>> Candidates,
    FString& Error);
                                                                                                
}

                                                                                              
#include "AngelscriptCoreExtensionPoints.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include <atomic>
class asIScriptFunction;
class asIScriptModule;
class asITypeInfo;
struct FAngelscriptTypeUsage;
class UStruct;
namespace AngelscriptCoreExtensionPoints::Private
{
struct FDebugAccessState
{
    FCriticalSection Mutex;
    TSharedPtr<TFunction<bool(const FDebugAccessView&)>, ESPMode::ThreadSafe> Provider;
    FDelegateHandle Handle;
    std::atomic<bool> Enabled{false};
    uint64 Epoch = 0;
    uint32 Calls = 0;
    uint32 Readers = 0;
    uint32 Servers = 0;
    bool bClosing = false;

    FDelegateHandle Register(TFunction<bool(const FDebugAccessView&)> Allow)
    {
        FScopeLock Lock(&Mutex);
        if (!Allow || Provider || Servers || Calls || Readers || bClosing) return {};
        Provider = MakeShared<TFunction<bool(const FDebugAccessView&)>, ESPMode::ThreadSafe>(MoveTemp(Allow));
        Handle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
        ++Epoch;
        Enabled.store(true, std::memory_order_release);
        return Handle;
    }
    ERemoveResult Remove(FDelegateHandle InHandle)
    {
        TSharedPtr<TFunction<bool(const FDebugAccessView&)>, ESPMode::ThreadSafe> Released;
        {
            FScopeLock Lock(&Mutex);
            if (!InHandle.IsValid() || InHandle != Handle) return ERemoveResult::Unknown;
            if (Servers || Calls || Readers) return ERemoveResult::Busy;
            Enabled.store(false, std::memory_order_release);
            Released = MoveTemp(Provider);
            Handle.Reset();
            ++Epoch;
        }
        return ERemoveResult::Removed;
    }
    bool Allow(const FDebugAccessView& View)
    {
        if (!Enabled.load(std::memory_order_acquire)) return true;
        TSharedPtr<TFunction<bool(const FDebugAccessView&)>, ESPMode::ThreadSafe> Borrow;
        {
            FScopeLock Lock(&Mutex);
            if (bClosing || !Provider) return false;
            if ((View.Scope != EDebugAccessScope::Script && View.Scope != EDebugAccessScope::Native && View.Scope != EDebugAccessScope::Global) || View.Phase > EDebugAccessPhase::Publish || View.SessionId == 0
                || View.Operation >= EDebugAccessOperation::Count
                || (View.Scope == EDebugAccessScope::Script
                    && (View.ModuleGenerationId.Generation == 0 || View.ModuleGenerationId.ModuleName.IsEmpty() || View.SourceIdentity.IsEmpty()))) return false;
            Borrow = Provider;
            ++Calls;
        }
        const bool bAllowed = (*Borrow)(View);
        Borrow.Reset();
        {
            FScopeLock Lock(&Mutex);
            --Calls;
        }
        return bAllowed;
    }
};
FDebugAccessState& DebugAccess();
struct FDebugReadScope
{
    FDebugAccessView View;
    FDebugReadScope* Previous = nullptr;
    TArray<FDebugAccessView> Reads;
    explicit FDebugReadScope(const FDebugAccessView& InView);
    ~FDebugReadScope();
    FDebugReadScope(const FDebugReadScope&) = delete;
    FDebugReadScope& operator=(const FDebugReadScope&) = delete;
};
bool AllowDebugRead(const FDebugAccessView& View);
bool AllowDebugFunction(asIScriptFunction* Function, bool bExecute = false);
bool AllowDebugType(asITypeInfo* Type);
bool AllowDebugUsage(const FAngelscriptTypeUsage& Usage);
bool AllowDebugOwner(UStruct* Owner);
bool AllowDebugProperty(asITypeInfo* Type, int PropertyIndex);
bool AllowDebugModule(asIScriptModule* Module);
FDebugAccessView DebugFunctionView(asIScriptFunction* Function, const FDebugAccessView& Request);
FDebugAccessView DebugTypeView(asITypeInfo* Type, const FDebugAccessView& Request);
FDebugReadScope* CurrentDebugRead();
}
                                                                                            
