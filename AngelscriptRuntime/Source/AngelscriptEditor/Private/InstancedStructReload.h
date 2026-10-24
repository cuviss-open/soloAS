// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "AngelscriptRuntimeGcMirrorAPI.h"
#include "AngelscriptReloadSafety.h"

class UScriptStruct;

// A failed preparation leaves all original reflected property values unchanged.
// Success commits these values; later failures in the enclosing UE reload are not rolled back here.
bool MigrateInstancedStructsForReload(const TMap<UScriptStruct*, UScriptStruct*>& Replacements, FString& Error);

struct FAngelscriptPayloadReloadType;

// Read-only admission; candidate UE structures and payload defaults do not exist yet.
bool ValidateInstancedStructReload(const TArray<FAngelscriptPayloadReloadType>& Types, FString& Error);

// Reference payloads support direct FInstancedStruct properties on stable native owners.
// Fields: numeric/bool scalars, FString, fixed enums, strong UObject references,
// and one-dimensional TArray of strong UObject references. Retained fields keep their types;
// added fields use script defaults. Nested containers, maps, sets and weak references are unsupported.
// Ordinary FInstancedStruct tagged save/load is separate from the identity-indexed reload replay;
// capture and reference remapping are provided by the enclosing normal FullReload transaction.
// Reference payloads are published together on the game thread before the final stock reload.
// A committed failure terminates the enclosing reload; this is not a UE reload rollback.
class FInstancedStructReloadScope
{
public:
    FInstancedStructReloadScope(const TMap<UScriptStruct*, UScriptStruct*>& Replacements,
        const TMap<UClass*, UClass*>& ReloadClasses,
        const TArray<AngelscriptRuntimeGcMirror::FRuntimeScriptReplacementWitness>& Witnesses);
    ~FInstancedStructReloadScope();
    FInstancedStructReloadScope(const FInstancedStructReloadScope&) = delete;
    FInstancedStructReloadScope& operator=(const FInstancedStructReloadScope&) = delete;
    FInstancedStructReloadScope(FInstancedStructReloadScope&&) = delete;
    FInstancedStructReloadScope& operator=(FInstancedStructReloadScope&&) = delete;
    bool Prepare(FString& Error);
    bool Publish(FString& Error);
    static bool CaptureAdmission(const TArray<FAngelscriptPayloadReloadType>& Types, FString& Error);

private:
    class FImpl;
    TSharedPtr<AngelscriptReloadSafety::IPreparedPayloadReloadLease> Impl;
    TMap<UScriptStruct*, UScriptStruct*> Structs;
    TMap<UClass*, UClass*> Classes;
    TArray<AngelscriptRuntimeGcMirror::FRuntimeScriptReplacementWitness> Witnesses;
};
