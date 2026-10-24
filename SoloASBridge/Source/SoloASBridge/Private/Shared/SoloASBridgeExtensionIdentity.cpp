#include "Shared/SoloASBridgeExtensionIdentity.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"

namespace SoloASBridge::Extensions
{
namespace
{
constexpr int32 CanonicalIdentityTokenMaxBytes = 64;

struct FProviderIdentityState
{
    FCriticalSection Mutex;
    uint64 ActiveSerial = 0;
    uint64 NextSerial = 1;
    FName ActiveProviderId = NAME_None;
    FString ActiveCompatibilityTag;
    FName SealedProviderId = NAME_None;
    FString SealedCompatibilityTag;
    FString SealedFingerprint;
    bool bIdentitySealed = false;
    bool bSealedProviderPresent = false;
};

FProviderIdentityState& GetProviderIdentityState()
{
    static FProviderIdentityState State;
    return State;
}

bool IsCanonicalIdentityToken(const FString& Token)
{
    if (Token.IsEmpty() || Token.Len() > CanonicalIdentityTokenMaxBytes)
        return false;

    for (const TCHAR Character : Token)
    {
        const bool bAllowed = Character < 128
            && (FChar::IsAlnum(Character)
                || Character == TEXT('.')
                || Character == TEXT('_')
                || Character == TEXT('/')
                || Character == TEXT('-'));
        if (!bAllowed)
            return false;
    }
    return true;
}

FString MakeIdentityFingerprint(
    bool bProviderPresent,
    const FName ProviderId,
    const FString& CompatibilityTag)
{
    static const FString ContractVersion =
        TEXT("soloas-bridge-extension-provider-identity/v1");
    const FString ProviderText = bProviderPresent
        ? ProviderId.ToString().ToLower()
        : FString();
    FTCHARToUTF8 ProviderUtf8(*ProviderText);
    FTCHARToUTF8 TagUtf8(*CompatibilityTag);
    const FString CanonicalIdentity = FString::Printf(
        TEXT("%s:%d:%d:%s:%d:%s"),
        *ContractVersion,
        bProviderPresent ? 1 : 0,
        ProviderUtf8.Length(),
        *ProviderText,
        TagUtf8.Length(),
        *CompatibilityTag);
    FTCHARToUTF8 IdentityUtf8(*CanonicalIdentity);
    uint8 Digest[FSHA1::DigestSize];
    FSHA1::HashBuffer(IdentityUtf8.Get(), IdentityUtf8.Length(), Digest);
    return TEXT("sha1:")
        + BytesToHex(Digest, FSHA1::DigestSize).ToLower();
}
}

FProviderIdentityRegistration::FProviderIdentityRegistration(
    FProviderIdentityRegistration&& Other) noexcept
    : Serial(Other.Serial)
    , CanonicalProviderId(Other.CanonicalProviderId)
{
    Other.Serial = 0;
    Other.CanonicalProviderId = NAME_None;
}

FProviderIdentityRegistration::~FProviderIdentityRegistration()
{
#if !UE_BUILD_SHIPPING
    ensureMsgf(
        !IsValid(),
        TEXT("An active extension provider identity registration was "
             "destroyed without explicit unregister."));
#endif
}

FProviderIdentityRegistration& FProviderIdentityRegistration::operator=(
    FProviderIdentityRegistration&& Other) noexcept
{
    if (this == &Other)
        return *this;

    if (IsValid())
    {
#if !UE_BUILD_SHIPPING
        ensureMsgf(
            false,
            TEXT("An active extension provider identity registration must be "
                 "explicitly unregistered before replacement."));
#endif
        return *this;
    }

    Serial = Other.Serial;
    CanonicalProviderId = Other.CanonicalProviderId;
    Other.Serial = 0;
    Other.CanonicalProviderId = NAME_None;
    return *this;
}

bool FProviderIdentityRegistration::IsValid() const
{
    return Serial != 0 && !CanonicalProviderId.IsNone();
}

bool RegisterProviderIdentity(
    const FProviderIdentity& Identity,
    FProviderIdentityRegistration& OutRegistration,
    FString& OutError)
{
    OutError.Reset();
    if (Identity.ProviderId.IsNone())
    {
        OutError = TEXT("Extension provider ID must be stable and non-empty.");
        return false;
    }

    const FString ProviderText = Identity.ProviderId.ToString();
    if (!IsCanonicalIdentityToken(ProviderText))
    {
        OutError = TEXT(
            "Extension provider ID must be a 1-64 byte canonical ASCII token.");
        return false;
    }
    if (!IsCanonicalIdentityToken(Identity.CompatibilityTag))
    {
        OutError = TEXT(
            "Extension compatibility tag must be a 1-64 byte canonical ASCII "
            "token.");
        return false;
    }
    if (OutRegistration.IsValid())
    {
        OutError = TEXT("Extension provider identity output is already active.");
        return false;
    }

    const FString CanonicalProviderText = ProviderText.ToLower();
    const FName CanonicalProviderId(*CanonicalProviderText);
    FProviderIdentityState& State = GetProviderIdentityState();
    FScopeLock Lock(&State.Mutex);
    if (State.ActiveSerial != 0)
    {
        OutError = State.ActiveProviderId == CanonicalProviderId
            ? TEXT("Extension provider identity is already registered.")
            : FString::Printf(
                TEXT("Extension provider identity conflicts with active "
                     "provider '%s'."),
                *State.ActiveProviderId.ToString());
        return false;
    }
    if (State.bIdentitySealed
        && (!State.bSealedProviderPresent
            || State.SealedProviderId != CanonicalProviderId
            || State.SealedCompatibilityTag != Identity.CompatibilityTag))
    {
        OutError = TEXT(
            "Extension provider identity differs from the sealed identity.");
        return false;
    }

    if (State.NextSerial == 0)
        ++State.NextSerial;
    State.ActiveSerial = State.NextSerial++;
    State.ActiveProviderId = CanonicalProviderId;
    State.ActiveCompatibilityTag = Identity.CompatibilityTag;
    OutRegistration.Serial = State.ActiveSerial;
    OutRegistration.CanonicalProviderId = CanonicalProviderId;
    return true;
}

bool UnregisterProviderIdentity(
    FProviderIdentityRegistration& Registration,
    FString& OutError)
{
    OutError.Reset();
    if (!Registration.IsValid())
    {
        OutError = TEXT("Extension provider identity registration is invalid.");
        return false;
    }

    FProviderIdentityState& State = GetProviderIdentityState();
    FScopeLock Lock(&State.Mutex);
    if (State.ActiveSerial != Registration.Serial
        || State.ActiveProviderId != Registration.CanonicalProviderId)
    {
        OutError = TEXT(
            "Extension provider identity registration does not own the active "
            "identity.");
        return false;
    }

    State.ActiveSerial = 0;
    State.ActiveProviderId = NAME_None;
    State.ActiveCompatibilityTag.Reset();
    Registration.Serial = 0;
    Registration.CanonicalProviderId = NAME_None;
    return true;
}

bool GetSealedProviderIdentityFingerprint(
    FString& OutFingerprint,
    FString& OutError)
{
    OutFingerprint.Reset();
    OutError.Reset();

    FProviderIdentityState& State = GetProviderIdentityState();
    FScopeLock Lock(&State.Mutex);
    if (!State.bIdentitySealed)
    {
        State.bSealedProviderPresent = State.ActiveSerial != 0;
        State.SealedProviderId = State.bSealedProviderPresent
            ? State.ActiveProviderId
            : NAME_None;
        State.SealedCompatibilityTag = State.bSealedProviderPresent
            ? State.ActiveCompatibilityTag
            : FString();
        State.SealedFingerprint = MakeIdentityFingerprint(
            State.bSealedProviderPresent,
            State.SealedProviderId,
            State.SealedCompatibilityTag);
        State.bIdentitySealed = true;
    }

    OutFingerprint = State.SealedFingerprint;
    return true;
}
}
