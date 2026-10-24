// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FProperty;

/** Result of an explicit caller-owned compiler-default property copy request. */
enum class EAngelscriptBlueprintDefaultCopyResult : uint8
{
	Accepted,
	NullSource,
	NullTarget,
	InvalidObject,
	SameObject,
	RecursiveCopy,
	ClassMismatch,
	EmptyPropertySelection,
	UnsafePropertyPolicy,
	InvalidPropertyName,
	DuplicateProperty,
	MissingProperty,
	PropertyTypeMismatch,
	UnsafeProperty,
	UnsupportedPropertyKind,
	PropertyRejectedByPolicy,
};

enum class EAngelscriptBlueprintDefaultCopyTracePhase : uint8
{
	ValidationAccepted,
	CopyCallReturned,
};

enum class EAngelscriptBlueprintDefaultCopyCapability : uint8
{
	ExternalPropertyCopy,
	CompilerDefaultCopy,
};

/**
 * Caller-owned policy for copying a bounded set of scalar properties between matching objects.
 *
 * This is an external filter/copy contract. It does not identify compiler-generated defaults,
 * implement PostDuplicateBlueprint, perform automatic CDO copying, own duplicate/reinstance
 * transactions, or claim transparent engine timing.
 */
struct FAngelscriptBlueprintDefaultCopyRequest
{
	UObject* Source = nullptr;
	UObject* Target = nullptr;
	TArray<FName> PropertyNames;
	TFunction<bool(const FProperty&, const FProperty&)> PropertyPolicy;
	bool bRecursiveCopy = false;
	mutable TArray<EAngelscriptBlueprintDefaultCopyTracePhase> Trace;
};

class ANGELSCRIPTEDITOR_API FAngelscriptBlueprintDefaultCopyFilterAdapter
{
public:
	static EAngelscriptBlueprintDefaultCopyResult Validate(
		const FAngelscriptBlueprintDefaultCopyRequest& Request);

	static EAngelscriptBlueprintDefaultCopyResult Copy(
		const FAngelscriptBlueprintDefaultCopyRequest& Request);

	static bool HasCapability(EAngelscriptBlueprintDefaultCopyCapability Capability);
};
