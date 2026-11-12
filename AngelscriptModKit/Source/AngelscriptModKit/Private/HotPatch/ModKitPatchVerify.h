// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned offline patch manifest verification commandlet for the
// AngelscriptModKit module . Zero new Runtime/Bridge
// registration points: the verifier is pure ModKit-side code with a
// self-contained Ed25519 verify-only primitive.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ModKitPatchVerify.generated.h"

/**
 * Offline patch manifest verification (-run=ModKitPatchVerify
 * -Manifest=<abs> [-PublicKey=<base64>] [-Kind=hotpatch|package]).
 * Package mode verifies the Mod directory beside the manifest and an optional
 * -MinimumVersion=<version>, without changing the runtime version floor.
 *
 * Parses and schema-validates the manifest, re-checks the payloadSha256
 * anchor, verifies the Ed25519 signature over the canonical payload, verifies
 * every file's SHA-256 against the manifest, and applies the precheck +
 * engine-version gates. Every failure emits
 * MODKIT_PATCH_VERIFY_FAILED reason=<schema|key-missing|signature|
 * sha-mismatch|precheck-required|engine-mismatch|config> and exits 1; success
 * emits MODKIT_PATCH_VERIFY_OK and exits 0. 
 * (modkit-g10-sign.ps1) drives one independent editor-cmd process per fixture
 * variant and asserts the marker/exit per red gate.
 */
UCLASS()
class UModKitPatchVerifyCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};