// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptInstancedSubobjectReplacementAdapter.h"

#include "UObject/Object.h"

EAngelscriptInstancedSubobjectReplacementResult
FAngelscriptInstancedSubobjectReplacementAdapter::Validate(
	const FAngelscriptInstancedSubobjectReplacementRequest& Request)
{
	if (Request.Existing == nullptr || Request.Replacement == nullptr
		|| Request.ExpectedOuter == nullptr || Request.ExpectedArchetype == nullptr)
	{
		return EAngelscriptInstancedSubobjectReplacementResult::NullArgument;
	}

	if (!IsValid(Request.Existing) || !IsValid(Request.Replacement)
		|| !IsValid(Request.ExpectedOuter) || !IsValid(Request.ExpectedArchetype))
	{
		return EAngelscriptInstancedSubobjectReplacementResult::InvalidObject;
	}

	if (Request.ExpectedName.IsNone() || Request.ExpectedClass == nullptr)
	{
		return EAngelscriptInstancedSubobjectReplacementResult::NullArgument;
	}

	if (!IsValid(Request.ExpectedClass))
	{
		return EAngelscriptInstancedSubobjectReplacementResult::InvalidObject;
	}

	if (Request.Existing == Request.Replacement)
	{
		return EAngelscriptInstancedSubobjectReplacementResult::SameObject;
	}

	if (Request.Existing->GetClass() != Request.ExpectedClass
		|| Request.Replacement->GetClass() != Request.ExpectedClass)
	{
		return EAngelscriptInstancedSubobjectReplacementResult::ClassMismatch;
	}

	if (Request.Existing->GetFName() != Request.ExpectedName
		|| Request.Replacement->GetFName() != Request.ExpectedName
		|| Request.Existing->GetFName() != Request.Replacement->GetFName())
	{
		return EAngelscriptInstancedSubobjectReplacementResult::NameMismatch;
	}

	if (Request.Existing->GetOuter() != Request.ExpectedOuter
		|| Request.Replacement->GetOuter() != Request.ExpectedOuter
		|| Request.Existing->GetOuter() != Request.Replacement->GetOuter())
	{
		return EAngelscriptInstancedSubobjectReplacementResult::OuterMismatch;
	}

	if (Request.Existing->GetArchetype() != Request.ExpectedArchetype
		|| Request.Replacement->GetArchetype() != Request.ExpectedArchetype
		|| Request.Existing->GetArchetype() != Request.Replacement->GetArchetype())
	{
		return EAngelscriptInstancedSubobjectReplacementResult::ArchetypeMismatch;
	}

	const EObjectFlags InstancedCategoryFlags = RF_DefaultSubObject | RF_ArchetypeObject;
	if (!EnumHasAnyFlags(Request.RequiredFlags, InstancedCategoryFlags))
	{
		return EAngelscriptInstancedSubobjectReplacementResult::MissingInstancedSubobjectFlags;
	}

	const EObjectFlags ExistingInstancedFlags =
		Request.Existing->GetFlags() & InstancedCategoryFlags;
	const EObjectFlags ReplacementInstancedFlags =
		Request.Replacement->GetFlags() & InstancedCategoryFlags;
	if (ExistingInstancedFlags != ReplacementInstancedFlags
		|| (ExistingInstancedFlags & Request.RequiredFlags) != Request.RequiredFlags)
	{
		return EAngelscriptInstancedSubobjectReplacementResult::RequiredFlagsMissing;
	}

	if (Request.Existing->HasAnyFlags(Request.ForbiddenFlags)
		|| Request.Replacement->HasAnyFlags(Request.ForbiddenFlags))
	{
		return EAngelscriptInstancedSubobjectReplacementResult::ForbiddenFlagsPresent;
	}

	if (Request.Existing->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| Request.Replacement->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return EAngelscriptInstancedSubobjectReplacementResult::DestroyedObject;
	}

	return EAngelscriptInstancedSubobjectReplacementResult::Accepted;
}
