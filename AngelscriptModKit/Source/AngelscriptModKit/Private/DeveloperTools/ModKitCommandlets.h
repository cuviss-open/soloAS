// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned validation commandlets for the AngelscriptModKit module.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ModKitCommandlets.generated.h"

/**
 * Validation commandlet (-run=ModKitAssetAudit): scans /Game assets for soft
 * object references to source-only leaf module classes (R2 asset-side
 * discipline: game assets must not reference leaf/mod classes, because the
 * compile-time dependency graph cannot see string or soft references).
 * Emits MODKIT_AUDIT_VIOLATION per finding and returns non-zero; emits
 * MODKIT_AUDIT_CLEAN otherwise. Direct FSoftObjectProperty members are
 * scanned; nested containers are not walked yet.
 */
UCLASS()
class UModKitAssetAuditCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};

/**
 * Validation fixture generator (-run=ModKitMakeAuditViolationFixture):
 * writes a data asset holding a soft reference to a leaf module class so the
 * audit commandlet has a deterministic violation to find. Test tooling for
 * the acceptance chain; emits MODKIT_AUDIT_FIXTURE_WRITTEN on success.
 */
UCLASS()
class UModKitMakeAuditViolationFixtureCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};

/** Assemble one prepared FeatureMod without packaging the host project. */
UCLASS()
class ANGELSCRIPTMODKIT_API UModKitAssembleCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	virtual int32 Main(const FString& Params) override;
};

/** Create the initial data asset for an enabled GameFeature plugin. */
UCLASS()
class UModKitCreateGameFeatureDataCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	virtual int32 Main(const FString& Params) override;
};

/** Seal a completed stock cache into a separate authenticated delivery file. */
UCLASS()
class UModKitSealScriptCacheCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    virtual int32 Main(const FString& Params) override;
};
