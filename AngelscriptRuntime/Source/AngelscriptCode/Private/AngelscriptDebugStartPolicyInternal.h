                                                                                                   
// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "AngelscriptCoreExtensionPoints.h"
namespace AngelscriptCoreExtensionPoints::Private
{
// Core-owned decision state. No callback is invoked after Lock returns.
struct FDebugStartPolicyState
{
    enum class EPhase : uint8 { Open, Locked, Detached };
    EPhase Phase = EPhase::Open;
    FDelegateHandle Handle;
    TFunction<bool()> Deny;
    bool bDeciding = false;

    EDebugPolicyRegistration Register(TFunction<bool()> Provider, FDelegateHandle& Out)
    {
        Out.Reset();
        if (Phase != EPhase::Open) return EDebugPolicyRegistration::TooLate;
        if (Deny || !Provider || bDeciding) return EDebugPolicyRegistration::Conflict;
        Handle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
        Deny = MoveTemp(Provider);
        Out = Handle;
        return EDebugPolicyRegistration::Registered;
    }
    ERemoveResult Remove(FDelegateHandle InHandle)
    {
        if (!InHandle.IsValid() || InHandle != Handle) return ERemoveResult::Unknown;
        if (Phase == EPhase::Locked || bDeciding) return ERemoveResult::Busy;
        Deny = nullptr;
        Handle.Reset();
        return ERemoveResult::Removed;
    }
    bool Lock(bool bOriginalWouldStart)
    {
        check(IsInGameThread() && Phase == EPhase::Open);
        Phase = EPhase::Locked;
        TGuardValue<bool> Guard(bDeciding, true);
        return bOriginalWouldStart && (!Deny || !Deny());
    }
    void Detach() { check(!bDeciding); Phase = EPhase::Detached; }
};
FDebugStartPolicyState& DebugStartPolicy();
}
                                                                                                 
