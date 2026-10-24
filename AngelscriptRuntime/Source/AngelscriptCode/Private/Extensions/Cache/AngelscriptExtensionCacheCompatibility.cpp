// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "Extensions/Cache/AngelscriptExtensionCacheCompatibility.h"

#include "Extensions/AngelscriptExtensionPipelineInternal.h"
#include "Shared/SoloASBridgeExtensionCache.h"

#include "Misc/Paths.h"

namespace
{
using namespace SoloASBridge::Extensions;

FExtensionCacheContract MakeBindsCacheContract()
{
	FExtensionCacheContract Contract;
	Contract.FormatId = TEXT("angelscript-extension-cache-companion/v2");
	Contract.CompanionSuffix = TEXT(".cuviss-cache-v2");
	Contract.FamilyId = TEXT("binds");
	FExtensionCacheMemberContract& Binds = Contract.Members.AddDefaulted_GetRef();
	Binds.Role = TEXT("binds");
	Binds.Suffix = TEXT("");
	FExtensionCacheMemberContract& Headers = Contract.Members.AddDefaulted_GetRef();
	Headers.Role = TEXT("headers");
	Headers.Suffix = TEXT(".Headers");
	return Contract;
}

FExtensionCacheContract MakePrecompiledCacheContract()
{
	FExtensionCacheContract Contract;
	Contract.FormatId = TEXT("angelscript-extension-cache-companion/v2");
	Contract.CompanionSuffix = TEXT(".cuviss-cache-v2");
	Contract.FamilyId = TEXT("precompiled");
	FExtensionCacheMemberContract& Precompiled = Contract.Members.AddDefaulted_GetRef();
	Precompiled.Role = TEXT("precompiled");
	Precompiled.Suffix = TEXT("");
	return Contract;
}

const FExtensionCacheContract* GetCacheContract(
	EAngelscriptExtensionCacheArtifactFamily Family,
	FString& OutError)
{
	static const FExtensionCacheContract BindsContract = MakeBindsCacheContract();
	static const FExtensionCacheContract PrecompiledContract =
		MakePrecompiledCacheContract();

	switch (Family)
	{
	case EAngelscriptExtensionCacheArtifactFamily::Binds:
		return &BindsContract;
	case EAngelscriptExtensionCacheArtifactFamily::Precompiled:
		return &PrecompiledContract;
	default:
		OutError = TEXT("Cache artifact family is invalid.");
		return nullptr;
	}
}

FString GetCacheSnapshotDirectory()
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AngelScript"),
		TEXT("ValidatedCacheSnapshots"));
}

}

struct FAngelscriptValidatedCacheSnapshot::FState
{
	SoloASBridge::Extensions::FValidatedCacheSnapshot Snapshot;
};

FAngelscriptValidatedCacheSnapshot::FAngelscriptValidatedCacheSnapshot() = default;

FAngelscriptValidatedCacheSnapshot::~FAngelscriptValidatedCacheSnapshot()
{
	Reset();
}

FAngelscriptValidatedCacheSnapshot::FAngelscriptValidatedCacheSnapshot(
	FAngelscriptValidatedCacheSnapshot&& Other) noexcept
	: State(MoveTemp(Other.State))
{
}

FAngelscriptValidatedCacheSnapshot&
FAngelscriptValidatedCacheSnapshot::operator=(
	FAngelscriptValidatedCacheSnapshot&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		State = MoveTemp(Other.State);
	}
	return *this;
}

bool FAngelscriptValidatedCacheSnapshot::IsValid() const
{
	return State.Get() != nullptr && State->Snapshot.IsValid();
}

const FString& FAngelscriptValidatedCacheSnapshot::GetPath() const
{
	check(IsValid());
	return State->Snapshot.GetPath();
}

void FAngelscriptValidatedCacheSnapshot::Reset()
{
	State.Reset();
}

bool PrepareAngelscriptExtensionCachePublish(
	EAngelscriptExtensionCacheArtifactFamily Family,
	const FString& ArtifactPath,
	FString& OutError)
{
	OutError.Reset();
	if (!IsAngelscriptExtensionProviderRegistered())
		return true;
	FString Fingerprint;
	if (!GetAngelscriptExtensionCacheFingerprint(Fingerprint, OutError))
		return false;
	(void)Fingerprint;
	const FExtensionCacheContract* Contract =
		GetCacheContract(Family, OutError);
	if (Contract == nullptr)
		return false;
	return PrepareExtensionCachePublish(*Contract, ArtifactPath, OutError);
}

bool CreateValidatedAngelscriptExtensionCacheSnapshot(
	EAngelscriptExtensionCacheArtifactFamily Family,
	const FString& ArtifactPath,
	FAngelscriptValidatedCacheSnapshot& OutSnapshot,
	FString& OutError)
{
	OutError.Reset();
	OutSnapshot.Reset();
	if (!IsAngelscriptExtensionProviderRegistered())
		return true;
	FString Fingerprint;
	if (!GetAngelscriptExtensionCacheFingerprint(Fingerprint, OutError))
		return false;
	const FExtensionCacheContract* Contract =
		GetCacheContract(Family, OutError);
	if (Contract == nullptr)
		return false;

	TUniquePtr<FAngelscriptValidatedCacheSnapshot::FState> State =
		MakeUnique<FAngelscriptValidatedCacheSnapshot::FState>();
	if (!CreateValidatedCacheSnapshot(
		*Contract,
		ArtifactPath,
		Fingerprint,
		GetCacheSnapshotDirectory(),
		State->Snapshot,
		OutError))
	{
		return false;
	}
	OutSnapshot.State = MoveTemp(State);
	return true;
}

bool WriteAngelscriptExtensionCacheCompanion(
	EAngelscriptExtensionCacheArtifactFamily Family,
	const FString& ArtifactPath,
	FString& OutError)
{
	OutError.Reset();
	if (!IsAngelscriptExtensionProviderRegistered())
		return true;
	FString Fingerprint;
	if (!GetAngelscriptExtensionCacheFingerprint(Fingerprint, OutError))
		return false;
	const FExtensionCacheContract* Contract =
		GetCacheContract(Family, OutError);
	if (Contract == nullptr)
		return false;
	return WriteExtensionCacheCompanion(
		*Contract,
		ArtifactPath,
		Fingerprint,
		OutError);
}
