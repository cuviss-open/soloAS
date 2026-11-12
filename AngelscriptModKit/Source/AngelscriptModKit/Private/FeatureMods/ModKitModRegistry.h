// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned startup mod discovery and GameFeaturePlugin activation
// orchestration for the AngelscriptModKit module.

#pragma once

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"
#include "Containers/Ticker.h"
#include "GameFeaturesProjectPolicies.h"
#include "ModKitModRegistry.generated.h"

namespace UE
{
	namespace GameFeatures
	{
		struct FResult;
	}
}

UCLASS()
class ANGELSCRIPTMODKIT_API UModKitGameFeaturesProjectPolicies : public UDefaultGameFeaturesProjectPolicies
{
	GENERATED_BODY()

public:
// SOLOAS-COMPAT-BEGIN(5.7) UE 5.7 has no missing-data policy virtual [test: 5.7 Editor compile] [id: CS.Compatibility.ModKit.ModKitModRegistry.MissingDataPolicy]
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
	virtual bool ShouldAllowMissingGameFeatureData(const FString& PluginName) const override;
#endif
// SOLOAS-COMPAT-END
};

DECLARE_MULTICAST_DELEGATE_OneParam(FModKitPlayerModApprovalRequired, const FString&);

/**
 * Mod identity and activation metadata. The FeatureMods/<Name> directory and
 * matching .uplugin filename provide the stable plugin ID.
 */
enum class EModKitModTrust : uint8
{
	BuiltIn,
	Official,
	Player,
};

struct FModKitSourceOwner;

struct FModKitModInfo
{
	FString DirectoryName;
	FString DirectoryPath;
	FString RelativeRoot;
	FString PluginPath;
	FString PluginURL;
	FString DisplayName;
	FString VersionName;
	bool bHasUPlugin = false;
	FString Revision;
	FString ConfigText;
	TArray<FString> LeafDependencies;
	EModKitModTrust Trust = EModKitModTrust::Player;
};

struct FModKitModVersion
{
	FString Name;
	FString Version;
};

/**
 * Startup mod discovery and GameFeaturePlugin activation orchestration 
 .
 *
 * Discovers FeatureMods/<Name>/<Name>.uplugin and Plugins/FeatureMods/<Name>/
 * <Name>.uplugin FeatureMods, identity-validates them (filename must equal the
 * directory name; the uplugin marker and FeatureMod.json version are required;
 * directory names containing ".as" or duplicate IDs are rejected with
 * MODKIT_CONFIG_ERROR), and starts activation of every FeatureMod
 * through UGameFeaturesSubsystem::LoadAndActivateGameFeaturePlugin with a
 * per-mod timeout. A core ticker (never a blocking wait inside
 * OnPostEngineInit) watches for settlement; once every uplugin mod reached a
 * terminal state the registry reports settlement and the append rendezvous in
 * FModKitCompilationManager proceeds. The activated FeatureMod roster drives
 * append compilation; disabled units stay out so their
 * scripts never compile (UX-D2).
 */
class FModKitModRegistry
{
public:
	static void Startup();
	static bool IsModSourcePath(const FString& Path);
	static bool ResolveSourceOwner(const FString& Path, FModKitSourceOwner& Owner, FString& Error);
	static bool IsSourceOwnerCurrent(const FModKitSourceOwner& Owner);
	static void Shutdown();

	/** Initiates discovery and activation. Called once from OnPostEngineInit. */
	static void StartActivation();

	/** True once every uplugin mod reached a terminal state (or none existed). */
	static bool IsActivationSettled();

	/**
	 * Roster predicate for the append compile set: true for activated FeatureMods;
	 * false for identity-rejected, activation-failed, and timed-out FeatureMods
	 * so disabled scripts never
	 * produce a MODKIT_APPEND_MODULE line (invariant with MODKIT_MOD_DISABLED).
	 */
	static bool ShouldCompileMod(const FString& ModDirectoryName);
	static void GetActivatedModNames(TArray<FString>& OutModNames);

	/** Allows content-free GameFeatureData only while a validated FeatureMod is activating. */
	static bool ShouldAllowMissingGameFeatureData(const FString& PluginName);

	// --- Runtime load/unload support ---

	/**
	 * Discovers a single mod directory on demand and adds it to the roster.
	 * Returns false when the directory is missing or fails FeatureMod identity
	 * validation. A valid FeatureMod is added in the Discovered state. An
	 * existing entry (startup roster or a previously unloaded FeatureMod) is
	 * returned as-is so a reload attempt can retry activation.
	 */
	static bool RescanOrAdd(const FString& ModDirectoryName);

	/**
	 * Starts async activation of a FeatureMod through
	 * LoadAndActivateGameFeaturePlugin and ensures the activation pump is
	 * running. Returns false when the entry is not a FeatureMod or the
	 * GameFeatures subsystem is unavailable (the entry is then disabled).
	 */
	static bool StartRuntimeActivation(const FString& ModDirectoryName);

	/** Deactivates a FeatureMod through GameFeatures. */
	static void DeactivateMod(const FString& ModDirectoryName);

	/** True when the mod is currently Activated on the roster. */
	static ANGELSCRIPTMODKIT_API bool IsModActivated(const FString& ModDirectoryName);

	/** Re-read identity and signed payload/floor without changing the active generation. */
	static bool PrepareReload(const FString& ModDirectoryName, FString& OutReason);
	static void CompleteReload(const FString& ModDirectoryName, bool bSuccess, bool bNativeReloadFailed);

	/** Returns active FeatureMods in stable plugin-ID order for save compatibility checks. */
	static ANGELSCRIPTMODKIT_API void GetActiveModVersions(TArray<FModKitModVersion>& OutVersions);

	/** Returns a discovered FeatureMod's script directory and workspace-relative root. */
	static ANGELSCRIPTMODKIT_API bool GetModScriptPaths(const FString& ModDirectoryName, FString& OutAbsoluteDirectory,
		FString& OutRelativeRoot);

	/** True once the mod reached a terminal state (Activated/Failed/Unloaded). */
	static bool IsModTerminal(const FString& ModDirectoryName);

	/** True when the mod is disabled (Failed). */
	static bool IsModFailed(const FString& ModDirectoryName);

	/** Unknown identities are untrusted; no verification or floor writes occur here. */
	static ANGELSCRIPTMODKIT_API EModKitModTrust GetModTrust(const FString& ModDirectoryName);

	/** Persistent host-game consent. Revocation applies to subsequent loads. */
	static ANGELSCRIPTMODKIT_API bool IsPlayerModApprovalPending(const FString& ModDirectoryName);
	static ANGELSCRIPTMODKIT_API bool IsPlayerModApproved(const FString& ModDirectoryName);
	static ANGELSCRIPTMODKIT_API bool ApprovePlayerMod(const FString& ModDirectoryName);
	static ANGELSCRIPTMODKIT_API bool RevokePlayerMod(const FString& ModDirectoryName);
	static ANGELSCRIPTMODKIT_API FModKitPlayerModApprovalRequired OnPlayerModApprovalRequired;

	/** True when a script timeout permanently disabled this mod for the process. */
	static bool IsTimeoutDisabled(const FString& ModDirectoryName);

	/** True when the FeatureMod has a valid plugin descriptor. */
	static bool IsUPluginMod(const FString& ModDirectoryName);

	/** Marks a runtime-loaded mod as unloaded (leaves the compile roster). */
	static void MarkModUnloaded(const FString& ModDirectoryName);

	/** Disables a mod with MODKIT_MOD_DISABLED (UX-D2). */
	static void MarkModFailed(const FString& ModDirectoryName, const TCHAR* Reason);

private:
	enum class EModActivationState : uint8
	{
		Discovered,
		AwaitingApproval,
		Activating,
		Activated,
		Failed,
		Unloaded,
	};

	struct FModEntry
	{
		FModKitModInfo Info;
		EModActivationState State = EModActivationState::Discovered;
		double DeadlineSeconds = 0.0;
		bool bDisabledByTimeout = false;
		bool bReloadRequiresRestart = false;
		TOptional<FModKitModInfo> ReloadCandidate;
	};

	static FModEntry& AddEntry(FModKitModInfo&& Info);
	static FModEntry* FindEntry(const FString& ModDirectoryName);
	static void EnsureActivationTicker();
	static void RejectConfig(FModEntry& Entry, const TCHAR* Reason);
	static void DisableMod(FModEntry& Entry, const TCHAR* Reason);
	static void DiscoverMods();
	static bool ParseDescriptor(FModKitModInfo& Info, FString& OutReason);
	static void OnActivationCallback(const FString& DirectoryName, const FString& PluginURL, const UE::GameFeatures::FResult& Result);
	static bool OnActivationTick(float DeltaTime);
	static void CheckTimeouts(double NowSeconds);
	static bool AllSettled();
	static void SettleActivation();

	static TArray<FModEntry> Mods;
	static bool bActivationStarted;
	static bool bActivationSettled;
	static double ActivationStartTimeSeconds;
	static FTSTicker::FDelegateHandle TickerHandle;
};
