// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "Extensions/AngelscriptModdingRegistration.h"

// Runtime dispatch only. Registration and handler contracts remain public.

struct FAngelscriptCacheCompileSurfaceGateRuntime : FAngelscriptCacheCompileSurfaceGate
{
	/**
	 * True when any registered handler requests preservation.
	 * Empty registry -> false (upstream behavior).
	 */
	static bool AnyPreserve();
};

struct FAngelscriptModdingCacheGenerationFilterRuntime : FAngelscriptModdingCacheGenerationFilter
{
	/** True when a registered handler excludes this unit from the current Game target. */
	static bool IsGameCacheExcludedScriptUnit(const FString& ScriptUnitId);

	/** True when a registered handler excludes this unit from the project cache. */
	static bool IsGameCacheExcludedProjectScriptUnit(const FString& ScriptUnitId);

	/**
	 * True when any registered handler excludes the module.
	 */
	static bool AnyExclude(const FString& ModuleName, const TArray<FString>& RelativeFilenames);

	/** True when any registered handler excludes the module from static JIT. */
	static bool AnyJitExclude(const FString& ModuleName, const TArray<FString>& RelativeFilenames);
};

struct FAngelscriptModdingDebugServerStartGateRuntime : FAngelscriptModdingDebugServerStartGate
{
	/**
	 * True when any registered handler suppresses the debug server.
	 * Empty registry -> false (upstream behavior).
	 */
	static bool AnySuppress();
};

struct FAngelscriptModdingDebugMessageAccessGateRuntime : FAngelscriptModdingDebugMessageAccessGate
{
	/** True when at least one policy handler is registered. */
	static bool HasHandlers();
};

struct FAngelscriptModdingLeafDirectionAuditRuntime : FAngelscriptModdingLeafDirectionAudit
{
	/** True when at least one handler is registered (skips audit cost otherwise). */
	static bool HasHandlers();

	/**
	 * True when any registered handler reports a violation.
	 * Empty registry -> false (upstream behavior).
	 */
	static bool AnyViolation(const FString& ModuleName, const TArray<FString>& DependencyModuleNames, FString& OutError);
};

struct FAngelscriptModdingApiAllowlistAuditRuntime : FAngelscriptModdingApiAllowlistAudit
{
	/** True when at least one handler is registered (skips audit cost otherwise). */
	static bool HasHandlers();

	/**
	 * True when any registered handler reports a violation.
	 * Empty registry -> false (upstream behavior).
	 */
	static bool AnyViolation(const FString& ModuleName, const FModApiUsage& Usage, FString& OutError);
};

struct FAngelscriptModdingScriptExecutionBudgetRuntime : FAngelscriptModdingScriptExecutionBudget
{
	/** True when at least one budget query is registered (skips query cost otherwise). */
	static bool HasHandlers();

	/**
	 * True when any registered query returns a positive budget; OutBudgetSeconds
	 * receives it. Empty registry -> false (upstream behavior).
	 */
	static bool AnyBudget(float& OutBudgetSeconds, const asIScriptContext& Context);

	/** Delivers a budget overrun to every registered timeout handler. */
	static void NotifyTimeout(const FString& ModuleName, double ElapsedSeconds);
};

struct FAngelscriptModdingPrecompiledDataTransformRuntime : FAngelscriptModdingPrecompiledDataTransform
{
	/** True when at least one transform handler is registered (skips query cost otherwise). */
	static bool HasHandlers();

	/**
	 * Queries the registered policy handler. Empty registry -> false and
	 * OutPolicy untouched (upstream behavior).
	 */
	static bool GetPolicy(EDirection Direction, FPolicy& OutPolicy);

	/**
	 * Invokes the registered bytes handler. Empty registry -> returns true
	 * with Bytes untouched (upstream behavior).
	 */
	static bool TransformBytes(EDirection Direction, FPolicy& Policy, TArray<uint8>& Bytes);
};

struct FAngelscriptModdingMemoryIntegrityRuntime : FAngelscriptModdingMemoryIntegrity
{
	/** True when at least one check is registered (skips call cost otherwise). */
	static bool HasHandlers();

	/** Runs every registered check. Empty registry -> no-op. */
	static void RunChecks();
};

struct FAngelscriptModdingStaticJITGenerationGateRuntime : FAngelscriptModdingStaticJITGenerationGate
{
	/** True when at least one handler is registered (skips query cost otherwise). */
	static bool HasHandlers();

	/**
	 * Queries the registered policy handler. Empty registry -> false and
	 * OutPolicy untouched (upstream behavior).
	 */
	static bool GetPolicy(
		const TArray<FString>& ModuleNames,
		const TArray<FString>& RelativeFilenames,
		int32 EngineBuildIdentifier,
		FPolicy& OutPolicy);

	/**
	 * Delivers the final output set to the registered completion handler.
	 * Empty registry -> no-op.
	 */
	static void NotifyComplete(
		const FString& ManifestPath,
		const TArray<FString>& OutputFilenames,
		const TArray<FString>& CoveredModuleNames,
		const TArray<FString>& CoveredFunctionIdentities);

	/**
	 * Batch-skip run flag: set by the WriteOutputCode gate block when it takes
	 * the early-return skip path, read once by the cache-save gate block in
	 * AngelscriptManager (a skipped JIT pass must also skip the cache re-save,
	 * because the cache embeds the process-specific precompiled reference table
	 * that pairs with the JIT output of the SAME generation). The flag is
	 * process-local mechanical state for the single-threaded generate flow and
	 * resets to false when consumed. Empty registry -> never set -> upstream.
	 */
	static void MarkBatchSkippedThisRun();

	static bool ConsumeBatchSkippedThisRun();
};
