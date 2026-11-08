// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once

#include "CoreMinimal.h"

class asITypeInfo;
struct FAngelscriptType;
class UClass;
class UFunction;
struct FAngelscriptClassBind;
struct FAngelscriptBlueprintEventBindingSnapshot;
struct FAngelscriptMethodBind;
struct FAngelscriptNativeInterfaceSelectionContext;

namespace AngelscriptNativeInterfaceBinding
{
using FTypeRegistrar = TFunctionRef<void(UClass*, const FString&)>;
using FEventDispatcher = void (*)(
	TSharedRef<FAngelscriptType>,
	UFunction*,
	const FAngelscriptBlueprintEventBindingSnapshot&,
    TMap<UClass*, TMap<FString, UFunction*>>&);


asITypeInfo* ReferenceType(
	UClass* InterfaceClass,
	const FString& TypeName);

void BindClosure(
	UClass* SelectedRoot,
	const FString& SelectedRootTypeName,
	FTypeRegistrar RegisterType);

bool PrepareDatabaseClass(
	UClass* Class,
	TFunctionRef<bool(UClass*)> ResolveDatabaseClass);

bool BindDatabaseMethodsIfInterface(
	UClass* Class,
	const TSharedPtr<FAngelscriptType>& Type,
	const FAngelscriptNativeInterfaceSelectionContext& Context,
	FEventDispatcher DispatchEvent,
    TMap<UClass*, TMap<FString, UFunction*>>& EventsByScriptName);

bool IsInterface(UClass* Class);

bool BindRuntimeMethodsIfInterface(
	UClass* Class,
	TSharedRef<FAngelscriptType> Type,
	const FAngelscriptNativeInterfaceSelectionContext& Context,
	FEventDispatcher DispatchEvent,
    TMap<UClass*, TMap<FString, UFunction*>>& EventsByScriptName);

bool FinalizeRuntimeInterfaceRecord(
	UClass* Class,
	TSharedRef<FAngelscriptType> Type,
	TFunctionRef<void()> FinalizeRecord);
}
