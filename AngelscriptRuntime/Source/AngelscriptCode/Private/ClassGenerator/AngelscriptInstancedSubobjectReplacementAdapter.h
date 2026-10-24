// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * Result of an explicit caller-owned instanced-subobject replacement check.
 *
 * This adapter validates objects already supplied by the caller. It does not perform
 * automatic UObject replacement or automatic GC, and it does not claim CoreUObject
 * replacement internals, transparent engine replacement, or linker or postload timing.
 */
enum class EAngelscriptInstancedSubobjectReplacementResult : uint8
{
	Accepted,
	NullArgument,
	InvalidObject,
	SameObject,
	NameMismatch,
	OuterMismatch,
	ArchetypeMismatch,
	ClassMismatch,
	MissingInstancedSubobjectFlags,
	RequiredFlagsMissing,
	ForbiddenFlagsPresent,
	DestroyedObject,
};

struct FAngelscriptInstancedSubobjectReplacementRequest
{
	const UObject* Existing = nullptr;
	const UObject* Replacement = nullptr;
	const UObject* ExpectedOuter = nullptr;
	const UObject* ExpectedArchetype = nullptr;
	const UClass* ExpectedClass = nullptr;
	FName ExpectedName = NAME_None;
	EObjectFlags RequiredFlags = RF_NoFlags;
	EObjectFlags ForbiddenFlags = RF_NoFlags;
};

struct FAngelscriptInstancedSubobjectReplacementAdapter
{
	static EAngelscriptInstancedSubobjectReplacementResult Validate(
		const FAngelscriptInstancedSubobjectReplacementRequest& Request);
};
