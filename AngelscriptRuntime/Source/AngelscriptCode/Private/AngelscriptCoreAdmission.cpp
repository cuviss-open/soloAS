// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptCoreAdmission.h"
#include "AngelscriptCoreAdmissionInternal.h"
                                                                                                          
#include "AngelscriptBinds.h"
                                                                                                        

namespace AngelscriptCoreExtensionPoints
{
                                                                                               
namespace
{
bool bInAdmissionCallback = false;
FLoadedSourceValidator LoadedSourceValidator;
FDelegateHandle LoadedSourceHandle;
}

FDelegateHandle RegisterLoadedSourceValidator(FLoadedSourceValidator Validator)
{
    if (!IsInGameThread() || bInAdmissionCallback || LoadedSourceValidator || !Validator) return {};
    LoadedSourceHandle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
    LoadedSourceValidator = MoveTemp(Validator);
    return LoadedSourceHandle;
}

ERemoveResult UnregisterLoadedSourceValidator(FDelegateHandle Handle)
{
    if (!IsInGameThread() || bInAdmissionCallback) return ERemoveResult::Busy;
    if (!Handle.IsValid() || Handle != LoadedSourceHandle) return ERemoveResult::Unknown;
    LoadedSourceValidator = nullptr;
    LoadedSourceHandle.Reset();
    return ERemoveResult::Removed;
}

bool Private::HasLoadedSourceValidator() { return !!LoadedSourceValidator; }

bool Private::ValidateLoadedSource(FStringView AbsolutePath, FStringView RelativePath,
    FStringView LoadedText, bool bDeleted, FString& Error)
{
    if (!LoadedSourceValidator) return true;
    if (!IsInGameThread() || bInAdmissionCallback)
    {
        Error = TEXT("Loaded source admission requires a non-reentrant game-thread call.");
        return false;
    }
    Private::FNativeCompileCallbackScope NoNativePermit(nullptr);
    TGuardValue<bool> Guard(bInAdmissionCallback, true);
    return LoadedSourceValidator(AbsolutePath, RelativePath, LoadedText, bDeleted, Error);
}
                                                                                             
                                                                                                  
namespace
{
FModuleAdmissionValidator ModuleAdmissionValidator;
FDelegateHandle ModuleAdmissionHandle;
}

FDelegateHandle RegisterModuleAdmission(FModuleAdmissionValidator Validator)
{
    if (!IsInGameThread() || bInAdmissionCallback || ModuleAdmissionValidator || !Validator) return {};
    ModuleAdmissionHandle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
    ModuleAdmissionValidator = MoveTemp(Validator);
    return ModuleAdmissionHandle;
}

ERemoveResult UnregisterModuleAdmission(FDelegateHandle Handle)
{
    if (!IsInGameThread() || bInAdmissionCallback) return ERemoveResult::Busy;
    if (!Handle.IsValid() || Handle != ModuleAdmissionHandle) return ERemoveResult::Unknown;
    ModuleAdmissionValidator = nullptr;
    ModuleAdmissionHandle.Reset();
    return ERemoveResult::Removed;
}

bool Private::ValidateModuleAdmission(TConstArrayView<TSharedRef<FAngelscriptModuleDesc>> Candidates,
    FString& Error)
{
    if (!ModuleAdmissionValidator) return true;
    if (!IsInGameThread() || bInAdmissionCallback)
    {
        Error = TEXT("Module admission requires a non-reentrant game-thread call.");
        return false;
    }
    Private::FNativeCompileCallbackScope NoNativePermit(nullptr);
    TGuardValue<bool> Guard(bInAdmissionCallback, true);
    return ModuleAdmissionValidator(Candidates, Error);
}
                                                                                                
}
