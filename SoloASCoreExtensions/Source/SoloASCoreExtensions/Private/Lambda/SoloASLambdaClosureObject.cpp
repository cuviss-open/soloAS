// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "SoloASLambdaClosureObject.h"
#include "Lambda/SoloASBridgeLambdaClosure.h"
#include "Misc/ScopeExit.h"

void USoloASLambdaClosureObject::ProcessEvent(UFunction* Function, void* Parameters)
{
    if (!Slots.Contains(Function->GetFName())) { Super::ProcessEvent(Function, Parameters); return; }
    if (!ensureAlwaysMsgf(IsInGameThread(), TEXT("Lambda invocation requires game thread"))) return;
    ++ActiveInvocations;
    ON_SCOPE_EXIT { --ActiveInvocations; SoloASBridge::SweepLambdaClosures(); };
    SoloASBridge::DispatchLambdaFallback(this, Function, Parameters);
}
