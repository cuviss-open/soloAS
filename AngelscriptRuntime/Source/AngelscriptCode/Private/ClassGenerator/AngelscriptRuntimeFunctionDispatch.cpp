// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/ASClass.h"

#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "ClassGenerator/AngelscriptActorConstruction.h"
#include "UObject/ScriptMacros.h"
#include "Misc/ScopeExit.h"

namespace
{
bool AreRuntimeValidationSignaturesCompatible(
	const UFunction* RuntimeFunction,
	const UFunction* RuntimeValidateFunction)
{
	TFieldIterator<FProperty> RuntimeParameter(RuntimeFunction);
	TFieldIterator<FProperty> ValidationParameter(RuntimeValidateFunction);
	while (RuntimeParameter
		&& (RuntimeParameter->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm)
	{
		if (!ValidationParameter
			|| (ValidationParameter->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) != CPF_Parm
			|| !RuntimeParameter->SameType(*ValidationParameter)
			|| (RuntimeParameter->PropertyFlags & CPF_ParmFlags)
				!= (ValidationParameter->PropertyFlags & CPF_ParmFlags)
			|| RuntimeParameter->GetOffset_ForUFunction()
				!= ValidationParameter->GetOffset_ForUFunction())
		{
			return false;
		}

		++RuntimeParameter;
		++ValidationParameter;
	}

	if (ValidationParameter
		&& (ValidationParameter->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm)
	{
		return false;
	}

	const FBoolProperty* ValidationReturn =
		CastField<FBoolProperty>(RuntimeValidateFunction->GetReturnProperty());
	return ValidationReturn != nullptr && ValidationReturn->IsNativeBool();
}
}

DEFINE_FUNCTION(UASFunction::execAngelscriptRuntimeThunk)
{
	UASFunction* RuntimeFunction = CastChecked<UASFunction>(Stack.CurrentNativeFunction);
	if (RuntimeFunction->HasAnyFunctionFlags(FUNC_NetValidate))
	{
		UFunction* RuntimeValidateFunction = RuntimeFunction->GetRuntimeValidateFunction();
		if (RuntimeValidateFunction == nullptr)
		{
			RPC_ValidateFailed(TEXT("Angelscript RPC validation function is missing"));
			return;
		}

		const bool bCompatibleValidationSignature =
			AreRuntimeValidationSignaturesCompatible(RuntimeFunction, RuntimeValidateFunction);
		checkf(
			bCompatibleValidationSignature,
			TEXT("Angelscript RPC validation signature must match its runtime function."));
		if (!bCompatibleValidationSignature)
		{
			RPC_ValidateFailed(TEXT("Angelscript RPC validation signature is incompatible"));
			return;
		}

		FFrame ValidationStack(
			Context,
			RuntimeValidateFunction,
			Stack.Locals,
			&Stack,
			Stack.PropertyChainForCompiledIn);
		ValidationStack.Code = Stack.Code;
		ValidationStack.OutParms = Stack.OutParms;

		bool bValidationPassed = false;
		RuntimeValidateFunction->Invoke(Context, ValidationStack, &bValidationPassed);
		if (!bValidationPassed)
		{
			RPC_ValidateFailed(TEXT("Angelscript RPC validation failed"));
			return;
		}
	}

	if (AActor* ActorContext = Cast<AActor>(Context))
	{
		checkf(
			IsInGameThread(),
			TEXT("Actor BindComponents lazy linking requires the game thread.")
		);
		if (IsInGameThread() && ActorContext->IsActorInitialized())
		{
			UASClass::EnsureBindComponentsLinked(ActorContext);
		}
	}

	UWorld* ConstructionWorld = nullptr;
	if (AActor* Actor = Cast<AActor>(Context); Actor && Actor->IsRunningUserConstructionScript()
		&& RuntimeFunction->GetFName() == FName(TEXT("UserConstructionScript"))
		&& AngelscriptActorConstruction::IsPureScriptActor(Actor) && Actor->GetWorld())
	{
		// Preserve the engine's UCS and delegate flow for the direct UClass carrier.
		ConstructionWorld = Actor->GetWorld();
		if (ConstructionWorld->IsGameWorld()) AngelscriptActorConstruction::Prepare(Actor);
	}
	const bool bPreviousConstruction = ConstructionWorld && ConstructionWorld->bIsRunningConstructionScript;
	if (ConstructionWorld) ConstructionWorld->bIsRunningConstructionScript = true;
	ON_SCOPE_EXIT
	{
		if (ConstructionWorld) ConstructionWorld->bIsRunningConstructionScript = bPreviousConstruction;
	};
	RuntimeFunction->RuntimeCallFunction(Context, Stack, RESULT_PARAM);
}
