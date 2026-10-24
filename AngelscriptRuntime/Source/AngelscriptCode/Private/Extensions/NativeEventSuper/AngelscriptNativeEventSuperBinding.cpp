// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "Extensions/NativeEventSuper/AngelscriptNativeEventSuperHost.h"

#include "AngelscriptBinds.h"
#include "Extensions/AngelscriptExtensionHooks.h"
#include "Extensions/NativeEventSuper/AngelscriptNativeEventSuper.h"
#include "Extensions/NativeEventSuper/AngelscriptNativeEventSuperInternal.h"
#include "AngelscriptManager.h"
#include "Binds/Helper_FunctionSignature.h"

#include "StartAngelscriptHeaders.h"
#include "angelscript.h"
#include "as_scriptfunction.h"
#include "EndAngelscriptHeaders.h"

#include "UObject/Class.h"

namespace Cuviss::NativeEventSuper
{
bool RegisterAlias(
	const FAngelscriptBlueprintEventBindingSnapshot& Binding)
{
	if (Binding.Target != EAngelscriptBlueprintEventTarget::Method
		|| Binding.TargetScriptTypeName.IsEmpty()
		|| Binding.Declaration.IsEmpty()
		|| Binding.ScriptName.IsEmpty()
		|| Binding.NativeDeclaringClassPath.IsEmpty()
		|| Binding.NativeFunctionName.IsNone()
		|| !ValidateNativeEventSuperArgumentShape(
			Binding.ArgumentTypes,
			Binding.ReturnType,
			Binding.ArgumentTypes.Num()
				+ (Binding.ReturnType.IsValid() ? 1 : 0)))
	{
		return false;
	}

	const FString DeclarationName = Binding.ScriptName + TEXT("(");
	const int32 NameOffset = Binding.Declaration.Find(
		DeclarationName, ESearchCase::CaseSensitive);
	if (NameOffset == INDEX_NONE)
		return false;

	FString AliasDeclaration = Binding.Declaration;
	AliasDeclaration.InsertAt(
		NameOffset + Binding.ScriptName.Len(), TEXT("_Implementation"));

	FAngelscriptManager& Manager = FAngelscriptManager::Get();
	asIScriptEngine* Engine = Manager.GetScriptEngine();
	if (Engine == nullptr)
		return false;
	asITypeInfo* TargetObjectType = Engine->GetTypeInfoByName(
		TCHAR_TO_ANSI(*Binding.TargetScriptTypeName));
	if (TargetObjectType == nullptr)
		return false;

	TUniquePtr<FAngelscriptNativeEventSuperAlias> Alias =
		MakeUnique<FAngelscriptNativeEventSuperAlias>();
	Alias->CanonicalScriptName = Binding.ScriptName;
	Alias->NativeFunctionName = Binding.NativeFunctionName.ToString();
	Alias->NativeDeclaringClassPath = Binding.NativeDeclaringClassPath;
	Alias->ReturnType = Binding.ReturnType;
	Alias->ArgumentTypes = Binding.ArgumentTypes;
	Alias->bInitializeReturn = Binding.bInitializeReturn;
	Alias->bZeroReturnPointer = Binding.bZeroReturnPointer;

	const int32 AliasFunctionId = FAngelscriptBinds::BindMethodDirect(
		Binding.TargetScriptTypeName,
		AliasDeclaration,
		asFUNCTION(CallNativeEventSuperWithSignature),
		asCALL_GENERIC,
		FASBindFunctionPointers{},
		nullptr);
	asIScriptFunction* AliasFunction =
		Engine->GetFunctionById(AliasFunctionId);
	asITypeInfo* AliasObjectType = AliasFunction == nullptr
		? nullptr
		: AliasFunction->GetObjectType();
	if (AliasFunction == nullptr || AliasObjectType != TargetObjectType)
		return false;

	Alias->FunctionIdentity = AliasFunction;
	AliasFunction->SetUserData(Alias.Get());
	if (RegisterAngelscriptNativeEventSuperAlias(
		Engine, MoveTemp(Alias)))
	{
		return true;
	}
	AliasFunction->SetUserData(nullptr);
	return false;
}

void OnBlueprintEventBound(
	const FAngelscriptFunctionSignature& Signature,
	const FString& TargetScriptTypeName,
	UFunction* Function,
	const FAngelscriptTypeUsage* ArgumentTypes,
	int32 ArgumentCount,
	const FAngelscriptTypeUsage& ReturnType,
	bool bInitializeReturn,
	bool bZeroReturnPointer)
{
	if (!IsAngelscriptExtensionStageRegistered(
			EAngelscriptExtensionStage::BlueprintEventBound))
	{
		return;
	}
	FAngelscriptBlueprintEventBindingSnapshot Binding;
	Binding.Target = Signature.TargetType
		== FAngelscriptFunctionSignature::EBindTargetType::GlobalFunction
		? EAngelscriptBlueprintEventTarget::GlobalFunction
		: Signature.TargetType
			== FAngelscriptFunctionSignature::EBindTargetType::MixinMethod
			? EAngelscriptBlueprintEventTarget::MixinMethod
			: EAngelscriptBlueprintEventTarget::Method;
	Binding.TargetScriptTypeName = TargetScriptTypeName;
	Binding.Declaration = Signature.Declaration;
	Binding.ScriptName = Signature.ScriptName;
	UClass* DeclaringClass = Function == nullptr
		? nullptr
		: Function->GetOuterUClass();
	Binding.NativeDeclaringClassPath = DeclaringClass == nullptr
		? FString()
		: DeclaringClass->GetPathName();
	Binding.NativeFunctionName = Function == nullptr
		? NAME_None
		: Function->GetFName();
	if (ArgumentTypes != nullptr && ArgumentCount > 0)
		Binding.ArgumentTypes.Append(ArgumentTypes, ArgumentCount);
	Binding.ReturnType = ReturnType;
	Binding.bInitializeReturn = bInitializeReturn;
	Binding.bZeroReturnPointer = bZeroReturnPointer;

	FString ExtensionError;
	if (!DispatchAngelscriptBlueprintEventBound(Binding, ExtensionError))
	{
		UE_LOG(
			Angelscript,
			Fatal,
			TEXT("Blueprint event extension failed for %s: %s"),
			Function == nullptr ? TEXT("<null>") : *Function->GetPathName(),
			*ExtensionError);
	}
}
}

bool RegisterAngelscriptNativeEventSuperAlias(
	const FAngelscriptBlueprintEventBindingSnapshot& Binding)
{
	return Cuviss::NativeEventSuper::RegisterAlias(Binding);
}
