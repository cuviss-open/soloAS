#pragma once

#include "CoreMinimal.h"

namespace SoloASBridge::Extensions
{
/** One required member of a cache artifact family. */
struct SOLOASBRIDGE_API FExtensionCacheMemberContract
{
    /** Stable ASCII role written to the companion manifest. */
    FString Role;

    /** Suffix appended to the artifact path; the first member must use none. */
    FString Suffix;
};

/** Product-neutral description of a cache artifact and its companion. */
struct SOLOASBRIDGE_API FExtensionCacheContract
{
    /** Stable ASCII format identifier written to the companion manifest. */
    FString FormatId;

    /** Non-empty suffix appended to the artifact path for the companion. */
    FString CompanionSuffix;

    /** Stable ASCII family identifier written to the companion manifest. */
    FString FamilyId;

    /** Ordered members whose bytes and SHA-1 digests are covered. */
    TArray<FExtensionCacheMemberContract> Members;
};

/** Move-only validated copy of a cache artifact family. */
class SOLOASBRIDGE_API FValidatedCacheSnapshot final
{
public:
    FValidatedCacheSnapshot() = default;
    ~FValidatedCacheSnapshot();

    FValidatedCacheSnapshot(FValidatedCacheSnapshot&& Other) noexcept;
    FValidatedCacheSnapshot& operator=(
        FValidatedCacheSnapshot&& Other) noexcept;

    FValidatedCacheSnapshot(const FValidatedCacheSnapshot&) = delete;
    FValidatedCacheSnapshot& operator=(
        const FValidatedCacheSnapshot&) = delete;

    bool IsValid() const;
    const FString& GetPath() const;

private:
    friend SOLOASBRIDGE_API bool CreateValidatedCacheSnapshot(
        const FExtensionCacheContract&,
        const FString&,
        const FString&,
        const FString&,
        FValidatedCacheSnapshot&,
        FString&);

    void Reset();

    FString SnapshotPath;
    TArray<FString> SnapshotMemberPaths;
};

/** Removes every family member and companion before a new publish. */
SOLOASBRIDGE_API bool PrepareExtensionCachePublish(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath,
    FString& OutError);

/**
 * Validates the published family and companion, then copies the covered bytes.
 *
 * The caller supplies the expected fingerprint and the snapshot directory.
 * The returned snapshot owns and removes every copied member.
 */
SOLOASBRIDGE_API bool CreateValidatedCacheSnapshot(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath,
    const FString& ExpectedFingerprint,
    const FString& SnapshotDirectory,
    FValidatedCacheSnapshot& OutSnapshot,
    FString& OutError);

/** Writes and verifies a companion for the current artifact family bytes. */
SOLOASBRIDGE_API bool WriteExtensionCacheCompanion(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath,
    const FString& ExpectedFingerprint,
    FString& OutError);
}
