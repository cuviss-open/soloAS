// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptClassLifecycleAdapter.h"

#include "ClassGenerator/ASClass.h"
#include "ClassGenerator/AngelscriptRuntimeGcMirror.h"

#include "StartAngelscriptHeaders.h"
#include "as_scriptengine.h"
#include "EndAngelscriptHeaders.h"

#include "UObject/Object.h"

namespace
{
bool IsValidScriptObject(
	UObject* Object,
	UASClass* ScriptClass
)
{
	if (!IsValid(Object))
	{
		return false;
	}

	return ScriptClass != nullptr
		&& !Object->IsUnreachable()
		&& !Object->HasAnyFlags(
			RF_BeginDestroyed
			| RF_FinishDestroyed
			| RF_ClassDefaultObject
			| RF_ArchetypeObject
		)
		&& Object->GetClass() == ScriptClass;
}

bool IsValidScriptType(
	UASClass* ScriptClass,
	asITypeInfo* TypeCookie
)
{
	return ScriptClass != nullptr
		&& TypeCookie != nullptr
		&& TypeCookie->GetUserData() == ScriptClass;
}

bool IsValidLifecycleIdentity(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* TypeCookie
)
{
	return IsValidScriptObject(Object, ScriptClass)
		&& IsValidScriptType(ScriptClass, TypeCookie);
}
}

namespace AngelscriptClassLifecycleAdapter
{
bool TrackScriptObject(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* TypeCookie
)
{
	if (!IsValid(Object))
	{
		return false;
	}
	if (!IsValidLifecycleIdentity(Object, ScriptClass, TypeCookie))
	{
		return false;
	}
	if (TypeCookie != ScriptClass->ScriptTypePtr)
	{
		return false;
	}

	return AngelscriptRuntimeGcMirror::TrackRuntimeObjectWithMirrorForLifecycle(
		Object,
		ScriptClass,
		TypeCookie
	);
}

bool ReconstructScriptObject(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* OldTypeCookie,
	asITypeInfo* NewTypeCookie
)
{
	if (!IsValidLifecycleIdentity(Object, ScriptClass, OldTypeCookie)
		|| !IsValidLifecycleIdentity(Object, ScriptClass, NewTypeCookie)
		|| NewTypeCookie != ScriptClass->ScriptTypePtr)
	{
		return false;
	}

	// UASClass owns the tracked-cookie protocol and fail-closed reconstruction
	// sequence. This is caller-owned lifecycle mirror evidence; it does not
	// assert transparent UClass, REINST, CDO, or archetype parity.
	// The caller explicitly chooses when this operation is invoked.
	ScriptClass->ReconstructScriptObject(
		Object,
		OldTypeCookie,
		NewTypeCookie
	);
	return true;
}

bool DestroyTrackedScriptObject(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* TypeCookie
)
{
	if (!IsValidLifecycleIdentity(Object, ScriptClass, TypeCookie))
	{
		return false;
	}

	// This lifecycle-only path checks the caller cookie against the tracked
	// mirror. The automatic/internal destruction path intentionally retains its
	// existing object-only call semantics:
	// UASClass::DestroyTrackedRuntimeObjectExplicitly(Object);
	return AngelscriptRuntimeGcMirror::DestroyTrackedRuntimeObjectExplicitlyForLifecycle(
		Object,
		ScriptClass,
		TypeCookie
	);
}

bool UntrackScriptObject(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* TypeCookie
)
{
	if (!IsValid(Object))
	{
		return false;
	}
	if (!IsValidLifecycleIdentity(Object, ScriptClass, TypeCookie))
	{
		return false;
	}

	return AngelscriptRuntimeGcMirror::UntrackRuntimeObjectWithMirrorForLifecycle(
		Object,
		ScriptClass,
		TypeCookie
	);
}
}
