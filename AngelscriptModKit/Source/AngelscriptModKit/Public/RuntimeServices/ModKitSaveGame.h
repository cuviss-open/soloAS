// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "ModKitSaveGame.generated.h"

USTRUCT(BlueprintType)
struct ANGELSCRIPTMODKIT_API FModKitSavedMod
{
	GENERATED_BODY()

	/** Stable plugin ID: the FeatureMod directory and .uplugin filename. */
	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "ModKit")
	FString Name;

	/** Version copied from the FeatureMod.json version field. */
	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "ModKit")
	FString Version;
};

/**
 * SaveGame base class that records the active mod identity and version.
 * Call CaptureModManifest before saving, then CanApplyWithCurrentMods after
 * loading and before applying gameplay state. An incompatible save remains
 * available to the caller but must not be applied. Plugin mods use their
 * ID comes from the plugin filename, and the version comes from the mod's
 * FeatureMod.json file.
 */
UCLASS(BlueprintType)
class ANGELSCRIPTMODKIT_API UModKitSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "ModKit")
	int32 ModManifestSchema = 0;

	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "ModKit")
	TArray<FModKitSavedMod> ModManifest;

	UFUNCTION(BlueprintCallable, Category = "ModKit|Save")
	bool CaptureModManifest(FString& OutReason);

	UFUNCTION(BlueprintCallable, Category = "ModKit|Save")
	bool CanApplyWithCurrentMods(TArray<FString>& OutReasons) const;
};
