// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned offline patch precheck commandlet for the AngelscriptModKit
// module . Zero new Runtime/Bridge registration points: the
// verdict reuses the runtime's own SoftReloadOnly classifier (ECompileResult)
// as the single source of truth.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ModKitPatchPrecheck.generated.h"

/**
 * Offline hot-patch precheck (-run=ModKitPatchPrecheck -PatchDir=<abs>
 * [-Manifest=<abs>]).
 *
 * Runs an explicit two-pass compile in one process (B1): the process-start
 * Initial compile forms the baseline, then the patch files are compiled with
 * SoftReloadOnly and the resulting ECompileResult is mapped to the
 * verdict/reason/exit contract (B3):
 *   FullyHandled          -> verdict=PASS reason=soft                  exit 0
 *   PartiallyHandled      -> verdict=REJECT reason=structural-suggested exit 1
 *   ErrorNeedFullReload   -> verdict=REJECT reason=structural-required  exit 1
 *   Error                 -> verdict=REJECT reason=compile-error        exit 1
 *   patch module outside the game module set -> REJECT unexpected-module  exit 1
 *   missing/empty/invalid config or broken baseline -> MODKIT_PRECHECK_CONFIG
 *       marker, exit 2, no verdict marker. An absent MODKIT_PRECHECK_VERDICT
 *       is failure (A7).
 *
 * Single process, single patch (A6): one process checks exactly one patch;
 * multi-patch batching is deliberately unsupported because the runtime's
 * swap-in side effects (AngelscriptManager.cpp:3206-3207) pollute later
 * verdicts in the same process.
 *
 * Per-line detail uses only the public Diagnostics map that FormatDiagnostics
 * reads; ResetDiagnostics is called before the patch compile (B4).
 */
UCLASS()
class UModKitPatchPrecheckCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};