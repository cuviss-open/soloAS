// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"

/**
 * Result of an explicit, caller-owned payload migration after a script struct replacement.
 *
 * This adapter deliberately does not replace FInstancedStruct::Serialize, participate in
 * CoreUObject reference collection, or promise the engine's automatic replacement timing.
 */
enum class EAngelscriptInstancedStructMigrationResult : uint8
{
	Migrated,
	NullArgument,
	InvalidStructIdentity,
	StaleStructChain,
	InvalidPayload,
	EmptyPayload,
	InvalidTarget,
	ArchiveWriteFailed,
	ArchiveReadFailed,
	SchemaDrift, // Old/New/Newest property layouts are not identical.
	UnsupportedReferencePayload, // The caller did not provide an explicit UObject replacement map.
	MissingReferenceReplacement, // A non-null payload reference was absent from that map.
};

struct FAngelscriptInstancedStructMigration
{
	static EAngelscriptInstancedStructMigrationResult MigratePayload(
		FInstancedStruct* Payload,
		const UScriptStruct* OldStruct,
		const UScriptStruct* NewStruct,
		const UScriptStruct* NewestStruct);

	/**
	 * Migrates a payload while applying an explicit caller-owned UObject replacement map.
	 *
	 * Every non-null UObject reference encountered in the payload must be a key in
	 * ReplacementMap. A null mapped value intentionally clears that reference, matching
	 * FArchiveReplaceObjectRef semantics. The caller owns the map and must keep all source,
	 * replacement, and struct objects GC-reachable for the duration of this call and must
	 * arrange reachability for the resulting payload afterwards. This adapter does not add
	 * references to a collector, root objects, or participate in automatic replacement.
	 */
	static EAngelscriptInstancedStructMigrationResult MigratePayloadWithObjectReplacements(
		FInstancedStruct* Payload,
		const UScriptStruct* OldStruct,
		const UScriptStruct* NewStruct,
		const UScriptStruct* NewestStruct,
		const TMap<UObject*, UObject*>& ReplacementMap);
};
