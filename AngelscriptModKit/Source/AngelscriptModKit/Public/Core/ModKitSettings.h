// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned configuration for the AngelscriptModKit module.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ModKitSettings.generated.h"

UCLASS(Config=Engine, DefaultConfig)
class ANGELSCRIPTMODKIT_API UModKitSettings : public UObject
{
	GENERATED_BODY()

public:
	/** Process policy captured before ModKit registers its runtime consumers. Restart to apply config edits. */
	static const UModKitSettings* Get();
	bool IsCapabilityDenied(const FString& Declaration) const;

	/**
	 * Script-root-relative path prefixes whose sources are excluded from the
	 * precompiled cache and compiled from source at startup instead. Entries
	 * are matched against module relative filenames (for example
	 * "DevOnly" covers "DevOnly/Foo.as"). The built-in mod discovery root
	 * "FeatureMods" and "Plugins/FeatureMods" are always source-only and do
	 * not need to be listed here.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true, DeprecatedProperty, DeprecationMessage = "Legacy allowlist setting has no effect"))
	TArray<FString> SourceOnlyScriptPaths;

	// Legacy fields are retained temporarily for existing editor code and tests.
	// They do not grant access or restrict official mods.
	UPROPERTY(Config, meta = (DeprecatedProperty, DeprecationMessage = "Legacy allowlist setting has no effect"))
	TArray<FString> AllowedApiPatterns;
	UPROPERTY(Config, meta = (DeprecatedProperty, DeprecationMessage = "Legacy allowlist setting has no effect"))
	TArray<FString> AllowedApiTiers;
	UPROPERTY(Config, meta = (DeprecatedProperty, DeprecationMessage = "Legacy allowlist setting has no effect"))
	TArray<FString> AllowedApiTags;
	UPROPERTY(Config, meta = (DeprecatedProperty, DeprecationMessage = "Legacy allowlist setting has no effect"))
	TArray<FString> AllowedApiModules;
	UPROPERTY(Config, meta = (DeprecatedProperty, DeprecationMessage = "Legacy allowlist setting has no effect"))
	TArray<FString> PublicBaseClassPatterns;
	UPROPERTY(Config, meta = (DeprecatedProperty, DeprecationMessage = "Legacy allowlist setting has no effect"))
	TArray<FString> AllowedOverridePatterns;

	/**
	 * Default patterns ship in Config/DefaultEngine.ini. Projects append with
	 * +DeniedApiPatterns and remove a default with -DeniedApiPatterns; internal
	 * APIs always remain denied.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	TArray<FString> DeniedApiPatterns;

	/** Native base types rejected for player mods. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	TArray<FString> DeniedBaseClassPatterns;

	/** Native overrides rejected for player mods. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	TArray<FString> DeniedOverridePatterns;

	/**
	 * Per-mod startup activation timeout in seconds. Every uplugin mod gets
	 * its own deadline; a mod that has not reached the Active state by then
	 * is disabled with MODKIT_MOD_DISABLED (UX-D2) while the game keeps
	 * starting. Bad mods never drag the whole startup window.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true, ClampMin = "0.1"))
	float ModStartupActivationTimeoutSec = 10.0f;

	/**
	 * v1 reserve only: when set, unload refuses with
	 * MODKIT_CONFIG_ERROR reason=force-destroy-not-implemented instead of
	 * destroying instances. The destructive force-destroy path is not
	 * implemented yet (v2 design); the runtime unload always refuses live
	 * instances instead.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bForceDestroyInstancesOnUnload = false;

	/**
	 * Cooked (Test/Shipping) builds never install the built-in loop-detection
	 * callback, so a script that runs away can hang the game forever. When
	 * this budget is positive, the ModKit handler installs its own
	 * loop-detection callback that interrupts any script call that exceeds
	 * the budget (wall-clock seconds) through the stock exception path.
	 * Editor and Development builds already have the built-in 1s callback, so
	 * this budget only takes effect there when bForceModKitTimeoutInDev is on.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true, ClampMin = "0.1"))
	float ModScriptExecutionTimeoutSec = 5.0f;

	/**
	 * When true, MaxConsecutiveTimeoutsBeforeDisable consecutive timeouts of
	 * the same mod disable it with MODKIT_MOD_DISABLED reason=script-timeout
	 * (UX-D2). The consecutive count accumulates per mod for the process
	 * lifetime and stops counting once the mod is disabled.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bDisableModOnTimeout = true;

	/**
	 * Consecutive script-execution timeouts of one mod that trigger the
	 * script-timeout disable path when bDisableModOnTimeout is set.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true, ClampMin = "1"))
	int32 MaxConsecutiveTimeoutsBeforeDisable = 3;

	/**
	 * Validation switch (not a delivery configuration): Development binaries
	 * already install the built-in 1s loop-detection callback, so ModKit
	 * timeouts never fire there. When true the ModKit handler returns its own
	 * budget and the ModKit callback REPLACES the built-in one
	 * (SetLoopDetectionCallback is a single slot), emitting
	 * MODKIT_TIMEOUT_FORCE_ACTIVE. Defaults to false = bit-for-bit upstream
	 * behavior in Development builds.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bForceModKitTimeoutInDev = false;

	/** Enable authenticated offline cache envelopes and immutable startup file views. Missing keys, plaintext at a protected path and failed authentication reject encrypted delivery. Generation writes native plaintext for subsequent offline sealing. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bEncryptScriptCache = false;

	/**
	 * The keystore entry id consumed for the precompiled cache transform. The
	 * key material itself never lives in ini; only this id does.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	FString ScriptCacheKeyId;

	/** Legacy serializer request. Debug stripping is unsupported by the stock-cache envelope path; generation and sealing reject this setting when enabled. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bStripLineNumbersOnCache = false;

	/** Legacy serializer request. Opcode permutation is unsupported by the stock-cache envelope path; generation and sealing reject this setting when enabled. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bObfuscateOpcode = false;

	/**
	 * KDF runtime binding (MK-Q02): the load side explicitly compares the
	 * runtime module's disk .text SHA-256 against the keystore-registered
	 * expectedRuntimeHash (mismatch -> MODKIT_CACHE_KEY_MISMATCH
	 * reason=runtime-hash, fail-closed) and the matched value feeds the KDF.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bBindCacheToRuntimeHash = true;

	/**
	 * Module name resolved by the runtime-hash provider (the Angelscript
	 * runtime module is AngelscriptCode; the actual DLL is
	 * AngelscriptCode[-Win64-<Config>].dll). Monolithic builds fall back to the
	 * main executable.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	FString RuntimeModuleName = TEXT("AngelscriptCode");

	/**
	 * Legacy validation setting. Plaintext rejection depends only on
	 * bEncryptScriptCache and applies in every build configuration; this flag
	 * does not enable or bypass it. Rejected plaintext reports
	 * MODKIT_CACHE_PLAINTEXT_REJECTED.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bForceRejectPlaintextInDev = false;

	/**
 
 * handler is never registered and both runtime call sites stay no-ops.
 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bMemoryIntegrityCheck = false;

	/**
	 * Seconds between integrity checks on the Tick entry (interval sampling,
	 * S5); the compile-batch entry and the force switch bypass the interval.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	float MemoryIntegrityIntervalSec = 10.0f;

	   
                                                                        
                                                                         
                                   
    
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bForceIntegrityCheckInDev = false;

	/** Legacy core output redirect. Non-empty values are rejected during generation; delivery tools collect the actual isolated RootDir/AS_JITTED_CODE output. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	FString JitOutputDirectory;

	/** Legacy independent generation selection. Non-empty values are rejected; native-root staging currently requires the same complete cache and JIT input sets. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	TArray<FString> JitOnlyModulePrefixes;

	/** Legacy strip request. Actual generated registration IDs do not establish VM-entry or bytecode-removal safety. Generation and sealing reject this setting when enabled. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bStripBytecodeForJitModules = false;

	/**
 
 * is inert (fail-closed). Patch fetch/land/apply never run without it.
 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bEnableHotPatch = false;

	/**
	 * KeyId for PatchPublicKeyBase64 in project DefaultEngine.ini. Shipping
	 * delivery trust is compiled from that file and requires rebuilding.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	FString PatchVerifyKeyId = TEXT("patch-verify-v1");

	/** Additional accepted ids for external FeatureMod package signatures. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	TArray<FString> PatchAcceptedKeyIds;

	/**
	 * Base64 of the 32-byte packed Ed25519 public key (RFC 8032 point
	 * encoding; see FModKitPatchSignature::DecodePublicKey). The test private
	 * key exists only in the fixture generator and never ships with the product.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	FString PatchPublicKeyBase64;

	/** Only load signed official external FeatureMods. Restart to apply. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bOnlyOfficialExternalMods = false;

	/** Ask the host game's UI for approval before the first player FeatureMod load. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bConfirmPlayerModsOnFirstLoad = true;

	/** Restrict official external FeatureMods to the player API allowlist as well. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true, DeprecatedProperty, DeprecationMessage = "Legacy allowlist setting has no effect"))
	bool bApplyAllowlistToOfficialMods = false;

	/** Development override for loading a lower external Mod version. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bAllowFeatureModDowngrade = false;

	/**
 * Default hot-patch feed for the runtime fetch entry point .
 * A bare path or file:// URL names a local feed directory (ops fixture
 * chain); an http(s):// URL is fetched with the HTTP module. Empty means
 * no default feed; the fetch entry point then requires an explicit feed.
 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	FString PatchFeedUrl;

	/**
	 * Hot-patch staging root override. Empty selects the default
	 * <ProjectSaved>/ModKitPatches, which is never a script root, never under
	 * FeatureMods/, and never inside any plugin content tree (red gate
	 * staging-root-excluded).
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	FString PatchStagingRoot;

	/**
	 * Maximum total bytes of one hot-patch fetch (manifest + payload files).
	 * A feed declaring more is rejected with reason=size before the payload
	 * bodies are buffered.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	int64 PatchMaxBytes = 64 * 1024 * 1024;

	/** Per-request timeout in seconds for http(s) patch fetches. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	int32 PatchFetchTimeoutSec = 30;

	/**
 * Keep the staged directory of a failed patch apply for forensics 
 . Default true (keep); false deletes the staged tree on
 * MODKIT_PATCH_FAILED. A rejected apply (no compile attempt) never
 * deletes anything.
 */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit", meta = (ConfigRestartRequired = true))
	bool bKeepFailedPatchOnDisk = true;
};
