// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned public API surface export commandlet for the AngelscriptModKit
// module . Zero new Runtime/Bridge registration points: the
// exported set is computed from the already-registered engine surface and the
// configured allowlist, using the exact same declaration format and matcher as
// the CS.Runtime.Modding.ApiAllowlistAudit handler.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ModKitApiSurfaceExport.generated.h"

/**
 * Public API declaration export (-run=ModKitExportApiSurface -OutDir=<abs>).
 *
 * Enumerates the candidate declaration surface - engine-primary enumeration
 * (asIScriptEngine global functions + object type methods/behaviours) with the
 * FAngelscriptBindDatabase classes/structs surface used for the snapshot
 * digest (design card MK-T601 v2 section 1.5) - aligns it to the three audit
 * faces (systemCall / baseType / override, mirroring
 * FModKitApiAllowlist::FModApiUsage), filters each face with
 * the UModKitSettings allowlist patterns through
 * FModKitApiAllowlist::IsAllowed (same policy and matcher as the runtime
 * audit), and writes a sorted PublicApi.as reference plus an ApiSurface.json
 * manifest carrying the engine version, BindDatabase digest and each
 * declaration's matchedBy tier/group/module/pattern reason.
 *
 * Deterministic within a fixed engine version: every face is sorted and
 * de-duplicated before writing. A configured pattern that hits no declaration
 * of its face emits MODKIT_API_EXPORT_WARNING (no silent empty export).
 * Self-consistency is asserted by the commandlet itself (every exported
 * declaration re-checks its recorded pattern).
 */
UCLASS()
class UModKitExportApiSurfaceCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
