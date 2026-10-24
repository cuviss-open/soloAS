// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

enum class EAngelscriptExtensionCacheArtifactFamily : uint8
{
	Binds,
	Precompiled,
};

/** Runtime-owned opaque handle for a bridge-validated cache snapshot. */
class FAngelscriptValidatedCacheSnapshot final
{
public:
	FAngelscriptValidatedCacheSnapshot();
	~FAngelscriptValidatedCacheSnapshot();

	FAngelscriptValidatedCacheSnapshot(
		FAngelscriptValidatedCacheSnapshot&& Other) noexcept;
	FAngelscriptValidatedCacheSnapshot& operator=(
		FAngelscriptValidatedCacheSnapshot&& Other) noexcept;

	FAngelscriptValidatedCacheSnapshot(
		const FAngelscriptValidatedCacheSnapshot&) = delete;
	FAngelscriptValidatedCacheSnapshot& operator=(
		const FAngelscriptValidatedCacheSnapshot&) = delete;

	bool IsValid() const;
	const FString& GetPath() const;

private:
	struct FState;

	friend bool CreateValidatedAngelscriptExtensionCacheSnapshot(
		EAngelscriptExtensionCacheArtifactFamily,
		const FString&,
		FAngelscriptValidatedCacheSnapshot&,
		FString&);

	void Reset();

	TUniquePtr<FState> State;
};

bool PrepareAngelscriptExtensionCachePublish(
	EAngelscriptExtensionCacheArtifactFamily Family,
	const FString& ArtifactPath,
	FString& OutError
);

bool CreateValidatedAngelscriptExtensionCacheSnapshot(
	EAngelscriptExtensionCacheArtifactFamily Family,
	const FString& ArtifactPath,
	FAngelscriptValidatedCacheSnapshot& OutSnapshot,
	FString& OutError
);

bool WriteAngelscriptExtensionCacheCompanion(
	EAngelscriptExtensionCacheArtifactFamily Family,
	const FString& ArtifactPath,
	FString& OutError
);
