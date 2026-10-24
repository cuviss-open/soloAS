// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptClassState.h"

#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptManager.h"
#include "ClassGenerator/ASClass.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Runtime/SoloASBridgeClassState.h"
#include "UObject/UObjectIterator.h"

namespace
{
uint8 GAngelscriptClassStateOwner;

const void* GetAngelscriptClassStateOwner()
{
	return &GAngelscriptClassStateOwner;
}

asITypeInfo* GetExternalAngelscriptClassScriptType(const UClass* Class)
{
	return static_cast<asITypeInfo*>(
		SoloASBridge::GetClassState(Class, GetAngelscriptClassStateOwner())
	);
}
}

asITypeInfo* GetDirectAngelscriptClassScriptType(const UClass* Class)
{
	if (Class == nullptr)
	{
		return nullptr;
	}

	if (const UASClass* ScriptClass = Cast<UASClass>(Class))
	{
		return ScriptClass->ScriptTypePtr;
	}

	return GetExternalAngelscriptClassScriptType(Class);
}

asITypeInfo* GetAngelscriptClassScriptType(const UClass* Class)
{
	if (Class == nullptr)
	{
		return nullptr;
	}

	if (asITypeInfo* DirectType = GetDirectAngelscriptClassScriptType(Class))
	{
		return DirectType;
	}

// CUVISS STUDIO EXTENSION BEGIN: CS.Runtime.ClassState.BlueprintAncestryFallback
	// UASClass derives from UClass, so the Blueprint metaclass guard below
	// also excludes cleared or retired direct script carriers.
	// Haze stores the AS type on BlueprintGeneratedClass identities as an Engine
	// field. Stock UE has no such field, so recover the same identity from the
	// nearest AS class when a generated Blueprint has no direct sidecar record.
	if (Class->GetClass() == nullptr
		|| !Class->GetClass()->IsChildOf(UBlueprintGeneratedClass::StaticClass()))
	{
		return nullptr;
	}

	for (const UClass* Cursor = Class->GetSuperClass(); Cursor != nullptr;
		Cursor = Cursor->GetSuperClass())
	{
		if (asITypeInfo* InheritedType =
			GetDirectAngelscriptClassScriptType(Cursor))
		{
			return InheritedType;
		}
		// A retired nearest script class must not expose a more distant identity.
		if (Cast<UASClass>(Cursor) != nullptr)
			return nullptr;
	}

	return nullptr;
// CUVISS STUDIO EXTENSION END: CS.Runtime.ClassState.BlueprintAncestryFallback
}

bool SetAngelscriptClassScriptType(UClass* Class, asITypeInfo* ScriptType)
{
	if (Class == nullptr || ScriptType == nullptr)
	{
		return false;
	}

	CheckAngelscriptExecutionMutationAllowed(
		!FAngelscriptManager::bIsInitialCompileFinished
	);

	if (UASClass* ScriptClass = Cast<UASClass>(Class))
	{
		ScriptClass->ScriptTypePtr = ScriptType;
		return true;
	}

	return SoloASBridge::SetClassState(
		Class,
		GetAngelscriptClassStateOwner(),
		ScriptType
	);
}

bool ClearAngelscriptClassScriptType(
	UClass* Class,
	const asITypeInfo* ExpectedScriptType
)
{
	if (Class == nullptr || ExpectedScriptType == nullptr)
	{
		return false;
	}

	CheckAngelscriptExecutionMutationAllowed(
		!FAngelscriptManager::bIsInitialCompileFinished
	);

	if (UASClass* ScriptClass = Cast<UASClass>(Class))
	{
		if (ScriptClass->ScriptTypePtr != ExpectedScriptType)
		{
			return false;
		}

		ScriptClass->ScriptTypePtr = nullptr;
		return true;
	}

	return SoloASBridge::ClearClassState(
		Class,
		GetAngelscriptClassStateOwner(),
		ExpectedScriptType
	);
}

void ClearAngelscriptClassScriptTypes()
{
	CheckAngelscriptExecutionMutationAllowed(
		!FAngelscriptManager::bIsInitialCompileFinished
	);

	SoloASBridge::ClearClassStates(GetAngelscriptClassStateOwner());

	for (UASClass* ScriptClass : TObjectRange<UASClass>())
	{
		ScriptClass->ScriptTypePtr = nullptr;
	}
}
