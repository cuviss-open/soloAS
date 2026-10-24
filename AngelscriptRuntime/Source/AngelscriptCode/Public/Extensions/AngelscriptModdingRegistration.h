// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.

// Cuviss-owned registration support for the Modding track registration points.
// Registration points are empty by default and carry no business logic.

#pragma once

#include "CoreMinimal.h"

class asIScriptContext;

/**
 * Registration point CS.Runtime.Modding.CacheCompileSurfaceGate.
 *
 * Gates the cache-mode teardown of the compile surface
 * (FAngelscriptPrecompiledData::ClearUnneededRuntimeData at the end of
 * FAngelscriptManager::Initialize_AnyThread). An empty registry keeps the
 * upstream behavior bit-for-bit: the teardown always runs.
 */
struct FAngelscriptCacheCompileSurfaceGate
{
	using FPreserveHandler = TFunction<bool()>;

	/**
	 * Registers a preserve handler. Must be called on the game thread before
	 * Angelscript initialization reaches the teardown site (module loading).
	 */
	static ANGELSCRIPTCODE_API void Register(FPreserveHandler Handler);
};

/**
 * Registration point CS.Runtime.Modding.CacheGenerationModuleFilter.
 *
 * Filters precompiled cache, static JIT generation and script-unit discovery.
 * Registered handlers own exclusion policy; empty registries exclude nothing.
 */
struct FAngelscriptModdingCacheGenerationFilter
{
	using FExcludeHandler = TFunction<bool(const FString& ModuleName, const TArray<FString>& RelativeFilenames)>;

	/**
	 * Registers an exclude handler. Must be called on the game thread before
	 * Angelscript cache generation runs (module loading).
	 */
	static ANGELSCRIPTCODE_API void Register(FExcludeHandler CacheExcludeHandler, FExcludeHandler JitExcludeHandler);

	using FScriptUnitExcludeHandler = TFunction<bool(const FString& ScriptUnitId)>;
	/** Installs target and project-cache exclusion queries; unregister before unloading their owner. */
	static ANGELSCRIPTCODE_API FDelegateHandle RegisterScriptUnitFilters(
		FScriptUnitExcludeHandler GameExcludeHandler, FScriptUnitExcludeHandler ProjectCacheExcludeHandler);
	static ANGELSCRIPTCODE_API void UnregisterScriptUnitFilters(FDelegateHandle Handle);
};

/**
 * Registration point CS.Runtime.Modding.DebugServerStartGate.
 *
 * Gates the automatic start of the Angelscript debug server TCP listener
 * (FAngelscriptManager::Initialize_AnyThread). An empty registry keeps the
 * upstream behavior bit-for-bit: the server starts whenever the upstream
 * condition holds.
 */
struct FAngelscriptModdingDebugServerStartGate
{
	using FSuppressHandler = TFunction<bool()>;

	/**
	 * Registers a suppress handler. Must be called on the game thread before
	 * Angelscript initialization runs (module loading).
	 */
	static ANGELSCRIPTCODE_API void Register(FSuppressHandler Handler);
};

                                                                                                 
/**
 * Registration point CS.Runtime.Modding.DebugMessageAccessGate.
 *
 * Authorizes one debugger message against its stable operation category,
 * Angelscript module identity, and normalized script source identity. The
 * Runtime extracts identity before request dispatch and checks outgoing
 * messages before serialization. A filtered aggregate marks a Debug Database
 * chunk whose script declarations have already been checked item by item.
 * Empty registry -> allowed (upstream behavior).
 */
struct FAngelscriptModdingDebugMessageAccessGate
{
	enum class EOperation : uint8
	{
		Unknown,
		Session,
		Breakpoint,
		ExecutionControl,
		CallStack,
		Variables,
		Evaluation,
		Source,
		DebugDatabase,
		AssetDatabase,
	};

	struct FMessageContext
	{
		EOperation Operation = EOperation::Unknown;
		FString ModuleName;
		FString SourceIdentity;
		bool bIdentityKnown = false;
		bool bGlobalOperation = false;
		bool bFilteredAggregate = false;
		bool bNativeApi = false;
	};

	using FAccessHandler = TFunction<bool(const FMessageContext& Context)>;

	/** Registers a policy handler. Returns a handle usable with Unregister. */
	static ANGELSCRIPTCODE_API FDelegateHandle Register(FAccessHandler Handler);

	/** Unregisters a previously registered policy handler. */
	static ANGELSCRIPTCODE_API void Unregister(FDelegateHandle Handle);

	/** All registered policies must allow. Empty registry -> true (upstream behavior). */
	static ANGELSCRIPTCODE_API bool IsAllowed(const FMessageContext& Context);

	/** Stable config/log token for an operation category. */
	static ANGELSCRIPTCODE_API const TCHAR* GetOperationToken(EOperation Operation);
};
                                                                                               

/**
 * Registration point CS.Runtime.Modding.LeafDirectionAudit.
 *
 * Post-compile leaf-direction audit, invoked after a compile batch succeeds
 * and before the swap-in decision (FAngelscriptManager::CompileModules). An
 * empty registry keeps the upstream behavior bit-for-bit: no audit runs.
 */
struct FAngelscriptModdingLeafDirectionAudit
{
	/**
	 * Returns true when the module violates the direction rule; OutError
	 * receives the diagnostic message. DependencyModuleNames are the angelscript
	 * module names recorded in the module's compile-time dependency set.
	 */
	using FViolationHandler = TFunction<bool(const FString& ModuleName, const TArray<FString>& DependencyModuleNames, FString& OutError)>;

	/**
	 * Registers a violation handler. Must be called on the game thread before
	 * any compile batch runs (module loading).
	 */
	static ANGELSCRIPTCODE_API void Register(FViolationHandler Handler);
};

/**
 * Registration point CS.Runtime.Modding.ApiAllowlistAudit.
 *
 * Post-compile API allowlist audit, invoked after a compile batch succeeds
 * and before the swap-in decision (FAngelscriptManager::CompileModules). The
 * hook body mechanically extracts each module's external (system) API surface
 * as declaration strings; the registered handler matches them against a
 * configured allowlist. An empty registry keeps the upstream behavior
 * bit-for-bit: no extraction runs.
 */
struct FAngelscriptModdingApiAllowlistAudit
{
	/**
	 * The external API surface of one compiled module: direct system function
	 * calls (including constructors, destructors, native virtual/interface
	 * calls and funcptr binding targets), inherited system base classes, and
	 * native virtual functions overridden by the module. All entries are
	 * GetDeclaration-style declaration strings.
	 */
	struct FModApiUsage
	{
		TArray<FString> SystemCallDeclarations;
		TArray<FString> BaseTypeDeclarations;
		TArray<FString> OverrideDeclarations;
	};

	/**
	 * Returns true when the module violates the allowlist; OutError receives
	 * the diagnostic message.
	 */
	using FAuditHandler = TFunction<bool(const FString& ModuleName, const FModApiUsage& Usage, FString& OutError)>;

	/**
	 * Registers an audit handler. Must be called on the game thread before
	 * any compile batch runs (module loading). Returns a handle usable with
	 * Unregister.
	 */
	static ANGELSCRIPTCODE_API FDelegateHandle Register(FAuditHandler Handler);

	/** Unregisters a previously registered handler by its handle. */
	static ANGELSCRIPTCODE_API void Unregister(FDelegateHandle Handle);
};

/**
 * Registration point CS.Runtime.Modding.ScriptExecutionBudgetGate.
 *
 * Cooked (Test/Shipping) builds never install the built-in loop-detection
 * callback, so script execution cannot time out there. This registration
 * point lets ModKit supply a wall-clock execution budget per context
 * creation: when a handler returns a positive budget the context gets a
 * registered loop-detection callback that interrupts over-budget script
 * calls through the stock exception path and reports each interrupt to the
 * registered timeout handlers. An empty registry keeps the upstream behavior
 * bit-for-bit: no callback is ever installed and cooked builds keep no
 * execution timeout.
 */
struct FAngelscriptModdingScriptExecutionBudget
{
	/**
	 * Returns the execution budget in seconds for a freshly created context,
	 * or 0.0 to leave the context alone (the built-in callback stays in
	 * Development/editor builds and no callback is installed elsewhere).
	 */
	using FQueryBudget = TFunction<float(const asIScriptContext& Context)>;

	/**
	 * Receives a budget overrun: ModuleName is the module of the currently
	 * executing function (or "unknown" when it cannot be resolved) and
	 * ElapsedSeconds is the wall-clock time spent since the first sample.
	 */
	using FTimeoutHandler = TFunction<void(const FString& ModuleName, double ElapsedSeconds)>;

	/** Registers the budget query. Returns a handle usable with Unregister. */
	static ANGELSCRIPTCODE_API FDelegateHandle Register(FQueryBudget Handler);

	/** Unregisters a previously registered budget query by its handle. */
	static ANGELSCRIPTCODE_API void Unregister(FDelegateHandle Handle);

	/** Registers a timeout notification handler. Returns a handle. */
	static ANGELSCRIPTCODE_API FDelegateHandle RegisterTimeout(FTimeoutHandler Handler);

	/** Unregisters a previously registered timeout handler by its handle. */
	static ANGELSCRIPTCODE_API void UnregisterTimeout(FDelegateHandle Handle);
};

/**
 * Registration point CS.Runtime.Modding.PrecompiledDataTransformGate.
 *
 * Gates the single-file precompiled-data save/load surface
 * (FAngelscriptPrecompiledData::Save / Load in StaticJIT/PrecompiledData.cpp)
 * behind a ModKit-supplied cache transform: encryption/decryption, integrity
 * verification, opcode permutation and debug-information stripping .
 * The hook bodies are purely mechanical - they query the registry, receive a
 * strategy (FPolicy) plus the raw cache bytes, and apply the registered
 * handler's byte transform. Salt generation, header parsing, KDF and key
 * derivation all live on the ModKit handler side. An empty registry keeps the
 * upstream behavior bit-for-bit: every block body is skipped and Save/Load
 * write and read the exact upstream bytes.
 */
struct FAngelscriptModdingPrecompiledDataTransform
{
	enum class EDirection : uint8 { Save, Load };

	/**
	 * Transform strategy filled by the registered handler for one direction.
	 * bTransform=false means the cache stays byte-for-byte upstream (plaintext
	 * passthrough). When true, Key/Salt/HeaderFragment are the derived key
	 * material (ModKit side) and OpcodePermutation is the 256-entry opcode map
	 * (0-219 bijection, 220-255 identity; empty = no permutation).
	 */
	struct FPolicy
	{
		bool bTransform = false;
		/**
 * when the runtime memory-integrity check
 * has recorded a breach, the policy handler returns bBlocked=true so
 * the bytes handler fails closed (Save invalidates the artifact,
 * Load discards the cache) with MODKIT_CACHE_TRANSFORM_BLOCKED.
 */
		bool bBlocked = false;
		FString KeyId;
		FString RuntimeModule;
		TArray<uint8> HeaderFragment;
		TArray<uint8> ExpectedRuntimeHash; // 32B, keystore-registered fingerprint
		TArray<uint8> Salt;                // 16B per-cache nonce
		TArray<uint8> Key;                 // 32B derived key (ModKit side)
		TArray<uint8> OpcodePermutation;   // 256B; empty = no opcode permutation
		bool bStripDebugInfo = false;
		/**
		 * Exact Runtime function identity (module + script name + serialized
		 * FunctionId), plus whether the cache record is an entry point that must
		 * keep VM bytecode. Empty predicate and VM-entry-required records never
		 * strip bytecode.
		 */
		TFunction<bool(const FString& ModuleName, const FString& FunctionIdentity, bool bVmEntryRequired)> ShouldStripBytecode;
		/** Reports mechanical save-time strip outcomes to the registered strategy. */
		TFunction<void(int32 Stripped, int32 VmRequiredRetained, int32 UnmatchedRetained, int32 AmbiguousRetained)> OnBytecodeStripComplete;
	};

	/** Fills OutPolicy for a direction. Returns false when the registry is empty. */
	using FPolicyHandler = TFunction<bool(EDirection Direction, FPolicy& OutPolicy)>;

	/**
	 * Transforms the raw cache bytes for a direction. Save: encrypt + prepend
	 * the MKCC header. Load: parse the header, verify and decrypt, returning
	 * false on rejection (caller discards the cache). Returns true and leaves
	 * Bytes unchanged for plaintext passthrough. Policy may be updated with
	 * the derived key/permutation after a successful Load.
	 */
	using FBytesHandler = TFunction<bool(EDirection Direction, FPolicy& Policy, TArray<uint8>& Bytes)>;

	/** Registers the policy + bytes handlers. Returns a handle usable with Unregister. */
	static ANGELSCRIPTCODE_API FDelegateHandle Register(FPolicyHandler PolicyHandler, FBytesHandler BytesHandler);

	/** Unregisters a previously registered transform handler by its handle. */
	static ANGELSCRIPTCODE_API void Unregister(FDelegateHandle Handle);
};

/**
 * Registration point CS.Runtime.Modding.RuntimeMemoryIntegrityGate.
 *
 * Lets a ModKit handler run periodic runtime-integrity checks from two
 * mechanical call sites in FAngelscriptManager: the end of the per-frame
 * Tick and the post-compile / pre-swap-in decision point . All
 * policy (sampling, hashing, delayed-degradation response) lives on the
 * ModKit handler side. An empty registry keeps the upstream behavior
 * bit-for-bit: both call sites skip the block body entirely.
 */
struct FAngelscriptModdingMemoryIntegrity
{
	using FCheckHandler = TFunction<void()>;

	/** Registers an integrity check. Returns a handle usable with Unregister. */
	static ANGELSCRIPTCODE_API FDelegateHandle Register(FCheckHandler Handler);

	/** Unregisters a previously registered check by its handle. */
	static ANGELSCRIPTCODE_API void Unregister(FDelegateHandle Handle);
};

/**
 * Registration point CS.Runtime.Modding.StaticJITGenerationGate.
 *
 * Gates the static JIT output generation surface
 * (FAngelscriptStaticJIT::WriteOutputCode) behind a ModKit-supplied
 * generation policy . The hook bodies are purely mechanical - they
 * query the registry once at the top of WriteOutputCode, receive a generation
 * policy (all-or-nothing whole-batch skip decision, content-addressed DataGuid,
 * redirected GenDir and ModKit control-directory manifest path), apply the
 * GenDir redirect plus a relative-include rewrite pass, and notify the handler
 * of the final output set so it can write the closure manifest. An empty
 * registry keeps the upstream behavior bit-for-bit: every block body is
 * skipped, GenDir stays FPaths::RootDir()/AS_JITTED_CODE, the InfoFile is
 * rewritten unconditionally and DataGuid stays the constructor NewGuid.
 */
struct FAngelscriptModdingStaticJITGenerationGate
{
	/**
	 * Generation policy filled by the registered handler for one
	 * WriteOutputCode pass. bSkipBatch=true makes the Runtime skip the entire
	 * generation compute + write surface (all-or-nothing) after assigning
	 * DataGuid; the decision must be fail-closed on the handler side (a
	 * missing manifest, a hash failure or a missing recorded file must never
	 * request a skip).
	 */
	struct FPolicy
	{
		bool bApply = false;          // registry present and policy valid
		bool bSkipBatch = false;      // all-or-nothing whole-batch skip
		FString JitOutputDirectory;   // empty = engine-root default (upstream)
		FString ManifestPath;         // ModKit control-directory manifest
		FGuid DataGuid;               // content-addressed (invalid = keep ctor value)
		TMap<FString, FGuid> ModuleDataGuids; // independent cache/JIT identity per module
	};

	/** Fills OutPolicy for the current pass. Returns false when the registry is empty. */
	using FPolicyHandler = TFunction<bool(
		const TArray<FString>& ModuleNames,
		const TArray<FString>& RelativeFilenames,
		int32 EngineBuildIdentifier,
		FPolicy& OutPolicy)>;

	/**
	 * Receives the final output set of a full generation pass so the handler
	 * can write the closure manifest (closure hash + output file list + JIT
	 * coverage set). Runs only after the write surface completed.
	 */
	using FCompleteHandler = TFunction<void(
		const FString& ManifestPath,
		const TArray<FString>& OutputFilenames,
		const TArray<FString>& CoveredModuleNames,
		const TArray<FString>& CoveredFunctionIdentities)>;

	/** Registers the policy + completion handlers. Returns a handle usable with Unregister. */
	static ANGELSCRIPTCODE_API FDelegateHandle Register(FPolicyHandler PolicyHandler, FCompleteHandler CompleteHandler);

	/** Unregisters a previously registered policy handler by its handle. */
	static ANGELSCRIPTCODE_API void Unregister(FDelegateHandle Handle);
};
