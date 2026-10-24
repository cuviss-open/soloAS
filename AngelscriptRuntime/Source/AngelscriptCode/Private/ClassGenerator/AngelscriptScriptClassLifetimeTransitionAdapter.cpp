// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptScriptClassLifetimeTransitionAdapter.h"

#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
using EScriptClassLifetimeTransitionResult = EAngelscriptScriptClassLifetimeTransitionResult;
using EState = EAngelscriptScriptClassLifetimeState;

EScriptClassLifetimeTransitionResult ValidateRequest(
	const FAngelscriptScriptClassLifetimeRequest& Request)
{
	if (Request.ScriptClass == nullptr)
		return EScriptClassLifetimeTransitionResult::NullClass;
	if (!IsValid(Request.ScriptClass))
		return EScriptClassLifetimeTransitionResult::InvalidClass;
	if (Request.Generation == 0 || Request.TypeCookie == 0)
		return EScriptClassLifetimeTransitionResult::InvalidGeneration;
	return EScriptClassLifetimeTransitionResult::Accepted;
}

}

EAngelscriptScriptClassLifetimeTransitionResult
FAngelscriptScriptClassLifetimeTransitionRegistry::ValidateIdentity(
	const FAngelscriptScriptClassLifetimeRequest& Request,
	const TMap<UClass*, FClassState>& Classes,
	const FClassState*& OutState)
{
	const EScriptClassLifetimeTransitionResult RequestResult = ValidateRequest(Request);
	if (RequestResult != EScriptClassLifetimeTransitionResult::Accepted)
		return RequestResult;
	OutState = Classes.Find(Request.ScriptClass);
	if (OutState == nullptr)
		return EScriptClassLifetimeTransitionResult::MissingClass;
	if (OutState->Generation != Request.Generation || OutState->TypeCookie != Request.TypeCookie)
		return EScriptClassLifetimeTransitionResult::StaleIdentity;
	return EScriptClassLifetimeTransitionResult::Accepted;
}

EAngelscriptScriptClassLifetimeTransitionResult
FAngelscriptScriptClassLifetimeTransitionRegistry::Register(
	const FAngelscriptScriptClassLifetimeRequest& Request)
{
	const EScriptClassLifetimeTransitionResult ValidationResult = ValidateRequest(Request);
	if (ValidationResult != EScriptClassLifetimeTransitionResult::Accepted)
		return ValidationResult;
	if (Classes.Contains(Request.ScriptClass))
		return EScriptClassLifetimeTransitionResult::DuplicateClass;

	FClassState& State = Classes.Add(Request.ScriptClass);
	State.Generation = Request.Generation;
	State.TypeCookie = Request.TypeCookie;
	State.State = EState::Registered;
	return EScriptClassLifetimeTransitionResult::Accepted;
}

EAngelscriptScriptClassLifetimeTransitionResult
FAngelscriptScriptClassLifetimeTransitionRegistry::Activate(
	const FAngelscriptScriptClassLifetimeRequest& Request)
{
	const FClassState* State = nullptr;
	const EScriptClassLifetimeTransitionResult ValidationResult = ValidateIdentity(Request, Classes, State);
	if (ValidationResult != EScriptClassLifetimeTransitionResult::Accepted)
		return ValidationResult;
	if (State->State != EState::Registered)
		return EScriptClassLifetimeTransitionResult::InvalidTransition;
	Classes.FindChecked(Request.ScriptClass).State = EState::Active;
	return EScriptClassLifetimeTransitionResult::Accepted;
}

EAngelscriptScriptClassLifetimeTransitionResult
FAngelscriptScriptClassLifetimeTransitionRegistry::BeginRetirement(
	const FAngelscriptScriptClassLifetimeRequest& Request)
{
	const FClassState* State = nullptr;
	const EScriptClassLifetimeTransitionResult ValidationResult = ValidateIdentity(Request, Classes, State);
	if (ValidationResult != EScriptClassLifetimeTransitionResult::Accepted)
		return ValidationResult;
	if (State->State == EState::Retired)
		return EScriptClassLifetimeTransitionResult::AlreadyRetired;
	if (State->State != EState::Active)
		return EScriptClassLifetimeTransitionResult::InvalidTransition;
	Classes.FindChecked(Request.ScriptClass).State = EState::Retiring;
	return EScriptClassLifetimeTransitionResult::Accepted;
}

EAngelscriptScriptClassLifetimeTransitionResult
FAngelscriptScriptClassLifetimeTransitionRegistry::CompleteRetirement(
	const FAngelscriptScriptClassLifetimeRequest& Request)
{
	const FClassState* State = nullptr;
	const EScriptClassLifetimeTransitionResult ValidationResult = ValidateIdentity(Request, Classes, State);
	if (ValidationResult != EScriptClassLifetimeTransitionResult::Accepted)
		return ValidationResult;
	if (State->State == EState::Retired)
		return EScriptClassLifetimeTransitionResult::AlreadyRetired;
	if (State->State != EState::Retiring)
		return EScriptClassLifetimeTransitionResult::InvalidTransition;
	Classes.FindChecked(Request.ScriptClass).State = EState::Retired;
	return EScriptClassLifetimeTransitionResult::Accepted;
}

EAngelscriptScriptClassLifetimeTransitionResult
FAngelscriptScriptClassLifetimeTransitionRegistry::Unregister(
	const FAngelscriptScriptClassLifetimeRequest& Request)
{
	const FClassState* State = nullptr;
	const EScriptClassLifetimeTransitionResult ValidationResult = ValidateIdentity(Request, Classes, State);
	if (ValidationResult != EScriptClassLifetimeTransitionResult::Accepted)
		return ValidationResult;
	if (State->State != EState::Retired)
		return EScriptClassLifetimeTransitionResult::NotRetired;
	Classes.Remove(Request.ScriptClass);
	return EScriptClassLifetimeTransitionResult::Accepted;
}

EAngelscriptScriptClassLifetimeTransitionResult
FAngelscriptScriptClassLifetimeTransitionRegistry::GetState(
	const FAngelscriptScriptClassLifetimeRequest& Request,
	EAngelscriptScriptClassLifetimeState& OutState) const
{
	OutState = EState::Registered;
	const FClassState* State = nullptr;
	const EScriptClassLifetimeTransitionResult ValidationResult = ValidateIdentity(Request, Classes, State);
	if (ValidationResult != EScriptClassLifetimeTransitionResult::Accepted)
		return ValidationResult;
	OutState = State->State;
	return EScriptClassLifetimeTransitionResult::Accepted;
}
