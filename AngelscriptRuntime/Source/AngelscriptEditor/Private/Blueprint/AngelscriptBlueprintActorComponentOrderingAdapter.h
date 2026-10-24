// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class AActor;
class UActorComponent;

enum class EAngelscriptBlueprintActorComponentOrderingResult : uint8
{
	Accepted,
	NullActor,
	InvalidActor,
	EmptySnapshot,
	NullComponent,
	InvalidComponent,
	DuplicateComponent,
	WrongOwner,
	DestroyedComponent,
	SnapshotSizeMismatch,
	ReplacementMissing,
	ReplacementUnexpected,
	ReplacementIdentityMismatch,
	OrderMismatch,
};

/** Explicit caller-owned actor/component replacement ordering request. */
struct FAngelscriptBlueprintActorComponentOrderingRequest
{
	AActor* Actor = nullptr;
	TArray<UActorComponent*> Before;
	TArray<UActorComponent*> After;
	TMap<UActorComponent*, UActorComponent*> Replacements;
};

/**
 * Validates a caller-provided component snapshot and builds its ordered replacement list.
 *
 * Ordering is inherited exclusively from Before; Replacements is an identity lookup.
 * This Private-module adapter has no public ABI promise. It does not replace UObjects,
 * repair attachments, run GC, or claim Haze's private pre-snapshot/reinstancing
 * transaction ordering.
 */
class ANGELSCRIPTEDITOR_API FAngelscriptBlueprintActorComponentOrderingAdapter final
{
public:
	static EAngelscriptBlueprintActorComponentOrderingResult Validate(
		const FAngelscriptBlueprintActorComponentOrderingRequest& Request);

	static EAngelscriptBlueprintActorComponentOrderingResult BuildOrderedReplacementList(
		const FAngelscriptBlueprintActorComponentOrderingRequest& Request,
		TArray<UActorComponent*>& OutOrderedComponents);
};
