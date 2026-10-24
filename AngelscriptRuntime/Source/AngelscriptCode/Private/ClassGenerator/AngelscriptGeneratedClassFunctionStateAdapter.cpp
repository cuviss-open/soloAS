// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptGeneratedClassFunctionStateAdapter.h"

#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
using EResult = EAngelscriptGeneratedClassFunctionStateResult;

EResult ValidateClassRequest(const FAngelscriptGeneratedClassStateRequest& Request)
{
	if (Request.GeneratedClass == nullptr)
		return EResult::NullClass;
	if (!IsValid(Request.GeneratedClass))
		return EResult::InvalidClass;
	if (Request.Generation == 0 || Request.TypeCookie == 0)
		return EResult::InvalidGeneration;
	return EResult::Accepted;
}
}

EAngelscriptGeneratedClassFunctionStateResult
FAngelscriptGeneratedClassFunctionStateRegistry::RegisterClass(
	const FAngelscriptGeneratedClassStateRequest& Request)
{
	const EResult ValidationResult = ValidateClassRequest(Request);
	if (ValidationResult != EResult::Accepted)
		return ValidationResult;
	if (Classes.Contains(Request.GeneratedClass))
		return EResult::DuplicateClass;

	FClassState& State = Classes.Add(Request.GeneratedClass);
	State.Generation = Request.Generation;
	State.TypeCookie = Request.TypeCookie;
	State.bRetired = false;
	return EResult::Accepted;
}

EAngelscriptGeneratedClassFunctionStateResult
FAngelscriptGeneratedClassFunctionStateRegistry::RegisterFunction(
	const FAngelscriptGeneratedFunctionStateRequest& Request)
{
	const EResult ValidationResult = ValidateClassRequest(Request.Class);
	if (ValidationResult != EResult::Accepted)
		return ValidationResult;
	if (Request.Function == nullptr)
		return EResult::NullFunction;
	if (!IsValid(Request.Function))
		return EResult::InvalidFunction;
	if (Request.FunctionName.IsNone() || Request.Function->GetName() != Request.FunctionName)
		return EResult::FunctionIdentityMismatch;
	if (Request.Function->GetOwnerClass() != Request.Class.GeneratedClass)
		return EResult::FunctionIdentityMismatch;

	FClassState* State = Classes.Find(Request.Class.GeneratedClass);
	if (State == nullptr)
		return EResult::MissingClass;
	if (State->Generation != Request.Class.Generation
		|| State->TypeCookie != Request.Class.TypeCookie)
		return EResult::StaleClass;
	if (State->bRetired)
		return EResult::RetiredClass;
	if (State->Functions.Contains(Request.FunctionName))
		return EResult::DuplicateFunction;
	State->Functions.Add(Request.FunctionName, Request.Function);
	return EResult::Accepted;
}

EAngelscriptGeneratedClassFunctionStateResult
FAngelscriptGeneratedClassFunctionStateRegistry::LookupFunction(
	const FAngelscriptGeneratedClassStateRequest& ClassRequest,
	const FName FunctionName,
	UFunction*& OutFunction) const
{
	OutFunction = nullptr;
	const EResult ValidationResult = ValidateClassRequest(ClassRequest);
	if (ValidationResult != EResult::Accepted)
		return ValidationResult;
	if (FunctionName.IsNone())
		return EResult::MissingFunction;

	const FClassState* State = Classes.Find(ClassRequest.GeneratedClass);
	if (State == nullptr)
		return EResult::MissingClass;
	if (State->Generation != ClassRequest.Generation
		|| State->TypeCookie != ClassRequest.TypeCookie)
		return EResult::StaleClass;
	if (State->bRetired)
		return EResult::RetiredClass;
	UFunction* const* Function = State->Functions.Find(FunctionName);
	if (Function == nullptr || *Function == nullptr || !IsValid(*Function))
		return EResult::MissingFunction;
	if ((*Function)->GetName() != FunctionName)
		return EResult::FunctionIdentityMismatch;
	if ((*Function)->GetOwnerClass() != ClassRequest.GeneratedClass)
		return EResult::FunctionIdentityMismatch;
	OutFunction = *Function;
	return EResult::Accepted;
}

EAngelscriptGeneratedClassFunctionStateResult
FAngelscriptGeneratedClassFunctionStateRegistry::RetireClass(
	const FAngelscriptGeneratedClassStateRequest& Request)
{
	const EResult ValidationResult = ValidateClassRequest(Request);
	if (ValidationResult != EResult::Accepted)
		return ValidationResult;
	FClassState* State = Classes.Find(Request.GeneratedClass);
	if (State == nullptr)
		return EResult::MissingClass;
	if (State->Generation != Request.Generation || State->TypeCookie != Request.TypeCookie)
		return EResult::StaleClass;
	if (State->bRetired)
		return EResult::RetiredClass;
	State->bRetired = true;
	State->Functions.Reset();
	return EResult::Accepted;
}

EAngelscriptGeneratedClassFunctionStateResult
FAngelscriptGeneratedClassFunctionStateRegistry::UnregisterClass(
	const FAngelscriptGeneratedClassStateRequest& Request)
{
	const EResult ValidationResult = ValidateClassRequest(Request);
	if (ValidationResult != EResult::Accepted)
		return ValidationResult;
	FClassState* State = Classes.Find(Request.GeneratedClass);
	if (State == nullptr)
		return EResult::MissingClass;
	if (State->Generation != Request.Generation || State->TypeCookie != Request.TypeCookie)
		return EResult::StaleClass;
	if (!State->bRetired)
		return EResult::RetiredClass;
	Classes.Remove(Request.GeneratedClass);
	return EResult::Accepted;
}
