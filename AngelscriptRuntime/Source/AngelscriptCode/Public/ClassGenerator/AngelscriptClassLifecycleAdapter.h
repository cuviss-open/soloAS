// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UASClass;
class UObject;
class asITypeInfo;

/**
 * Explicit caller-owned operations for an externally tracked AngelScript class object.
 *
 * The seam accepts only an ordinary reachable, non-CDO, non-Archetype, and
 * non-destroying object with the exact UASClass and AngelScript type-cookie
 * identity. Callers own the game-thread, execution-writer, quiescence, and
 * shutdown preconditions, as well as construction/default execution and the
 * decision to invoke reconstruction. Track is the entry point for an external
 * object that has never been registered: its project-private transaction
 * prepares a Pending RuntimeGcMirror entry, calls SoloASBridge, and removes the
 * Pending entry if Bridge rejects the track. Untrack transitions Pending to
 * Untracking without nesting the Bridge and mirror mutexes; after a successful
 * Bridge removal, a mirror-removal failure attempts Bridge re-track and Pending
 * recovery, and calls FailRuntimeMirror if that rollback cannot restore both
 * sides. Reconstruct, Destroy, and Untrack must not be combined with normal
 * construction registration. Destroy delegates script teardown; Untrack is
 * only for a caller that has already completed it. The bool result reports
 * dispatch/validation acceptance only: because the underlying UASClass
 * operations are void, it is not a postcondition or an internal failure
 * report. This is not transparent UClass/CoreUObject lifecycle parity, does
 * not use private engine hooks, and does not provide automatic GC or
 * reinstancing.
 */
namespace AngelscriptClassLifecycleAdapter
{
ANGELSCRIPTCODE_API bool TrackScriptObject(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* TypeCookie
);

ANGELSCRIPTCODE_API bool ReconstructScriptObject(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* OldTypeCookie,
	asITypeInfo* NewTypeCookie
);

ANGELSCRIPTCODE_API bool DestroyTrackedScriptObject(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* TypeCookie
);

ANGELSCRIPTCODE_API bool UntrackScriptObject(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* TypeCookie
);
}
