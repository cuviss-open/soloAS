// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "Extensions/NativeEventSuper/AngelscriptNativeEventSuperHost.h"

#include "AngelscriptManager.h"
#include "Extensions/NativeEventSuper/AngelscriptNativeEventSuperInternal.h"
#include "Runtime/NativeEventSuper/SoloASBridgeNativeEventSuper.h"

#include "StartAngelscriptHeaders.h"
#include "as_generic.h"
#include "EndAngelscriptHeaders.h"

namespace Cuviss::NativeEventSuper
{
namespace
{
const ANSICHAR* BridgeDiagnostic(
	ESoloASBridgeNativeEventSuperResult Result)
{
	switch (Result)
	{
	case ESoloASBridgeNativeEventSuperResult::Succeeded:
		return "Native Blueprint event super bridge result: Succeeded.";
	case ESoloASBridgeNativeEventSuperResult::InvalidRequest:
		return "Native Blueprint event super bridge result: InvalidRequest.";
	case ESoloASBridgeNativeEventSuperResult::OffGameThread:
		return "Native Blueprint event super bridge result: OffGameThread.";
	case ESoloASBridgeNativeEventSuperResult::CallerClassNotFound:
		return "Native Blueprint event super bridge result: CallerClassNotFound.";
	case ESoloASBridgeNativeEventSuperResult::NativeClassNotFound:
		return "Native Blueprint event super bridge result: NativeClassNotFound.";
	case ESoloASBridgeNativeEventSuperResult::CallerFunctionNotFound:
		return "Native Blueprint event super bridge result: CallerFunctionNotFound.";
	case ESoloASBridgeNativeEventSuperResult::NativeFunctionNotFound:
		return "Native Blueprint event super bridge result: NativeFunctionNotFound.";
	case ESoloASBridgeNativeEventSuperResult::CallerOwnerInvalid:
		return "Native Blueprint event super bridge result: CallerOwnerInvalid.";
	case ESoloASBridgeNativeEventSuperResult::NativeClassInvalid:
		return "Native Blueprint event super bridge result: NativeClassInvalid.";
	case ESoloASBridgeNativeEventSuperResult::NativeAncestorNotFound:
		return "Native Blueprint event super bridge result: NativeAncestorNotFound.";
	case ESoloASBridgeNativeEventSuperResult::NativeAncestorMismatch:
		return "Native Blueprint event super bridge result: NativeAncestorMismatch.";
	case ESoloASBridgeNativeEventSuperResult::CallerFunctionShapeInvalid:
		return "Native Blueprint event super bridge result: CallerFunctionShapeInvalid.";
	case ESoloASBridgeNativeEventSuperResult::UnsupportedFunctionFlags:
		return "Native Blueprint event super bridge result: UnsupportedFunctionFlags.";
	case ESoloASBridgeNativeEventSuperResult::InterfaceRejected:
		return "Native Blueprint event super bridge result: InterfaceRejected.";
	case ESoloASBridgeNativeEventSuperResult::TargetRequired:
		return "Native Blueprint event super bridge result: TargetRequired.";
	case ESoloASBridgeNativeEventSuperResult::TargetTypeMismatch:
		return "Native Blueprint event super bridge result: TargetTypeMismatch.";
	case ESoloASBridgeNativeEventSuperResult::ParameterLayoutMismatch:
		return "Native Blueprint event super bridge result: ParameterLayoutMismatch.";
	case ESoloASBridgeNativeEventSuperResult::UnsupportedReferenceReturn:
		return "Native Blueprint event super bridge result: UnsupportedReferenceReturn.";
	case ESoloASBridgeNativeEventSuperResult::UnsupportedWritableObjectReference:
		return "Native Blueprint event super bridge result: UnsupportedWritableObjectReference.";
	case ESoloASBridgeNativeEventSuperResult::UnsupportedAliasLayout:
		return "Native Blueprint event super bridge result: UnsupportedAliasLayout.";
	case ESoloASBridgeNativeEventSuperResult::ParameterBufferInvalid:
		return "Native Blueprint event super bridge result: ParameterBufferInvalid.";
	case ESoloASBridgeNativeEventSuperResult::NativeImplementationMissing:
		return "Native Blueprint event super bridge result: NativeImplementationMissing.";
	default:
		return "Native Blueprint event super bridge result: Unknown.";
	}
}
}

void Dispatch(
	asIScriptGeneric* InGeneric,
	const FEventCallHost& Host)
{
	asCGeneric* Generic = static_cast<asCGeneric*>(InGeneric);
	const FAngelscriptNativeEventSuperAlias* Alias = Generic == nullptr
		? nullptr
		: ResolveAngelscriptNativeEventSuperAlias(Generic->GetFunction());
	UObject* Target = Generic == nullptr
		? nullptr
		: static_cast<UObject*>(Generic->GetObject());
	FString CallerClassPath;
	if (!IsInGameThread() || !IsValid(Target) || Alias == nullptr
		|| !Host.IsValid())
	{
		FAngelscriptManager::Throw(
			"Native Blueprint event super precondition failed.");
		return;
	}
	if (!ResolveNativeEventSuperCaller(*Alias, CallerClassPath))
	{
		FAngelscriptManager::Throw(
			"Native Blueprint event super caller resolution failed.");
		return;
	}
	if (!ValidateNativeEventSuperArgumentShape(*Alias))
	{
		FAngelscriptManager::Throw(
			"Native Blueprint event super signature exceeds event capacity.");
		return;
	}

	void* EventCall = Host.Acquire();
	check(EventCall != nullptr);
	for (int32 Arg = 0; Arg < Alias->ArgumentTypes.Num(); ++Arg)
	{
		FAngelscriptTypeUsage ArgumentType = Alias->ArgumentTypes[Arg];
		if (!Host.Push(
			EventCall,
			ArgumentType,
			Generic->GetAddressOfArg(Arg),
			true))
		{
			return;
		}
	}
	if (Alias->ReturnType.IsValid())
	{
		void* ReturnPtr = Generic->GetAddressOfReturnLocation();
		if (Alias->bInitializeReturn)
			Alias->ReturnType.ConstructValue(ReturnPtr);
		else if (Alias->bZeroReturnPointer)
			*static_cast<void**>(ReturnPtr) = nullptr;

		FAngelscriptTypeUsage ReturnType = Alias->ReturnType;
		if (!Host.Push(EventCall, ReturnType, &ReturnPtr, false))
			return;
	}

	Host.Prepare(EventCall);
	FSoloASBridgeNativeEventSuperRequest Request;
	Request.CallerScriptClassPath = *CallerClassPath;
	Request.NativeDeclaringClassPath = *Alias->NativeDeclaringClassPath;
	Request.FunctionName = FName(*Alias->NativeFunctionName);
	Request.Target = Target;
	Request.InitializedParameterBuffer = Host.Buffer(EventCall);
	Request.ParameterBufferCapacity = Host.BufferCapacity;
	Request.ParameterBufferAlignment = Host.BufferAlignment;
	const ESoloASBridgeNativeEventSuperResult Result =
		SoloASBridge::InvokeNativeEventSuper(Request);

	Host.Cleanup(EventCall);

	if (Result != ESoloASBridgeNativeEventSuperResult::Succeeded)
		FAngelscriptManager::Throw(BridgeDiagnostic(Result));
}
}
