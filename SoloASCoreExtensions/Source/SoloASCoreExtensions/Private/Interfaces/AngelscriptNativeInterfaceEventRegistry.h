// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once

#include "CoreMinimal.h"

class UClass;
class UFunction;
struct FAngelscriptType;
struct FAngelscriptBlueprintEventBindingSnapshot;

namespace AngelscriptNativeInterfaceEventRegistry
{
using FEventMap = TMap<UClass*, TMap<FString, UFunction*>>;

void Register(
	FEventMap& EventsByScriptName,
	UFunction* Function,
	const FString& ScriptName);

UFunction* FindInterface(
	const FEventMap& EventsByScriptName,
	UClass* Class,
	const FString& ScriptName);

void DispatchBlueprintEvent(
	TSharedRef<FAngelscriptType> Type,
	UFunction* Function,
	const FAngelscriptBlueprintEventBindingSnapshot& Signature,
    FEventMap& EventsByScriptName);
}
