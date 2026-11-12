// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned startup mod discovery and GameFeaturePlugin activation
// orchestration for the AngelscriptModKit module.

#include "FeatureMods/ModKitModRegistry.h"
#include "Core/AngelscriptModKit.h"
#include "Misc/App.h"
#include "Misc/MessageDialog.h"

#include "FeatureMods/ModKitAssetReplacements.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "Core/ModKitSettings.h"
#include "FeatureMods/ModKitSignature.h"
#include "FeatureMods/ModKitRuntimeOps.h"
#include "Misc/ConfigCacheIni.h"
#include "Shared/Delivery/ModKitPatchManifest.h"

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "GameFeaturesSubsystem.h"
// SOLOAS-COMPAT-BEGIN(5.7) UE 5.7 only forward-declares GameFeatures results [test: 5.7 Editor compile] [id: CS.Compatibility.ModKit.ModKitRuntimeOps.ResultDefinition]
#include "GameFeaturePluginOperationResult.h"
// SOLOAS-COMPAT-END
#include "GameFeaturesSubsystemSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FModKitPlayerModApprovalRequired FModKitModRegistry::OnPlayerModApprovalRequired;
TArray<FModKitModRegistry::FModEntry> FModKitModRegistry::Mods;
bool FModKitModRegistry::bActivationStarted = false;
bool FModKitModRegistry::bActivationSettled = false;
double FModKitModRegistry::ActivationStartTimeSeconds = 0.0;
FTSTicker::FDelegateHandle FModKitModRegistry::TickerHandle;

// SOLOAS-COMPAT-BEGIN(5.7) UE 5.7 has no missing-data policy virtual [test: 5.7 Editor compile] [id: CS.Compatibility.ModKit.ModKitModRegistry.MissingDataPolicy]
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
bool UModKitGameFeaturesProjectPolicies::ShouldAllowMissingGameFeatureData(const FString& PluginName) const
{
	return FModKitModRegistry::ShouldAllowMissingGameFeatureData(PluginName);
}

#endif
// SOLOAS-COMPAT-END

namespace
{
	FFeatureModPlayerApprovalHandler PlayerApprovalHandler;
	FDelegateHandle StartupDiscoveryHandle;

	bool ReadFeatureModText(const FString& Path, FString& Out)
	{
		// Match discovery and plugin registration through the active file layer
		// (loose files, sandbox redirects, or mounted pak files). Signed package
		// verification and persistent version floors retain their physical I/O.
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		if (!Reader) return false;
		const int64 Size = Reader->TotalSize();
		if (Size < 0 || Size > FModKitPatchManifest::MaxManifestBytes) return false;
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(static_cast<int32>(Size));
		if (Size > 0) Reader->Serialize(Bytes.GetData(), Size);
		if (Reader->IsError()) return false;
		FFileHelper::BufferToString(Out, Bytes.GetData(), Bytes.Num());
		return FModKitPatchManifest::IsJsonWithinBudget(Out);
	}

	bool IsBuiltInSource(const FModKitModInfo& Info)
	{
		if (Info.RelativeRoot != TEXT("Plugins/FeatureMods")) return false;
#if WITH_EDITOR
		return true;
#else
		// Packaged project units are read through the game's container layer.
		// Loose additions cannot acquire BuiltIn permissions by borrowing that path.
		IPlatformFile& Physical = FPlatformFileManager::Get().GetPlatformPhysical();
		return !Physical.FileExists(*FPaths::ConvertRelativePathToFull(Info.PluginPath))
			&& !Physical.FileExists(*FPaths::ConvertRelativePathToFull(Info.DirectoryPath / TEXT("FeatureMod.json")));
#endif
	}

	bool IsValidModId(const FString& Name)
	{
		if (Name.IsEmpty()) return false;
		for (TCHAR Character : Name)
			if (!FChar::IsAlnum(Character) && Character != TCHAR('_') && Character != TCHAR('-')) return false;
		return true;
	}

	FString PlayerApprovalPath()
	{
		return FPaths::ProjectSavedDir() / TEXT("FeatureModApprovals.ini");
	}

	bool WritePlayerApproval(const FString& Name, bool bApproved)
	{
		if (!IsValidModId(Name)) return false;
		FConfigFile File;
		File.Read(PlayerApprovalPath());
		File.SetBool(TEXT("PlayerMods"), *Name, bApproved);
		return IFileManager::Get().MakeDirectory(*FPaths::ProjectSavedDir(), true)
			&& File.Write(PlayerApprovalPath());
	}

	bool EnsurePlayerApproval(const FString& Name, FString* OutReason = nullptr)
	{
		if (OutReason) *OutReason = TEXT("player-approval-required");
		if (!UModKitSettings::Get()->bConfirmPlayerModsOnFirstLoad) return true;
		if (!IsValidModId(Name)) return false;
		FConfigFile File;
		File.Read(PlayerApprovalPath());
		bool bApproved = false;
		if (File.GetBool(TEXT("PlayerMods"), *Name, bApproved))
		{
			if (!bApproved && OutReason) *OutReason = TEXT("player-approval-denied");
			return bApproved;
		}
		if (PlayerApprovalHandler.IsBound())
		{
			bApproved = PlayerApprovalHandler.Execute(Name);
		}
		else if (FModKitModRegistry::OnPlayerModApprovalRequired.IsBound())
		{
			FModKitModRegistry::OnPlayerModApprovalRequired.Broadcast(Name);
			if (FModKitModRegistry::IsPlayerModApproved(Name)) return true;
			if (OutReason && (IsRunningCommandlet() || FApp::IsUnattended() || !FApp::CanEverRender()))
				*OutReason = TEXT("player-approval-unavailable");
			return false;
		}
		else
		{
			if (IsRunningCommandlet() || FApp::IsUnattended()
				|| !FApp::CanEverRender())
			{
				if (OutReason) *OutReason = TEXT("player-approval-unavailable");
				UE_LOG(LogAngelscriptModKit, Warning, TEXT("MODKIT_PLAYER_APPROVAL_UNAVAILABLE name=%s: interactive consent required; load refused"), *Name);
				return false;
			}
			bApproved = FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No,
				FText::Format(NSLOCTEXT("FeatureMods", "PlayerConsent", "Allow player FeatureMod '{0}' to run? Only allow code from authors you trust. Your choice is saved for this project."), FText::FromString(Name))) == EAppReturnType::Yes;
		}
		if (!bApproved && OutReason) *OutReason = TEXT("player-approval-denied");
		if (!WritePlayerApproval(Name, bApproved))
		{
			if (OutReason) *OutReason = TEXT("player-approval-write-failed");
			return false;
		}
		return bApproved;
	}

	struct FFeatureModRoot
	{
		FString RelativePath;
		FString AbsolutePath;
	};

	TArray<FFeatureModRoot> GetFeatureModRoots()
	{
		TArray<FFeatureModRoot> Roots;
		FFeatureModRoot& ExternalRoot = Roots.AddDefaulted_GetRef();
		ExternalRoot.RelativePath = TEXT("FeatureMods");
		ExternalRoot.AbsolutePath = FPaths::ProjectDir() / TEXT("FeatureMods");

		FFeatureModRoot& ProjectPluginRoot = Roots.AddDefaulted_GetRef();
		ProjectPluginRoot.RelativePath = TEXT("Plugins/FeatureMods");
		ProjectPluginRoot.AbsolutePath = FPaths::ProjectPluginsDir() / TEXT("FeatureMods");
		return Roots;
	}
}

void SetFeatureModPlayerApprovalHandler(FFeatureModPlayerApprovalHandler Handler)
{
	check(IsInGameThread());
	PlayerApprovalHandler = MoveTemp(Handler);
}

void FModKitModRegistry::Startup()
{
	// AddToPluginsList mutates the map being traversed while PreDefault modules
	// start. The phase-complete event is outside that traversal and still before
	// the PostDefault AngelscriptLoader initializes the VM.
	StartupDiscoveryHandle = IPluginManager::Get().OnLoadingPhaseComplete().AddLambda(
		[](ELoadingPhase::Type Phase, bool bSuccess)
		{
			if (Phase != ELoadingPhase::PreDefault || !bSuccess) return;
			IPluginManager::Get().OnLoadingPhaseComplete().Remove(StartupDiscoveryHandle);
			StartupDiscoveryHandle.Reset();
			if (FAngelscriptManager::IsInitialized())
				UE_LOG(LogAngelscriptModKit, Fatal, TEXT("MODKIT_DISCOVERY_TOO_LATE: FeatureMods must be discovered before VM initialization"));
			DiscoverMods();
			UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_DISCOVERY_COMPLETE phase=PreDefault vmInitialized=0 mods=%d"), Mods.Num());
		});
	// Stock GameFeatures rejects a plugin without GameFeatureData by default.
	// Use the ModKit policy for source-only FeatureMods unless the project chose
	// a different policy itself.
	UGameFeaturesSubsystemSettings* Settings = GetMutableDefault<UGameFeaturesSubsystemSettings>();
	const FSoftClassPath DefaultPolicyPath(UDefaultGameFeaturesProjectPolicies::StaticClass()->GetPathName());
	if (Settings->GameFeaturesManagerClassName.IsNull() || Settings->GameFeaturesManagerClassName == DefaultPolicyPath)
	{
		Settings->GameFeaturesManagerClassName = FSoftClassPath(UModKitGameFeaturesProjectPolicies::StaticClass()->GetPathName());
	}
}

bool FModKitModRegistry::IsModSourcePath(const FString& Path)
{
    FString Normalized = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Normalized);
    FPaths::CollapseRelativeDirectories(Normalized);
    for (const auto& Entry : GetFeatureModRoots())
    {
        FString Root = FPaths::ConvertRelativePathToFull(Entry.AbsolutePath);
        FPaths::NormalizeDirectoryName(Root);
        FPaths::CollapseRelativeDirectories(Root);
        if (FPaths::IsSamePath(Normalized, Root) || FPaths::IsUnderDirectory(Normalized, Root)) return true;
    }
    return false;
}

bool FModKitModRegistry::ResolveSourceOwner(const FString& Path, FModKitSourceOwner& Owner, FString& Error)
{
	bool bFound = false;
	for (const FModEntry& Entry : Mods)
	{
		const FModKitModInfo& Info = Entry.ReloadCandidate.IsSet() ? Entry.ReloadCandidate.GetValue() : Entry.Info;
		FString Root = FPaths::ConvertRelativePathToFull(Info.DirectoryPath / TEXT("Script"));
		FPaths::NormalizeFilename(Root);
		FPaths::CollapseRelativeDirectories(Root);
#if PLATFORM_WINDOWS
		Root.ToLowerInline();
#endif
		if (!Path.StartsWith(Root + TEXT("/"), ESearchCase::CaseSensitive)) continue;
		if (bFound) { Error = TEXT("MODKIT_SOURCE_OWNER overlapping FeatureMod roots"); return false; }
		if (Entry.State == EModActivationState::Failed || Entry.State == EModActivationState::AwaitingApproval
			|| (Info.Trust == EModKitModTrust::Player && !IsPlayerModApproved(Info.DirectoryName)))
		{ Error = TEXT("MODKIT_SOURCE_OWNER inactive or unapproved FeatureMod"); return false; }
		bFound = true;
		Owner.Id = Info.DirectoryName;
		Owner.ScriptRoot = Root;
		Owner.Revision = Info.Revision;
		Owner.Layer = EModKitSourceLayer::Leaf;
		Owner.Trust = Info.Trust;
		Owner.LeafDependencies = Info.LeafDependencies;
		if (Info.Trust == EModKitModTrust::Official) Owner.VerifiedConfig = Info.ConfigText;
	}
	return bFound;
}

bool FModKitModRegistry::IsSourceOwnerCurrent(const FModKitSourceOwner& Owner)
{
	FModKitSourceOwner Current;
	FString Error;
	return ResolveSourceOwner(Owner.ScriptRoot / TEXT("__owner_probe.as"), Current, Error)
		&& Current.Id == Owner.Id && Current.Revision == Owner.Revision;
}

void FModKitModRegistry::Shutdown()
{
	if (StartupDiscoveryHandle.IsValid())
	{
		IPluginManager::Get().OnLoadingPhaseComplete().Remove(StartupDiscoveryHandle);
		StartupDiscoveryHandle.Reset();
	}
	OnPlayerModApprovalRequired.Clear();
	PlayerApprovalHandler.Unbind();
	FModKitSignature::Reset();
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	Mods.Reset();
	bActivationStarted = false;
	bActivationSettled = false;
}

FModKitModRegistry::FModEntry& FModKitModRegistry::AddEntry(FModKitModInfo&& Info)
{
	Mods.Emplace();
	FModEntry& Entry = Mods.Last();
	Entry.Info = MoveTemp(Info);
	return Entry;
}

FModKitModRegistry::FModEntry* FModKitModRegistry::FindEntry(const FString& ModDirectoryName)
{
	for (FModEntry& Entry : Mods)
	{
		if (Entry.Info.DirectoryName == ModDirectoryName)
		{
			return &Entry;
		}
	}
	return nullptr;
}

void FModKitModRegistry::EnsureActivationTicker()
{
	if (TickerHandle.IsValid())
	{
		return;
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateStatic(&FModKitModRegistry::OnActivationTick), 0.1f);
}

void FModKitModRegistry::RejectConfig(FModEntry& Entry, const TCHAR* Reason)
{
	Entry.State = EModActivationState::Failed;
	UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR mod=%s reason=%s"), *Entry.Info.DirectoryName, Reason);
}

void FModKitModRegistry::DisableMod(FModEntry& Entry, const TCHAR* Reason)
{
	Entry.State = EModActivationState::Failed;
	Entry.bDisabledByTimeout |= FCString::Strcmp(Reason, TEXT("script-timeout")) == 0;
	FModKitAssetReplacements::RemoveForMod(Entry.Info.DirectoryName);
	UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_MOD_DISABLED name=%s reason=%s"), *Entry.Info.DirectoryName, Reason);
}

void FModKitModRegistry::DiscoverMods()
{
	Mods.Reset();

	IFileManager& FileManager = IFileManager::Get();
	TArray<FModKitModInfo> Candidates;
	for (const FFeatureModRoot& Root : GetFeatureModRoots())
	{
		if (!FileManager.DirectoryExists(*Root.AbsolutePath))
		{
			continue;
		}

		TArray<FString> ModDirectories;
		FileManager.FindFiles(ModDirectories, *(Root.AbsolutePath / TEXT("*")), false, true);
		ModDirectories.Sort();
		for (const FString& ModDirectory : ModDirectories)
		{
			FModKitModInfo& Info = Candidates.AddDefaulted_GetRef();
			Info.DirectoryName = ModDirectory;
			Info.DirectoryPath = Root.AbsolutePath / ModDirectory;
			Info.RelativeRoot = Root.RelativePath;
			Info.DisplayName = ModDirectory;
		}
	}

	TMap<FString, int32> IdCounts;
	for (const FModKitModInfo& Info : Candidates)
	{
		++IdCounts.FindOrAdd(Info.DirectoryName.ToLower());
	}

	for (FModKitModInfo& Info : Candidates)
	{
		const FString& ModDirectory = Info.DirectoryName;

		if (ModDirectory.Contains(TEXT(".as")))
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR mod=%s reason=directory-name-contains-as"), *ModDirectory);
			RejectConfig(AddEntry(MoveTemp(Info)), TEXT("config-error"));
			continue;
		}
		if (IdCounts.FindRef(ModDirectory.ToLower()) > 1)
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR mod=%s reason=duplicate-featuremod-id"), *ModDirectory);
			RejectConfig(AddEntry(MoveTemp(Info)), TEXT("duplicate-featuremod-id"));
			continue;
		}

		TArray<FString> DescriptorFiles;
		FileManager.FindFiles(DescriptorFiles, *(Info.DirectoryPath / TEXT("*.uplugin")), true, false);

		// The plugin filename is its stable ID and must match the folder.
		const FString ExpectedDescriptor = ModDirectory + TEXT(".uplugin");
		if (DescriptorFiles.Num() == 1 && DescriptorFiles[0] == ExpectedDescriptor)
		{
			Info.bHasUPlugin = true;
			Info.PluginPath = Info.DirectoryPath / DescriptorFiles[0];
			FString Reason;
			if (ParseDescriptor(Info, Reason))
			{
				FText PluginAddFailure;
				if (!IPluginManager::Get().AddToPluginsList(Info.PluginPath, &PluginAddFailure))
				{
					UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR mod=%s reason=plugin-registration-failed detail=%s"), *ModDirectory, *PluginAddFailure.ToString());
					RejectConfig(AddEntry(MoveTemp(Info)), TEXT("plugin-registration-failed"));
					continue;
				}
				Info.PluginURL = UGameFeaturesSubsystem::GetPluginURL_FileProtocol(Info.PluginPath);
				AddEntry(MoveTemp(Info));
				continue;
			}
			if (Reason == TEXT("player-approval-required"))
			{
				FModEntry& Entry = AddEntry(MoveTemp(Info));
				Entry.State = EModActivationState::AwaitingApproval;
				UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_PLAYER_APPROVAL_REQUIRED name=%s"), *ModDirectory);
				continue;
			}
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR mod=%s reason=%s"), *ModDirectory, *Reason);
			RejectConfig(AddEntry(MoveTemp(Info)), TEXT("featuremod-config-error"));
			continue;
		}

		const TCHAR* Reason = DescriptorFiles.Num() == 0 ? TEXT("featuremod-descriptor-missing") : TEXT("identity-mismatch");
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR mod=%s reason=%s"), *ModDirectory, Reason);
		RejectConfig(AddEntry(MoveTemp(Info)), Reason);
	}
}

bool FModKitModRegistry::ParseDescriptor(FModKitModInfo& Info, FString& OutReason)
{
	OutReason.Reset();
	FString JsonText;
	if (!ReadFeatureModText(Info.PluginPath, JsonText))
	{
		OutReason = TEXT("featuremod-descriptor-unreadable");
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutReason = TEXT("featuremod-descriptor-invalid");
		return false;
	}

	bool bFeatureMod = false;
	if (!Root->TryGetBoolField(TEXT("FeatureMod"), bFeatureMod) || !bFeatureMod)
	{
		OutReason = TEXT("featuremod-marker-required");
		return false;
	}

	bool bExplicitlyLoaded = false;
	if (!Root->TryGetBoolField(TEXT("ExplicitlyLoaded"), bExplicitlyLoaded) || !bExplicitlyLoaded)
	{
		OutReason = TEXT("featuremod-explicit-load-required");
		return false;
	}

	FString BuiltInInitialFeatureState;
	if (!Root->TryGetStringField(TEXT("BuiltInInitialFeatureState"), BuiltInInitialFeatureState)
		|| BuiltInInitialFeatureState != TEXT("Installed"))
	{
		OutReason = TEXT("featuremod-initial-state-must-be-installed");
		return false;
	}

	if (FString FriendlyName; Root->TryGetStringField(TEXT("FriendlyName"), FriendlyName))
	{
		Info.DisplayName = FriendlyName;
	}
	const FString DescriptorText = JsonText;

	const FString ConfigPath = Info.DirectoryPath / TEXT("FeatureMod.json");
	if (!ReadFeatureModText(ConfigPath, JsonText))
	{
		OutReason = TEXT("featuremod-config-missing");
		return false;
	}
	TSharedPtr<FJsonObject> FeatureModConfig;
	const TSharedRef<TJsonReader<>> ConfigReader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(ConfigReader, FeatureModConfig) || !FeatureModConfig.IsValid())
	{
		OutReason = TEXT("featuremod-config-invalid");
		return false;
	}
	if (!FeatureModConfig->TryGetStringField(TEXT("version"), Info.VersionName))
	{
		OutReason = TEXT("featuremod-version-required");
		return false;
	}
	Info.VersionName.TrimStartAndEndInline();
	if (Info.VersionName.IsEmpty())
	{
		OutReason = TEXT("featuremod-version-required");
		return false;
	}
	if (!IsBuiltInSource(Info))
	{
		FString SignatureReason;
		if (!FModKitSignature::ValidateTrust(Info, Info.Trust, SignatureReason))
		{
			OutReason = TEXT("external-signature-") + SignatureReason;
			return false;
		}
		else if (Info.Trust == EModKitModTrust::Player && !EnsurePlayerApproval(Info.DirectoryName, &OutReason))
		{
			return false;
		}
		else if (Info.Trust == EModKitModTrust::Official)
		{
			const TMap<FString, TArray<uint8>>* Verified = FModKitSignature::GetVerifiedPackageFiles(Info.DirectoryPath / TEXT("Script"));
			auto MatchesSignedMetadata = [Verified](const FString& Path, const FString& Text)
			{
				FString Key = FPaths::ConvertRelativePathToFull(Path);
				FPaths::NormalizeFilename(Key);
				FPaths::CollapseRelativeDirectories(Key);
				#if PLATFORM_WINDOWS
				Key.ToLowerInline();
#endif
				const TArray<uint8>* Bytes = Verified ? Verified->Find(Key) : nullptr;
				if (!Bytes) return false;
				FString SignedText;
				FFileHelper::BufferToString(SignedText, Bytes->GetData(), Bytes->Num());
				return Text == SignedText;
			};
			if (!MatchesSignedMetadata(Info.PluginPath, DescriptorText) || !MatchesSignedMetadata(ConfigPath, JsonText))
			{
				OutReason = TEXT("external-signature-package-metadata-mismatch");
				return false;
			}
		}
	}
	else
	{
		Info.Trust = EModKitModTrust::BuiltIn;
	}
	Info.ConfigText = JsonText;
	const TSharedPtr<FJsonObject>* Dependencies = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Leaves = nullptr;
	Info.LeafDependencies.Reset();
	if (FeatureModConfig->TryGetObjectField(TEXT("dependencies"), Dependencies)
		&& (*Dependencies)->TryGetArrayField(TEXT("featureMods"), Leaves))
	{
		for (const auto& Leaf : *Leaves)
		{
			FString Id;
			if (!Leaf.IsValid() || !Leaf->TryGetString(Id) || Id.IsEmpty())
			{ OutReason = TEXT("featuremod-dependency-invalid"); return false; }
			Info.LeafDependencies.AddUnique(Id);
		}
	}
	// Each validated package operation is a new revision, including a same-version reinstall.
	Info.Revision = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	return true;
}

void FModKitModRegistry::StartActivation()
{
	check(IsInGameThread());
	if (bActivationStarted)
	{
		return;
	}
	bActivationStarted = true;

	UGameFeaturesSubsystem* GameFeatures = GEngine ? GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
	if (GameFeatures == nullptr)
	{
		// S-2 fail-closed: without the subsystem (GameFeatures/ModularGameplay
		// plugins disabled) no FeatureMod can activate. UGameFeaturesSubsystem::Get() is never used
		// because that static accessor dereferences GEngine unconditionally.
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_GF_UNAVAILABLE reason=subsystem-missing"));
		for (FModEntry& Entry : Mods)
		{
			if (Entry.Info.bHasUPlugin)
			{
				Entry.State = EModActivationState::Failed;
			}
		}
		SettleActivation();
		return;
	}

	// The engine subsystem manager runs Initialize() on every engine
	// subsystem before OnPostEngineInit, which also installs the project
	// policy; a live subsystem therefore implies an initialized policy. The
	// policy-not-initialized edge (GetPolicy ensures) is otherwise unreachable
	// through this API and is documented as "待 g 轮实测".

	const float TimeoutSeconds = UModKitSettings::Get()->ModStartupActivationTimeoutSec;
	ActivationStartTimeSeconds = FPlatformTime::Seconds();

	int32 ActivatingCount = 0;
	for (FModEntry& Entry : Mods)
	{
		if (!Entry.Info.bHasUPlugin || Entry.State != EModActivationState::Discovered)
		{
			continue;
		}

		Entry.State = EModActivationState::Activating;
		Entry.DeadlineSeconds = ActivationStartTimeSeconds + TimeoutSeconds;

		const FGameFeaturePluginLoadComplete CompleteDelegate = FGameFeaturePluginLoadComplete::CreateLambda(
			[DirectoryName = Entry.Info.DirectoryName, PluginURL = Entry.Info.PluginURL](const UE::GameFeatures::FResult& Result)
			{
				OnActivationCallback(DirectoryName, PluginURL, Result);
			});
		GameFeatures->LoadAndActivateGameFeaturePlugin(Entry.Info.PluginURL, CompleteDelegate);
		++ActivatingCount;
	}

	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_GFP_ACTIVATION_BEGIN mods=%d"), Mods.Num());

	if (ActivatingCount == 0 || AllSettled())
	{
		SettleActivation();
		return;
	}

	EnsureActivationTicker();
}

void FModKitModRegistry::OnActivationCallback(const FString& DirectoryName, const FString& PluginURL, const UE::GameFeatures::FResult& Result)
{
	check(IsInGameThread());

	UGameFeaturesSubsystem* GameFeatures = GEngine ? GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;

	for (FModEntry& Entry : Mods)
	{
		if (Entry.Info.DirectoryName != DirectoryName)
		{
			continue;
		}
		if (Entry.State != EModActivationState::Activating)
		{
			return; // a timeout already disabled this mod
		}

		if (Result.HasValue() && GameFeatures != nullptr && GameFeatures->IsGameFeaturePluginActive(PluginURL))
		{
			Entry.State = EModActivationState::Activated;
			UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_MOD_ACTIVATED name=%s"), *DirectoryName);
		}
		else
		{
			DisableMod(Entry, TEXT("activation-failed"));
		}
		break;
	}

	if (AllSettled())
	{
		SettleActivation();
	}
}

bool FModKitModRegistry::OnActivationTick(float DeltaTime)
{
	check(IsInGameThread());
	(void)DeltaTime;
	CheckTimeouts(FPlatformTime::Seconds());
	if (AllSettled())
	{
		// Self-remove: the pump may have been re-armed by a runtime activation
		// after startup settlement, in which case SettleActivation already ran.
		if (TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
			TickerHandle.Reset();
		}
		SettleActivation();
		return false; // stop ticking
	}
	return true;
}

void FModKitModRegistry::CheckTimeouts(double NowSeconds)
{
	for (FModEntry& Entry : Mods)
	{
		if (!Entry.Info.bHasUPlugin || Entry.State != EModActivationState::Activating)
		{
			continue;
		}
		if (NowSeconds >= Entry.DeadlineSeconds)
		{
			DisableMod(Entry, TEXT("activation-timeout"));
		}
	}
}

bool FModKitModRegistry::AllSettled()
{
	for (const FModEntry& Entry : Mods)
	{
		if (Entry.Info.bHasUPlugin
			&& Entry.State != EModActivationState::Activated
			&& Entry.State != EModActivationState::Failed
			&& Entry.State != EModActivationState::Unloaded)
		{
			return false;
		}
	}
	return true;
}

void FModKitModRegistry::SettleActivation()
{
	if (bActivationSettled)
	{
		return;
	}
	bActivationSettled = true;

	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_GFP_ACTIVATION_SETTLED mods=%d"), Mods.Num());

	// The append rendezvous (bInitialCompileFinished && bPostEngineInit &&
	// bActivationSettled) now releases; MaybeRunAppend filters the scan by
	// the roster.
	FModKitCompilationManager::MaybeRunAppend();
}

bool FModKitModRegistry::IsActivationSettled()
{
	return bActivationSettled;
}

bool FModKitModRegistry::ShouldCompileMod(const FString& ModDirectoryName)
{
	for (const FModEntry& Entry : Mods)
	{
		if (Entry.Info.DirectoryName == ModDirectoryName)
		{
			return Entry.State == EModActivationState::Activated;
		}
	}
	return false;
}

void FModKitModRegistry::GetActivatedModNames(TArray<FString>& OutModNames)
{
	OutModNames.Reset();
	for (const FModEntry& Entry : Mods)
	{
		if (Entry.State == EModActivationState::Activated)
		{
			OutModNames.Add(Entry.Info.DirectoryName);
		}
	}
}

bool FModKitModRegistry::ShouldAllowMissingGameFeatureData(const FString& PluginName)
{
	const FModEntry* Entry = FindEntry(PluginName);
	// Only a validated FeatureMod that is in its activation transition may use
	// the transient, empty GameFeatureData object.
	return Entry != nullptr && Entry->Info.bHasUPlugin && Entry->State == EModActivationState::Activating;
}

bool FModKitModRegistry::PrepareReload(const FString& ModDirectoryName, FString& OutReason)
{
	if (FModKitCompilationManager::IsSourceAdmissionPending()) { OutReason = TEXT("busy-source-admission"); return false; }
	check(IsInGameThread());
	FModEntry* Entry = FindEntry(ModDirectoryName);
	if (!Entry || Entry->State != EModActivationState::Activated || !Entry->Info.bHasUPlugin)
	{ OutReason = TEXT("not-loaded"); return false; }
	if (Entry->bReloadRequiresRestart)
	{ OutReason = TEXT("native-reload-requires-restart"); return false; }
	FModKitModInfo Candidate = Entry->Info;
	TArray<FString> Descriptors;
	IFileManager::Get().FindFiles(Descriptors, *(Candidate.DirectoryPath / TEXT("*.uplugin")), true, false);
	if (Descriptors.Num() != 1 || Descriptors[0] != ModDirectoryName + TEXT(".uplugin"))
	{ OutReason = TEXT("featuremod-identity-mismatch"); return false; }
	for (const FFeatureModRoot& Root : GetFeatureModRoots())
		if (Root.RelativePath != Candidate.RelativeRoot
			&& IFileManager::Get().DirectoryExists(*(Root.AbsolutePath / ModDirectoryName)))
		{ OutReason = TEXT("duplicate-featuremod-id"); return false; }
	FModKitSignature::BeginReloadVerification(Candidate.DirectoryPath);
	if (!ParseDescriptor(Candidate, OutReason))
	{
		FModKitSignature::CompleteReloadVerification(Candidate.DirectoryPath, false);
		return false;
	}
	Entry->ReloadCandidate = MoveTemp(Candidate);
	return true;
}

void FModKitModRegistry::CompleteReload(const FString& ModDirectoryName, bool bSuccess, bool bNativeReloadFailed)
{
	FModEntry* Entry = FindEntry(ModDirectoryName);
	check(Entry && Entry->ReloadCandidate.IsSet());
	FModKitSignature::CompleteReloadVerification(Entry->ReloadCandidate->DirectoryPath, bSuccess);
	if (bSuccess) Entry->Info = MoveTemp(Entry->ReloadCandidate.GetValue());
	Entry->ReloadCandidate.Reset();
	Entry->bReloadRequiresRestart |= bNativeReloadFailed;
}

bool FModKitModRegistry::RescanOrAdd(const FString& ModDirectoryName)
{
	if (FModKitCompilationManager::IsSourceAdmissionPending()) return false;
	check(IsInGameThread());

	if (!IsValidModId(ModDirectoryName)) return false;
	FModEntry* Existing = FindEntry(ModDirectoryName);
	if (Existing != nullptr)
	{
		if (Existing->bDisabledByTimeout || Existing->bReloadRequiresRestart) return false;
		if (Existing->State == EModActivationState::Activated || Existing->State == EModActivationState::Activating)
			return true;
		// Re-discover inactive identities so registration, roots and descriptor
		// validation are identical for first loads and subsequent loads.
		Mods.RemoveAll([&](const FModEntry& Entry) { return Entry.Info.DirectoryName == ModDirectoryName; });
	}

	IFileManager& FileManager = IFileManager::Get();
	FModKitModInfo Info;
	Info.DirectoryName = ModDirectoryName;
	Info.DisplayName = ModDirectoryName;
	for (const FFeatureModRoot& Root : GetFeatureModRoots())
	{
		const FString CandidatePath = Root.AbsolutePath / ModDirectoryName;
		if (!FileManager.DirectoryExists(*CandidatePath))
		{
			continue;
		}
		if (!Info.DirectoryPath.IsEmpty())
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR mod=%s reason=duplicate-featuremod-id"), *ModDirectoryName);
			return false;
		}
		Info.DirectoryPath = CandidatePath;
		Info.RelativeRoot = Root.RelativePath;
	}
	if (Info.DirectoryPath.IsEmpty())
	{
		return false; // unknown FeatureMod
	}

	if (ModDirectoryName.Contains(TEXT(".as")))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR mod=%s reason=directory-name-contains-as"), *ModDirectoryName);
		return false;
	}

	TArray<FString> DescriptorFiles;
	FileManager.FindFiles(DescriptorFiles, *(Info.DirectoryPath / TEXT("*.uplugin")), true, false);

	// The plugin filename is its stable ID and must match the folder.
	const FString ExpectedDescriptor = ModDirectoryName + TEXT(".uplugin");
	if (DescriptorFiles.Num() == 1 && DescriptorFiles[0] == ExpectedDescriptor)
	{
		Info.bHasUPlugin = true;
		Info.PluginPath = Info.DirectoryPath / DescriptorFiles[0];
		FString Reason;
		if (!ParseDescriptor(Info, Reason))
		{
			if (Reason == TEXT("player-approval-required"))
			{
				AddEntry(MoveTemp(Info)).State = EModActivationState::AwaitingApproval;
				UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_PLAYER_APPROVAL_REQUIRED name=%s"), *ModDirectoryName);
			}
			else
			{
				UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR mod=%s reason=%s"), *ModDirectoryName, *Reason);
			}
			return false;
		}
		// Runtime FeatureMods may appear after the engine's startup plugin scan.
		IPluginManager::Get().AddPluginSearchPath(FPaths::GetPath(Info.PluginPath), true);
		FText PluginAddFailure;
		if (!IPluginManager::Get().AddToPluginsList(Info.PluginPath, &PluginAddFailure))
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR mod=%s reason=plugin-registration-failed detail=%s"), *ModDirectoryName, *PluginAddFailure.ToString());
			return false;
		}
		Info.PluginURL = UGameFeaturesSubsystem::GetPluginURL_FileProtocol(Info.PluginPath);
		AddEntry(MoveTemp(Info));
		return true;
	}

	const TCHAR* Reason = DescriptorFiles.Num() == 0 ? TEXT("featuremod-descriptor-missing") : TEXT("identity-mismatch");
	UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR mod=%s reason=%s"), *ModDirectoryName, Reason);
	return false;
}

bool FModKitModRegistry::StartRuntimeActivation(const FString& ModDirectoryName)
{
	check(IsInGameThread());
	FModEntry* Entry = FindEntry(ModDirectoryName);
	if (Entry == nullptr || !Entry->Info.bHasUPlugin)
	{
		return false;
	}
	if (Entry->State == EModActivationState::Activated)
	{
		return true;
	}

	UGameFeaturesSubsystem* GameFeatures = GEngine ? GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
	if (GameFeatures == nullptr)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_GF_UNAVAILABLE reason=subsystem-missing"));
		DisableMod(*Entry, TEXT("activation-failed"));
		return false;
	}

	Entry->State = EModActivationState::Activating;
	Entry->DeadlineSeconds = FPlatformTime::Seconds() + UModKitSettings::Get()->ModStartupActivationTimeoutSec;

	const FGameFeaturePluginLoadComplete CompleteDelegate = FGameFeaturePluginLoadComplete::CreateLambda(
		[DirectoryName = Entry->Info.DirectoryName, PluginURL = Entry->Info.PluginURL](const UE::GameFeatures::FResult& Result)
		{
			OnActivationCallback(DirectoryName, PluginURL, Result);
		});
	GameFeatures->LoadAndActivateGameFeaturePlugin(Entry->Info.PluginURL, CompleteDelegate);

	EnsureActivationTicker();
	return true;
}

void FModKitModRegistry::DeactivateMod(const FString& ModDirectoryName)
{
	FModEntry* Entry = FindEntry(ModDirectoryName);
	if (Entry == nullptr || !Entry->Info.bHasUPlugin)
	{
		return;
	}
	UGameFeaturesSubsystem* GameFeatures = GEngine ? GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
	if (GameFeatures == nullptr)
	{
		return;
	}
	// Async state-machine rollback; a pure-script GD mod produces no
	// GameFeatureAction side effects , just the state rollback.
	GameFeatures->DeactivateGameFeaturePlugin(Entry->Info.PluginURL);
}

bool FModKitModRegistry::IsModActivated(const FString& ModDirectoryName)
{
	const FModEntry* Entry = FindEntry(ModDirectoryName);
	return Entry != nullptr && Entry->State == EModActivationState::Activated;
}

void FModKitModRegistry::GetActiveModVersions(TArray<FModKitModVersion>& OutVersions)
{
	check(IsInGameThread());
	OutVersions.Reset();
	for (const FModEntry& Entry : Mods)
	{
		if (Entry.State != EModActivationState::Activated)
		{
			continue;
		}

		FModKitModVersion& Version = OutVersions.AddDefaulted_GetRef();
		Version.Name = Entry.Info.DirectoryName;
		Version.Version = Entry.Info.VersionName;
	}
	OutVersions.Sort([](const FModKitModVersion& Left, const FModKitModVersion& Right)
	{
		return Left.Name < Right.Name;
	});
}

bool FModKitModRegistry::GetModScriptPaths(const FString& ModDirectoryName,
	FString& OutAbsoluteDirectory, FString& OutRelativeRoot)
{
	OutAbsoluteDirectory.Reset();
	OutRelativeRoot.Reset();
	const FModEntry* Entry = FindEntry(ModDirectoryName);
	if (Entry == nullptr)
	{
		return false;
	}

	OutAbsoluteDirectory = Entry->Info.DirectoryPath / TEXT("Script");
	OutRelativeRoot = Entry->Info.RelativeRoot / Entry->Info.DirectoryName / TEXT("Script");
	return true;
}

bool FModKitModRegistry::IsModTerminal(const FString& ModDirectoryName)
{
	const FModEntry* Entry = FindEntry(ModDirectoryName);
	return Entry != nullptr && Entry->State != EModActivationState::Activating;
}

bool FModKitModRegistry::IsModFailed(const FString& ModDirectoryName)
{
	const FModEntry* Entry = FindEntry(ModDirectoryName);
	return Entry != nullptr && Entry->State == EModActivationState::Failed;
}

bool FModKitModRegistry::IsPlayerModApprovalPending(const FString& ModDirectoryName)
{
	const FModEntry* Entry = FindEntry(ModDirectoryName);
	return Entry && Entry->State == EModActivationState::AwaitingApproval;
}

bool FModKitModRegistry::IsPlayerModApproved(const FString& ModDirectoryName)
{
	check(IsInGameThread());
	if (!IsValidModId(ModDirectoryName)) return false;
	FConfigFile File;
	File.Read(PlayerApprovalPath());
	bool bApproved = false;
	return File.GetBool(TEXT("PlayerMods"), *ModDirectoryName, bApproved) && bApproved;
}

bool FModKitModRegistry::ApprovePlayerMod(const FString& ModDirectoryName)
{
	check(IsInGameThread());
	if (!WritePlayerApproval(ModDirectoryName, true)) return false;
	const FModEntry* Entry = FindEntry(ModDirectoryName);
	if (Entry && Entry->Info.Trust == EModKitModTrust::Player && Entry->State == EModActivationState::AwaitingApproval)
		FModKitRuntimeOps::RequestLoad(ModDirectoryName);
	return true;
}

bool FModKitModRegistry::RevokePlayerMod(const FString& ModDirectoryName)
{
	check(IsInGameThread());
	return WritePlayerApproval(ModDirectoryName, false);
}

EModKitModTrust FModKitModRegistry::GetModTrust(const FString& ModDirectoryName)
{
	const FModEntry* Entry = FindEntry(ModDirectoryName);
	return Entry ? (Entry->ReloadCandidate.IsSet() ? Entry->ReloadCandidate->Trust : Entry->Info.Trust)
		: EModKitModTrust::Player;
}

bool FModKitModRegistry::IsTimeoutDisabled(const FString& ModDirectoryName)
{
	const FModEntry* Entry = FindEntry(ModDirectoryName);
	return Entry != nullptr && Entry->bDisabledByTimeout;
}

bool FModKitModRegistry::IsUPluginMod(const FString& ModDirectoryName)
{
	const FModEntry* Entry = FindEntry(ModDirectoryName);
	return Entry != nullptr && Entry->Info.bHasUPlugin;
}

void FModKitModRegistry::MarkModUnloaded(const FString& ModDirectoryName)
{
	FModEntry* Entry = FindEntry(ModDirectoryName);
	if (Entry != nullptr)
	{
		FModKitAssetReplacements::RemoveForMod(ModDirectoryName);
		Entry->State = EModActivationState::Unloaded;
	}
}

void FModKitModRegistry::MarkModFailed(const FString& ModDirectoryName, const TCHAR* Reason)
{
	FModEntry* Entry = FindEntry(ModDirectoryName);
	if (Entry != nullptr)
	{
		DisableMod(*Entry, Reason);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFeatureModDescriptorFileLayerTest,
	"AngelscriptModKit.FeatureMods.DescriptorFileLayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFeatureModDescriptorFileLayerTest::RunTest(const FString&)
{
	const FString Root = FPaths::ProjectSavedDir() / TEXT("Automation/FeatureModDescriptor")
		/ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	IFileManager& Files = IFileManager::Get();
	if (!TestTrue(TEXT("Create through the active file layer"), Files.MakeDirectory(*Root, true))) return false;
	ON_SCOPE_EXIT { Files.DeleteDirectory(*Root, false, true); };
	const FString DescriptorPath = Root / TEXT("Sample.uplugin");
	const FString ConfigPath = Root / TEXT("FeatureMod.json");
	const FString Descriptor = TEXT("{\"FeatureMod\":true,\"FriendlyName\":\"Sample \u00e9\"}");
	const FString Config = TEXT("{\"version\":\"1.0\"}");
	if (!TestTrue(TEXT("Write descriptor"), FFileHelper::SaveStringToFile(Descriptor, *DescriptorPath))
		|| !TestTrue(TEXT("Write config"), FFileHelper::SaveStringToFile(Config, *ConfigPath))) return false;
	TArray<FString> Found;
	Files.FindFiles(Found, *(Root / TEXT("*.uplugin")), true, false);
	TestTrue(TEXT("Discovery sees descriptor"), Found.Num() == 1 && Found[0] == TEXT("Sample.uplugin"));
	FString Text;
	TestTrue(TEXT("Discovered descriptor is readable"), ReadFeatureModText(DescriptorPath, Text));
	TestEqual(TEXT("Descriptor bytes decode unchanged"), Text, Descriptor);
	TestTrue(TEXT("Config uses the same layer"), ReadFeatureModText(ConfigPath, Text));
	TestEqual(TEXT("Config decodes unchanged"), Text, Config);
	TestFalse(TEXT("Missing descriptor is rejected"), ReadFeatureModText(Root / TEXT("Missing.uplugin"), Text));
	TestTrue(TEXT("Write empty file"), FFileHelper::SaveStringToFile(TEXT(""), *ConfigPath));
	TestTrue(TEXT("Empty text is read for the parser to reject"), ReadFeatureModText(ConfigPath, Text));
	TestTrue(TEXT("Empty text has no stale content"), Text.IsEmpty());
	const FString Deep = FString::ChrN(17, TCHAR('[')) + TEXT("0") + FString::ChrN(17, TCHAR(']'));
	TestTrue(TEXT("Write excessive nesting"), FFileHelper::SaveStringToFile(Deep, *ConfigPath));
	TestFalse(TEXT("Depth budget remains enforced"), ReadFeatureModText(ConfigPath, Text));
	const FString Large = FString::ChrN(static_cast<int32>(FModKitPatchManifest::MaxManifestBytes) + 1, TCHAR(' '));
	TestTrue(TEXT("Write excessive bytes"), FFileHelper::SaveStringToFile(Large, *ConfigPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	TestFalse(TEXT("Byte budget remains enforced before allocation"), ReadFeatureModText(ConfigPath, Text));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFeatureModPlayerConsentTest,
	"AngelscriptModKit.FeatureMods.PlayerConsent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFeatureModPlayerConsentTest::RunTest(const FString&)
{
	const FString Name = TEXT("Consent-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString Original;
	const bool bHadFile = FFileHelper::LoadFileToString(Original, *PlayerApprovalPath());
	ON_SCOPE_EXIT
	{
		if (bHadFile) FFileHelper::SaveStringToFile(Original, *PlayerApprovalPath());
		else IFileManager::Get().Delete(*PlayerApprovalPath());
	};
	int32 Requests = 0;
	const FDelegateHandle Handle = FModKitModRegistry::OnPlayerModApprovalRequired.AddLambda(
		[&](const FString& RequestedName) { if (RequestedName == Name) ++Requests; });
	ON_SCOPE_EXIT { FModKitModRegistry::OnPlayerModApprovalRequired.Remove(Handle); };
	TestFalse(TEXT("An unseen player is unapproved"), FModKitModRegistry::IsPlayerModApproved(Name));
	const bool bRequiresApproval = UModKitSettings::Get()->bConfirmPlayerModsOnFirstLoad;
	TestEqual(TEXT("Unapproved load follows confirmation policy"), EnsurePlayerApproval(Name), !bRequiresApproval);
	TestEqual(TEXT("The host receives the requested identity"), Requests, bRequiresApproval ? 1 : 0);
	TestTrue(TEXT("The host can persist approval"), FModKitModRegistry::ApprovePlayerMod(Name));
	TestTrue(TEXT("Approval can be read back from Saved"), FModKitModRegistry::IsPlayerModApproved(Name));
	TestTrue(TEXT("An approved player proceeds without another prompt"), EnsurePlayerApproval(Name));
	TestEqual(TEXT("Approval does not repeat its prompt"), Requests, bRequiresApproval ? 1 : 0);
	TestTrue(TEXT("The host can revoke approval"), FModKitModRegistry::RevokePlayerMod(Name));
	TestFalse(TEXT("Revoked approval is no longer accepted"), FModKitModRegistry::IsPlayerModApproved(Name));
	const int32 RequestsBeforeRejection = Requests;
	TestEqual(TEXT("A persisted rejection blocks subsequent loads"), EnsurePlayerApproval(Name), !bRequiresApproval);
	TestEqual(TEXT("A persisted rejection does not ask again"), Requests, RequestsBeforeRejection);
	const FString HostName = Name + TEXT("-Host");
	int32 HostRequests = 0;
	SetFeatureModPlayerApprovalHandler(FFeatureModPlayerApprovalHandler::CreateLambda(
		[&](const FString& RequestedName) { ++HostRequests; return RequestedName == HostName; }));
	ON_SCOPE_EXIT { SetFeatureModPlayerApprovalHandler(FFeatureModPlayerApprovalHandler()); };
	TestTrue(TEXT("The public host handler can approve a first load"), EnsurePlayerApproval(HostName));
	TestTrue(TEXT("Approval is read from disk on subsequent loads"), EnsurePlayerApproval(HostName));
	TestEqual(TEXT("The host is called only for the first load"), HostRequests, bRequiresApproval ? 1 : 0);
	TestEqual(TEXT("The public host handler can refuse a first load"), EnsurePlayerApproval(Name + TEXT("-Rejected")), !bRequiresApproval);
	const int32 CallsAfterRefusal = HostRequests;
	TestEqual(TEXT("Host refusal survives a fresh disk read"), EnsurePlayerApproval(Name + TEXT("-Rejected")), !bRequiresApproval);
	TestEqual(TEXT("Host refusal does not repeat its prompt"), HostRequests, CallsAfterRefusal);
	SetFeatureModPlayerApprovalHandler(FFeatureModPlayerApprovalHandler());
	FModKitModRegistry::OnPlayerModApprovalRequired.Remove(Handle);
	if (bRequiresApproval && (IsRunningCommandlet() || FApp::IsUnattended() || !FApp::CanEverRender()))
	{
		FString Reason;
		TestFalse(TEXT("Headless first load cannot silently allow code"), EnsurePlayerApproval(Name + TEXT("-Headless"), &Reason));
		TestEqual(TEXT("Headless first load refuses waiting for UI"), Reason, FString(TEXT("player-approval-unavailable")));
	}
	const FString Provision = FString::Printf(TEXT("[PlayerMods]\n%s=True\n"), *Name);
	TestTrue(TEXT("CI can provision the documented INI before launch"),
		FFileHelper::SaveStringToFile(Provision, *PlayerApprovalPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	TestTrue(TEXT("Raw provision is consumed through the same approval API"), FModKitModRegistry::IsPlayerModApproved(Name));
	const int32 RequestsBeforeProvision = Requests;
	TestTrue(TEXT("Preapproved player needs no UI"), EnsurePlayerApproval(Name));
	TestEqual(TEXT("Headless provision does not broadcast another prompt"), Requests, RequestsBeforeProvision);
	TestTrue(TEXT("Provisioned approval can be revoked"), FModKitModRegistry::RevokePlayerMod(Name));
	TestFalse(TEXT("Revoking provisioned state takes effect"), FModKitModRegistry::IsPlayerModApproved(Name));
	TestFalse(TEXT("Approval cannot escape its mod identity"), FModKitModRegistry::ApprovePlayerMod(TEXT("../Outside")));
	return true;
}

#endif
