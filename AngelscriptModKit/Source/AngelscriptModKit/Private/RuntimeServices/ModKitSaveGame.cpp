// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "RuntimeServices/ModKitSaveGame.h"

#include "FeatureMods/ModKitModRegistry.h"

namespace
{
	constexpr int32 ModManifestSchemaVersion = 1;
}

bool UModKitSaveGame::CaptureModManifest(FString& OutReason)
{
	check(IsInGameThread());
	OutReason.Reset();
	if (!FModKitModRegistry::IsActivationSettled())
	{
		OutReason = TEXT("mod-activation-incomplete");
		return false;
	}

	TArray<FModKitModVersion> ActiveVersions;
	FModKitModRegistry::GetActiveModVersions(ActiveVersions);
	TArray<FModKitSavedMod> NewManifest;
	NewManifest.Reserve(ActiveVersions.Num());
	for (const FModKitModVersion& Active : ActiveVersions)
	{
		if (Active.Version.IsEmpty())
		{
			OutReason = FString::Printf(TEXT("unversioned-mod:%s"), *Active.Name);
			return false;
		}

		FModKitSavedMod& Saved = NewManifest.AddDefaulted_GetRef();
		Saved.Name = Active.Name;
		Saved.Version = Active.Version;
	}

	ModManifest = MoveTemp(NewManifest);
	ModManifestSchema = ModManifestSchemaVersion;
	return true;
}

bool UModKitSaveGame::CanApplyWithCurrentMods(TArray<FString>& OutReasons) const
{
	check(IsInGameThread());
	OutReasons.Reset();
	if (!FModKitModRegistry::IsActivationSettled())
	{
		OutReasons.Add(TEXT("mod-activation-incomplete"));
		return false;
	}
	if (ModManifestSchema != ModManifestSchemaVersion)
	{
		OutReasons.Add(TEXT("unsupported-mod-manifest"));
		return false;
	}

	TArray<FModKitModVersion> ActiveVersions;
	FModKitModRegistry::GetActiveModVersions(ActiveVersions);
	TSet<FString> SavedNames;
	for (const FModKitSavedMod& Saved : ModManifest)
	{
		if (Saved.Name.IsEmpty() || Saved.Version.IsEmpty())
		{
			OutReasons.Add(TEXT("invalid-or-unversioned-saved-mod"));
			continue;
		}
		if (SavedNames.Contains(Saved.Name))
		{
			OutReasons.Add(FString::Printf(TEXT("duplicate-saved-mod:%s"), *Saved.Name));
			continue;
		}
		SavedNames.Add(Saved.Name);

		const FModKitModVersion* Current = ActiveVersions.FindByPredicate([&Saved](const FModKitModVersion& Candidate)
		{
			return Candidate.Name == Saved.Name;
		});
		if (Current == nullptr)
		{
			OutReasons.Add(FString::Printf(TEXT("missing-mod:%s"), *Saved.Name));
		}
		else if (Current->Version.IsEmpty())
		{
			OutReasons.Add(FString::Printf(TEXT("unversioned-current-mod:%s"), *Saved.Name));
		}
		else if (Current->Version != Saved.Version)
		{
			OutReasons.Add(FString::Printf(TEXT("version-mismatch:%s:saved=%s:current=%s"),
				*Saved.Name, *Saved.Version, *Current->Version));
		}
	}

	for (const FModKitModVersion& Current : ActiveVersions)
	{
		if (!SavedNames.Contains(Current.Name))
		{
			OutReasons.Add(FString::Printf(TEXT("unexpected-active-mod:%s"), *Current.Name));
		}
		if (Current.Version.IsEmpty())
		{
			OutReasons.Add(FString::Printf(TEXT("unversioned-current-mod:%s"), *Current.Name));
		}
	}

	return OutReasons.IsEmpty();
}
