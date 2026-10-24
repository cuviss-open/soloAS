// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

enum class EAngelscriptScriptLayer : uint8
{
	Trunk,
	Branch,
	Leaf,
};

struct FAngelscriptScriptUnit
{
	FString Id = TEXT("Project");
	EAngelscriptScriptLayer Layer = EAngelscriptScriptLayer::Trunk;
	TArray<FString> Dependencies;
	uint32 AccessMask = ~uint32(0);
	FString ConfigurationError;
};

// One unit owns all file modules under a script root. Native-only dependencies
// remain in the catalog so they do not look like missing script units.
class ANGELSCRIPTCODE_API FAngelscriptScriptUnitManager
{
public:
	void Discover();
	// Incremental compilation only needs inactive configurations for its input roots.
	void Discover(const TArray<FString>& SourceFiles);
	FAngelscriptScriptUnit ResolveOwner(const FString& AbsoluteFilename) const;
	FString ResolveModuleName(const FString& AbsoluteFilename, const FString& RelativeFilename) const;
	void AddUnit(const FAngelscriptScriptUnit& Unit);
	void BuildOrder(TArray<FString>& OutOrder, TMap<FString, FString>& OutErrors) const;
	bool CanReference(const FString& From, const FString& To, FString& OutError) const;
	TSet<FString> GetDependents(const TSet<FString>& ChangedUnits) const;
	const FAngelscriptScriptUnit* Find(const FString& Id) const { return Data->Units.Find(Id); }

private:
	// Discover shares a completed graph; AddUnit detaches before changing it.
	struct FData
	{
		TMap<FString, FAngelscriptScriptUnit> Units;
		TMap<FString, FAngelscriptScriptUnit> ScriptRoots;
		bool bHasBuildOrder = false;
		TArray<FString> CachedBuildOrder;
		TMap<FString, FString> CachedBuildErrors;
	};
	TSharedRef<FData, ESPMode::ThreadSafe> Data = MakeShared<FData, ESPMode::ThreadSafe>();
};
