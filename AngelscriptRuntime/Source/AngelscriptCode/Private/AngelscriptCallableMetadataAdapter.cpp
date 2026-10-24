// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptCallableMetadataAdapter.h"

#include "Runtime/SoloASBridgeCallableDispatch.h"
#include "Shared/SoloASBridgeReflection.h"
#include "Runtime/SoloASBridgeScriptMetadata.h"
#include "UObject/Class.h"

namespace
{
bool MatchesAnsiName(const ANSICHAR* Candidate, const FString& Expected)
{
	return Candidate != nullptr
		&& Expected.Equals(ANSI_TO_TCHAR(Candidate), ESearchCase::CaseSensitive);
}

bool HasNativeScriptCallableTrait(const UFunction* Function)
{
	if (IsInGameThread())
	{
		ESoloASBridgeFunctionTrait RegisteredTraits =
			ESoloASBridgeFunctionTrait::None;
		if (SoloASBridge::GetFunctionTraits(Function, RegisteredTraits)
			&& EnumHasAnyFlags(
				RegisteredTraits,
				ESoloASBridgeFunctionTrait::ScriptCallable
			))
		{
			return true;
		}
	}

	const UClass* Owner = Function->GetOuterUClass();
	if (Owner == nullptr)
	{
		return false;
	}

	FString OwnerName(Owner->GetPrefixCPP());
	OwnerName += Owner->GetName();
	const FString FunctionName = Function->GetName();
	const auto Metadata = SoloASBridge::GetScriptMetadata();
	const auto& Descriptors = Metadata->FunctionTraits;
	int32 First = 0;
	int32 Last = Descriptors.Num();
	// UHT sorts these records by ordinal owner/function name.
	while (First < Last)
	{
		const int32 Middle = First + (Last - First) / 2;
		const auto& Descriptor = Descriptors[Middle];
		int32 Order = OwnerName.Compare(ANSI_TO_TCHAR(Descriptor.OwnerName), ESearchCase::CaseSensitive);
		if (Order == 0)
		{
			Order = FunctionName.Compare(ANSI_TO_TCHAR(Descriptor.FunctionName), ESearchCase::CaseSensitive);
		}
		if (Order == 0)
			return MatchesAnsiName(Descriptor.OwnerName, OwnerName)
				&& MatchesAnsiName(Descriptor.FunctionName, FunctionName)
				&& EnumHasAnyFlags(Descriptor.Traits, ESoloASBridgeFunctionTrait::ScriptCallable);
		if (Order < 0)
			Last = Middle;
		else
			First = Middle + 1;
	}
	return false;
}
}

bool IsAngelscriptScriptCallableFunction(const UFunction* Function)
{
	if (Function == nullptr || !SoloASBridge::CanInvokeReflectedCallable(Function))
		return false;

#if WITH_METADATA
	// Independently packaged plugins are absent from Bridge's UHT session.
	if (Function->HasMetaData(TEXT("ScriptCallable")))
		return true;
#endif

	return HasNativeScriptCallableTrait(Function);
}

