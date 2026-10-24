// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

#include "Extensions/NativeEventSuper/AngelscriptNativeEventSuper.h"
#include "AngelscriptType.h"

class asIScriptGeneric;
class UFunction;
struct FAngelscriptFunctionSignature;

namespace Cuviss::NativeEventSuper
{
struct FEventCallHost
{
	using FAcquire = void* (*)();
	using FPush = bool (*)(
		void*, FAngelscriptTypeUsage&, void*, bool);
	using FAction = void (*)(void*);
	using FBuffer = void* (*)(void*);

	FAcquire Acquire = nullptr;
	FPush Push = nullptr;
	FAction Prepare = nullptr;
	FBuffer Buffer = nullptr;
	FAction Cleanup = nullptr;
	SIZE_T BufferCapacity = 0;
	SIZE_T BufferAlignment = 0;

	bool IsValid() const
	{
		return Acquire != nullptr
			&& Push != nullptr
			&& Prepare != nullptr
			&& Buffer != nullptr
			&& Cleanup != nullptr
			&& BufferCapacity != 0
			&& BufferAlignment != 0;
	}
};

void Dispatch(
	asIScriptGeneric* Generic,
	const FEventCallHost& Host);

bool RegisterAlias(
	const FAngelscriptBlueprintEventBindingSnapshot& Binding);

void OnBlueprintEventBound(
	const FAngelscriptFunctionSignature& Signature,
	const FString& TargetScriptTypeName,
	UFunction* Function,
	const FAngelscriptTypeUsage* ArgumentTypes,
	int32 ArgumentCount,
	const FAngelscriptTypeUsage& ReturnType,
	bool bInitializeReturn,
	bool bZeroReturnPointer);

void CallNativeEventSuperWithSignature(asIScriptGeneric* Generic);

template <
	typename EventCallType,
	EventCallType& (*AcquireEventCall)()>
struct TEventCallHostAdapter
{
	static void* Acquire()
	{
		return &AcquireEventCall();
	}

	static bool Push(
		void* EventCall,
		FAngelscriptTypeUsage& Type,
		void* Value,
		bool bCopyInitialValue)
	{
		EventCallType& Call = *static_cast<EventCallType*>(EventCall);
		const int32 ExpectedArgumentIndex = Call.ArgumentIndex + 1;
		if (bCopyInitialValue)
			Call.template PushArgument<true, true>(Type, Value);
		else
			Call.template PushArgument<true, false>(Type, Value);
		return Call.ArgumentIndex == ExpectedArgumentIndex;
	}

	static void Prepare(void* EventCall)
	{
		static_cast<EventCallType*>(EventCall)->ExecutePreamble();
	}

	static void* Buffer(void* EventCall)
	{
		return &static_cast<EventCallType*>(EventCall)->ArgumentBuffer[0];
	}

	static void Cleanup(void* EventCall)
	{
		static_cast<EventCallType*>(EventCall)->ExecuteCleanup();
	}

	static const FEventCallHost& Get()
	{
		static const FEventCallHost Host = {
			&Acquire,
			&Push,
			&Prepare,
			&Buffer,
			&Cleanup,
			sizeof(((EventCallType*)nullptr)->ArgumentBuffer),
			alignof(EventCallType),
		};
		return Host;
	}
};

template <
	typename EventCallType,
	EventCallType& (*AcquireEventCall)()>
void DispatchWithHost(asIScriptGeneric* Generic)
{
	Dispatch(
		Generic,
		TEventCallHostAdapter<EventCallType, AcquireEventCall>::Get());
}
}
