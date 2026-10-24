#include "Shared/SoloASBridgeExtensionCacheValidation.h"

#include "Misc/Paths.h"

namespace SoloASBridge::Extensions::CacheInternal
{
namespace
{
bool IsManifestToken(const FString& Token)
{
    if (Token.IsEmpty())
        return false;

    for (const TCHAR Character : Token)
    {
        if (Character < 33 || Character > 126)
            return false;
    }
    return true;
}

bool IsPathSuffix(const FString& Suffix, bool bMayBeEmpty)
{
    if (Suffix.IsEmpty())
        return bMayBeEmpty;

    if (Suffix[0] != TEXT('.'))
        return false;

    for (const TCHAR Character : Suffix)
    {
        if (Character < 33
            || Character > 126
            || Character == TEXT('/')
            || Character == TEXT('\\'))
        {
            return false;
        }
    }
    return true;
}

bool ValidateContract(
    const FExtensionCacheContract& Contract,
    FString& OutError)
{
    if (!IsManifestToken(Contract.FormatId))
    {
        OutError = TEXT("Cache format ID must be a non-empty ASCII token.");
        return false;
    }
    if (!IsManifestToken(Contract.FamilyId))
    {
        OutError = TEXT("Cache family ID must be a non-empty ASCII token.");
        return false;
    }
    if (!IsPathSuffix(Contract.CompanionSuffix, false))
    {
        OutError = TEXT(
            "Cache companion suffix must be a non-empty safe ASCII suffix.");
        return false;
    }
    if (Contract.Members.IsEmpty())
    {
        OutError = TEXT("Cache contract must declare at least one member.");
        return false;
    }
    if (!Contract.Members[0].Suffix.IsEmpty())
    {
        OutError = TEXT("The primary cache member must use an empty suffix.");
        return false;
    }

    for (int32 Index = 0; Index < Contract.Members.Num(); ++Index)
    {
        const FExtensionCacheMemberContract& Member = Contract.Members[Index];
        if (!IsManifestToken(Member.Role))
        {
            OutError = FString::Printf(
                TEXT("Cache member %d role must be a non-empty ASCII token."),
                Index);
            return false;
        }
        if (!IsPathSuffix(Member.Suffix, Index == 0))
        {
            OutError = FString::Printf(
                TEXT("Cache member %d suffix is invalid."), Index);
            return false;
        }
        if (Member.Suffix.Equals(
            Contract.CompanionSuffix,
            ESearchCase::IgnoreCase))
        {
            OutError = TEXT(
                "Cache companion suffix collides with a family member.");
            return false;
        }

        for (int32 Previous = 0; Previous < Index; ++Previous)
        {
            const FExtensionCacheMemberContract& Prior =
                Contract.Members[Previous];
            if (Member.Role.Equals(Prior.Role, ESearchCase::IgnoreCase))
            {
                OutError = TEXT("Cache member roles must be unique.");
                return false;
            }
            if (Member.Suffix.Equals(Prior.Suffix, ESearchCase::IgnoreCase))
            {
                OutError = TEXT("Cache member suffixes must be unique.");
                return false;
            }
        }
    }
    return true;
}
}

bool ValidateOperationInputs(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath,
    FString& OutError)
{
    if (!ValidateContract(Contract, OutError))
        return false;
    if (ArtifactPath.IsEmpty()
        || FPaths::GetCleanFilename(ArtifactPath).IsEmpty())
    {
        OutError = TEXT("Cache artifact path must identify a file.");
        return false;
    }
    return true;
}

bool ValidateFingerprint(
    const FString& ExpectedFingerprint,
    FString& OutError)
{
    if (!IsManifestToken(ExpectedFingerprint))
    {
        OutError = TEXT(
            "Expected cache fingerprint must be a non-empty ASCII token.");
        return false;
    }
    return true;
}

FString GetCompanionPath(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath)
{
    return ArtifactPath + Contract.CompanionSuffix;
}

TArray<FCacheArtifactMember> MakeMembers(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath)
{
    TArray<FCacheArtifactMember> Members;
    Members.Reserve(Contract.Members.Num());
    for (const FExtensionCacheMemberContract& Definition : Contract.Members)
    {
        FCacheArtifactMember& Member = Members.AddDefaulted_GetRef();
        Member.Role = Definition.Role;
        Member.Suffix = Definition.Suffix;
        Member.Path = ArtifactPath + Definition.Suffix;
    }
    return Members;
}
}
