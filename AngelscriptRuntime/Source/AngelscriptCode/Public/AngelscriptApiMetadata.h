// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

// Native producers own their identity until Unregister. Values are copied; no VM pointers
// or callbacks are retained. Keys are exact AngelScript declarations, including owner.
struct FAngelscriptApiMetadata
{
	FString Tier;
	FString Module;
	TArray<FString> Tags;
};

struct FAngelscriptApiMetadataEntry
{
	FString Key;
	FAngelscriptApiMetadata Metadata;
};

enum class EAngelscriptApiMetadataResult : uint8 { Missing, Found, Conflict };

class ANGELSCRIPTCODE_API FAngelscriptApiMetadataRegistry
{
public:
	// Atomically replaces this producer's complete set. Empty sets unregister it.
	// Duplicate keys disagreeing within one set also produce Conflict.
	static void Register(const void* Producer, const TArray<FAngelscriptApiMetadataEntry>& Entries);
	static void Unregister(const void* Producer);
	// Thread safe. OutMetadata is cleared for Missing/Conflict; tags are a sorted set.
	static EAngelscriptApiMetadataResult Query(const FString& Key, FAngelscriptApiMetadata& OutMetadata);
};
