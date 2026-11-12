// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "RuntimeServices/ModKitRuntimeHash.h"

enum class EModKitCacheDirection : uint8 { Save, Load };

// Only authenticated byte envelopes; no VM or serializer policy.
struct FModKitCacheEnvelopePolicy
{
    bool bTransform = false;
    bool bBlocked = false;
    FString KeyId;
    FString RuntimeModule;
    TArray<uint8> HeaderFragment;
    TArray<uint8> ExpectedRuntimeHash;
};

struct FModKitCacheTransform
{
    static void Startup();
    static void Shutdown();
    static bool RetainsFileView();
    static bool GetTransformPolicy(EModKitCacheDirection Direction, FModKitCacheEnvelopePolicy& OutPolicy);
    static bool TransformCacheBytes(EModKitCacheDirection Direction, FModKitCacheEnvelopePolicy& Policy, TArray<uint8>& Bytes);
    static void SetRuntimeHashProvider(FModKitRuntimeHashProvider Provider);
    static void ExecGenerateScriptCacheKey(const TArray<FString>& Args);
};
