// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "FeatureMods/ModKitAssetReplacements.h"

#include "Core/AngelscriptModKit.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "FeatureMods/ModKitModRegistry.h"
#include "FeatureMods/ModKitRuntimeOps.h"

namespace
{
	struct FReplacementEntry
	{
		FString ModName;
		FSoftObjectPath SourceAsset;
		FSoftObjectPath ReplacementAsset;
	};

	TArray<FReplacementEntry> Replacements;
	FDelegateHandle GModKitAssetReplacementsBeforeUnloadHandle;

	void RemoveModReplacements(const FString& ModName)
	{
		FModKitAssetReplacements::RemoveForMod(ModName);
	}

	void RemoveBeforeUnloadHandlerIfUnused()
	{
		if (Replacements.IsEmpty() && GModKitAssetReplacementsBeforeUnloadHandle.IsValid())
		{
			FModKitRuntimeOps::OnBeforeUnload.Remove(GModKitAssetReplacementsBeforeUnloadHandle);
			GModKitAssetReplacementsBeforeUnloadHandle.Reset();
		}
	}
}

bool FModKitAssetReplacements::Register(const FString& ModName, const FSoftObjectPath& SourceAsset,
	const FSoftObjectPath& ReplacementAsset, FString& OutReason)
{
	check(IsInGameThread());
	OutReason.Reset();

	if (!FModKitModRegistry::IsModActivated(ModName))
	{
		OutReason = TEXT("mod-not-active");
		return false;
	}
	if (!SourceAsset.IsValid() || !ReplacementAsset.IsValid()
		|| SourceAsset.ToString().Contains(TEXT(":")) || ReplacementAsset.ToString().Contains(TEXT(":")))
	{
		OutReason = TEXT("invalid-asset-path");
		return false;
	}

	const FString SourcePackage = SourceAsset.GetLongPackageName();
	const FString ReplacementPackage = ReplacementAsset.GetLongPackageName();
	if (!SourcePackage.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
	{
		OutReason = TEXT("source-outside-game-content");
		return false;
	}

	const FString ModContentRoot = FString::Printf(TEXT("/%s/"), *ModName);
	if (!ReplacementPackage.StartsWith(ModContentRoot, ESearchCase::CaseSensitive))
	{
		OutReason = TEXT("replacement-outside-own-mod");
		return false;
	}

	for (FReplacementEntry& Entry : Replacements)
	{
		if (Entry.SourceAsset != SourceAsset)
		{
			continue;
		}
		if (Entry.ModName != ModName)
		{
			OutReason = TEXT("source-already-replaced");
			return false;
		}
		Entry.ReplacementAsset = ReplacementAsset;
		return true;
	}

	FReplacementEntry& Entry = Replacements.AddDefaulted_GetRef();
	Entry.ModName = ModName;
	Entry.SourceAsset = SourceAsset;
	Entry.ReplacementAsset = ReplacementAsset;
	if (!GModKitAssetReplacementsBeforeUnloadHandle.IsValid())
	{
		GModKitAssetReplacementsBeforeUnloadHandle = FModKitRuntimeOps::OnBeforeUnload.AddStatic(&RemoveModReplacements);
	}
	return true;
}

FSoftObjectPath FModKitAssetReplacements::Resolve(const FSoftObjectPath& SourceAsset)
{
	check(IsInGameThread());
	for (const FReplacementEntry& Entry : Replacements)
	{
		if (Entry.SourceAsset == SourceAsset && FModKitModRegistry::IsModActivated(Entry.ModName))
		{
			return Entry.ReplacementAsset;
		}
	}
	return SourceAsset;
}

UObject* FModKitAssetReplacements::LoadSynchronous(const FSoftObjectPath& SourceAsset)
{
	const FSoftObjectPath ResolvedAsset = Resolve(SourceAsset);
	if (UObject* Replacement = ResolvedAsset.TryLoad())
	{
		return Replacement;
	}
	if (ResolvedAsset != SourceAsset)
	{
		UE_LOG(LogAngelscriptModKit, Warning, TEXT("MODKIT_ASSET_REPLACEMENT_FALLBACK source=%s replacement=%s reason=missing"),
			*SourceAsset.ToString(), *ResolvedAsset.ToString());
	}
	return SourceAsset.TryLoad();
}

void FModKitAssetReplacements::RemoveForMod(const FString& ModName)
{
	check(IsInGameThread());
	Replacements.RemoveAllSwap([&ModName](const FReplacementEntry& Entry)
	{
		return Entry.ModName == ModName;
	});
	RemoveBeforeUnloadHandlerIfUnused();
}

void FModKitAssetReplacements::Shutdown()
{
	check(IsInGameThread());
	if (GModKitAssetReplacementsBeforeUnloadHandle.IsValid())
	{
		FModKitRuntimeOps::OnBeforeUnload.Remove(GModKitAssetReplacementsBeforeUnloadHandle);
		GModKitAssetReplacementsBeforeUnloadHandle.Reset();
	}
	Replacements.Reset();
}
