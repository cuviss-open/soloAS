// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UFunction;
struct FASBindFunctionPointers;
                                                                                                 
struct asFunctionCaller;

ANGELSCRIPTCODE_API bool IsAngelscriptCallableDispatchCaller(const asFunctionCaller& Caller);
                                                                                               

/**
 * Builds stable AngelScript call pointers for a reflected native function.
 *
 * Ordinary game-thread calls resolve the canonical owner path and name again.
 * Int32-only static native calls also capture a worker entry for the binding
 * epoch. Registration runs on the game thread and drains execution readers
 * before replacing that entry. Native code and layout must outlive the epoch;
 * an in-place native reconstruction requires an explicit binding rebuild.
 * Re-registration with a changed declaration is rejected without replacing
 * the existing snapshot.
 *
 * ProcessEvent marshaling is copy based. Functions requiring reference-return
 * identity or aliased parameter identity are deliberately rejected.
 */
ANGELSCRIPTCODE_API bool RegisterAngelscriptCallableDispatch(
	const UFunction* Function,
	FASBindFunctionPointers& OutFunctionPointers
);

/**
 * Retires all bind-epoch dispatch records.
 *
 * The caller must first stop new AngelScript execution and acquire the
 * execution write scope, which drains every active caller.
 */
ANGELSCRIPTCODE_API void ShutdownAngelscriptCallableDispatch();

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
/**
 * Explicit producer assertion: these exact native static thunk bodies and their
 * helpers were reviewed to support null Context/Stack.Object. FUNC_Static alone
 * is not that proof. Only already bound, live records may be qualified.
 * GT/write-scope registration is transactional and starts disabled.
 */
ANGELSCRIPTCODE_API bool QualifyContextIndependentStaticNativeCallablesForTest(
    TConstArrayView<const UFunction*> Functions);
ANGELSCRIPTCODE_API void ClearContextIndependentStaticNativeQualificationsForTest();
#endif
