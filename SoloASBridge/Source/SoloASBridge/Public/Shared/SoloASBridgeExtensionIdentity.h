#pragma once

#include "CoreMinimal.h"

namespace SoloASBridge::Extensions
{
/** Stable identity supplied by one optional extension provider. */
struct SOLOASBRIDGE_API FProviderIdentity
{
    /** 1-64 ASCII bytes from [A-Za-z0-9._/-]. */
    FName ProviderId = NAME_None;

    /** 1-64 ASCII bytes from [A-Za-z0-9._/-]. */
    FString CompatibilityTag;
};

/**
 * Opaque move-only ownership token for the active provider identity.
 *
 * Destruction never unregisters implicitly. The owner must explicitly
 * unregister before destroying or replacing a valid token.
 */
class SOLOASBRIDGE_API FProviderIdentityRegistration final
{
public:
    FProviderIdentityRegistration() = default;
    ~FProviderIdentityRegistration();

    FProviderIdentityRegistration(
        FProviderIdentityRegistration&& Other) noexcept;
    FProviderIdentityRegistration& operator=(
        FProviderIdentityRegistration&& Other) noexcept;

    FProviderIdentityRegistration(
        const FProviderIdentityRegistration&) = delete;
    FProviderIdentityRegistration& operator=(
        const FProviderIdentityRegistration&) = delete;

    bool IsValid() const;

private:
    friend SOLOASBRIDGE_API bool RegisterProviderIdentity(
        const FProviderIdentity&,
        FProviderIdentityRegistration&,
        FString&);
    friend SOLOASBRIDGE_API bool UnregisterProviderIdentity(
        FProviderIdentityRegistration&,
        FString&);

    uint64 Serial = 0;
    FName CanonicalProviderId = NAME_None;
};

/**
 * Registers the only active provider identity.
 *
 * The provider ID is canonicalized to lowercase. If the identity fingerprint
 * has already been read, only the identity captured by that first read may be
 * registered again.
 */
SOLOASBRIDGE_API bool RegisterProviderIdentity(
    const FProviderIdentity& Identity,
    FProviderIdentityRegistration& OutRegistration,
    FString& OutError);

/** Explicitly unregisters the active provider identity. */
SOLOASBRIDGE_API bool UnregisterProviderIdentity(
    FProviderIdentityRegistration& Registration,
    FString& OutError);

/**
 * Returns the stable SHA-1 fingerprint sealed by the first read.
 *
 * The first call irreversibly captures either the active canonical identity
 * or the absence of a provider. All identity operations are synchronized.
 */
SOLOASBRIDGE_API bool GetSealedProviderIdentityFingerprint(
    FString& OutFingerprint,
    FString& OutError);
}
