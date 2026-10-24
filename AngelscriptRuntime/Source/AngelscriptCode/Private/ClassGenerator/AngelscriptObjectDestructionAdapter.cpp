// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptObjectDestructionAdapter.h"

#include "AngelscriptExecutionQuiescence.h"
#include "ClassGenerator/ASClass.h"
#include "ClassGenerator/AngelscriptClassState.h"

#include "StartAngelscriptHeaders.h"
#include "as_scriptengine.h"
#include "EndAngelscriptHeaders.h"

#include "UObject/Object.h"

EAngelscriptObjectDestructionResult
FAngelscriptObjectDestructionAdapter::Validate(
	const FAngelscriptObjectDestructionRequest& Request)
{
	if (Request.Object == nullptr)
		return EAngelscriptObjectDestructionResult::NullObject;
	if (Request.ScriptClass == nullptr)
		return EAngelscriptObjectDestructionResult::NullScriptClass;
	if (Request.LifecycleGeneration != UASClass::RuntimeLifecycleGeneration)
		return EAngelscriptObjectDestructionResult::InvalidLifecycleGeneration;
	if (!IsInGameThread())
		return EAngelscriptObjectDestructionResult::NotGameThread;
	if (IsAngelscriptExecutionShutdownStarted())
		return EAngelscriptObjectDestructionResult::ExecutionShutdownStarted;
	if (!IsAngelscriptExecutionWriterHeldByCurrentThread())
		return EAngelscriptObjectDestructionResult::MissingExecutionWriter;
	if (!IsValid(Request.Object))
		return EAngelscriptObjectDestructionResult::InvalidObject;
	if (Request.Object->IsUnreachable())
		return EAngelscriptObjectDestructionResult::ObjectIsUnreachable;
	if (Request.Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		return EAngelscriptObjectDestructionResult::ObjectIsBeingDestroyed;
	if (Request.Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		return EAngelscriptObjectDestructionResult::UnsupportedObjectKind;
	if (Request.Object->GetClass() != Request.ScriptClass)
		return EAngelscriptObjectDestructionResult::MismatchedScriptClass;
	if (GetAngelscriptClassScriptType(Request.ScriptClass) == nullptr)
		return EAngelscriptObjectDestructionResult::MissingScriptType;
	asITypeInfo* ScriptType = GetAngelscriptClassScriptType(Request.ScriptClass);
	if (ScriptType->GetUserData() != Request.ScriptClass)
		return EAngelscriptObjectDestructionResult::MismatchedScriptType;
	return EAngelscriptObjectDestructionResult::Accepted;
}

EAngelscriptObjectDestructionResult
FAngelscriptObjectDestructionAdapter::Destroy(
	const FAngelscriptObjectDestructionRequest& Request)
{
	const EAngelscriptObjectDestructionResult ValidationResult = Validate(Request);
	if (ValidationResult != EAngelscriptObjectDestructionResult::Accepted)
		return ValidationResult;

	UASClass::DestroyTrackedRuntimeObjectExplicitly(Request.Object);
	return EAngelscriptObjectDestructionResult::Destroyed;
}
