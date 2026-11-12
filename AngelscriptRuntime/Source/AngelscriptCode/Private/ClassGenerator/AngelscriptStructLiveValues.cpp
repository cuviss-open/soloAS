                                                                                          
// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptStructLiveValues.h"

#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptInclude.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "UObject/UObjectThreadContext.h"
#include "UObject/UObjectGlobals.h"
#include "StartAngelscriptHeaders.h"
#include "as_objecttype.h"
#include "as_scriptfunction.h"
#include "EndAngelscriptHeaders.h"

namespace
{
struct FTypeState
{
	int32 Count = 0;
	bool bComplete = false;
};
FCriticalSection StateMutex;
TMap<const asITypeInfo*, FTypeState> Types;
TSet<uint64> Owners;
uint64 NextToken = 0;
}

namespace AngelscriptStructLiveValues
{
std::atomic<bool> Enabled{false};

void TypeCreated(const asITypeInfo* Type)
{
	FScopeLock Lock(&StateMutex);
	if (Enabled.load(std::memory_order_relaxed))
		Types.Add(Type);
}

void TypeDestroyed(const asITypeInfo* Type)
{
	FScopeLock Lock(&StateMutex);
	Types.Remove(Type);
}

void TypeCompiled(const asITypeInfo* Type)
{
	FScopeLock Lock(&StateMutex);
	if (FTypeState* State = Types.Find(Type))
		State->bComplete = true;
}

void Invalidate(const asITypeInfo* Type)
{
	FScopeLock Lock(&StateMutex);
	if (FTypeState* State = Types.Find(Type))
		State->bComplete = false;
}

void InvalidateModule(const asIScriptModule* Module)
{
	FScopeLock Lock(&StateMutex);
	for (auto& Pair : Types)
	{
		if (Pair.Key->GetModule() == Module)
			Pair.Value.bComplete = false;
	}
}

void Completed(const asITypeInfo* Type, bool bConstructed)
{
	if ((Type->GetFlags() & (asOBJ_VALUE | asOBJ_SCRIPT_OBJECT)) != (asOBJ_VALUE | asOBJ_SCRIPT_OBJECT))
		return;
	FScopeLock Lock(&StateMutex);
	if (!Enabled.load(std::memory_order_relaxed))
		return;
	FTypeState* State = Types.Find(Type);
	if (State == nullptr)
		return;
	// Origin alone cannot establish ownership of a value moved out of a
	// template's native container. Keep that type unknown until recompiled.
	const FObjectInitializer* Initializer = FUObjectThreadContext::Get().TopInitializer();
	if (Initializer != nullptr && Initializer->GetObj() != nullptr && Initializer->GetObj()->IsTemplate())
		State->bComplete = false;
	if (bConstructed)
	{
		if (State->Count == MAX_int32)
			State->bComplete = false;
		else
			++State->Count;
	}
	else if (State->Count > 0)
		--State->Count;
	else
		State->bComplete = false;
}
}

uint64 EnableAngelscriptStructLiveValues()
{
	check(IsInGameThread());
	FAngelscriptExecutionWriteScope Execution;
	FScopeLock Lock(&StateMutex);
	const uint64 Token = ++NextToken;
	Owners.Add(Token);
	AngelscriptStructLiveValues::Enabled.store(true, std::memory_order_release);
	return Token;
}

void DisableAngelscriptStructLiveValues(uint64 Token)
{
	check(IsInGameThread());
	FAngelscriptExecutionWriteScope Execution;
	FScopeLock Lock(&StateMutex);
	if (Owners.Remove(Token) == 0 || Owners.Num() != 0)
		return;
	AngelscriptStructLiveValues::Enabled.store(false, std::memory_order_release);
	Types.Empty();
}

bool QueryAngelscriptStructLiveValues(const asITypeInfo* Type, int32& OutExternalLiveCount)
{
	check(IsInGameThread());
	OutExternalLiveCount = -1;
	FScopeLock Lock(&StateMutex);
	if (!AngelscriptStructLiveValues::Enabled.load(std::memory_order_relaxed))
		return false;
	const FTypeState* State = Types.Find(Type);
	if (State == nullptr || !State->bComplete)
		return false;
	const auto* ObjectType = static_cast<const asCObjectType*>(Type);
	if ((ObjectType->flags & (asOBJ_VALUE | asOBJ_SCRIPT_OBJECT)) != (asOBJ_VALUE | asOBJ_SCRIPT_OBJECT)
		|| (ObjectType->flags & asOBJ_POD) || ObjectType->isInvalidGeneratedType
		|| ObjectType->beh.construct == 0 || ObjectType->beh.destruct == 0)
		return false;
	// Only source generations with retained lifecycle bodies are certified.
	for (int FunctionId : {ObjectType->beh.construct, ObjectType->beh.destruct, ObjectType->beh.copyconstruct})
	{
		if (FunctionId != 0)
		{
			const asCScriptFunction* Function = static_cast<const asCScriptFunction*>(Type->GetEngine()->GetFunctionById(FunctionId));
			if (Function == nullptr || Function->scriptData == nullptr || Function->jitFunction != nullptr)
				return false;
		}
	}
	// A global object/container can own nested values of this type. Without a
	// complete current owner graph it is unsafe to subtract its creation count.
	const asIScriptModule* Module = Type->GetModule();
	if (Module == nullptr)
		return false;
	for (asUINT Index = 0; Index < Module->GetGlobalVarCount(); ++Index)
	{
		int TypeId = 0;
		Module->GetGlobalVar(Index, nullptr, nullptr, &TypeId, nullptr);
		if ((TypeId & asTYPEID_MASK_OBJECT) == 0 || (TypeId & asTYPEID_OBJHANDLE))
			continue;
		const asITypeInfo* GlobalType = Type->GetEngine()->GetTypeInfoById(TypeId);
		if (GlobalType == nullptr)
			return false;
		const asDWORD Flags = GlobalType->GetFlags();
		// References do not own value storage. Native POD values, including the
		// generated TStructType identity, have no script-value destruction graph.
		if ((Flags & asOBJ_REF) || ((Flags & asOBJ_POD) && !(Flags & asOBJ_SCRIPT_OBJECT)))
			continue;
		// TSubclassOf instances contain a UClass reference, not script value storage.
		const asITypeInfo* SubclassTemplate = Type->GetEngine()->GetTypeInfoByName("TSubclassOf");
		if ((Flags & asOBJ_VALUE) && SubclassTemplate != nullptr
			&& static_cast<const asCObjectType*>(GlobalType)->templateBaseType == SubclassTemplate)
			continue;
		return false;
	}
	OutExternalLiveCount = State->Count;
	return true;
}
                                                                                        
