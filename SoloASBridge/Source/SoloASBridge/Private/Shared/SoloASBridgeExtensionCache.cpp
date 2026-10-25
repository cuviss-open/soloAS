#include "Shared/SoloASBridgeExtensionCache.h"
#include "Shared/SoloASBridgeExtensionCacheValidation.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "HAL/FileManagerGeneric.h"
#include "Misc/AutomationTest.h"
#endif

namespace SoloASBridge::Extensions
{
namespace
{
using CacheInternal::FCacheArtifactMember;
using CacheInternal::GetCompanionPath;
using CacheInternal::MakeMembers;
using CacheInternal::ValidateFingerprint;
using CacheInternal::ValidateOperationInputs;

FString ComputeDigest(const TArray<uint8>& Bytes)
{
    uint8 Digest[FSHA1::DigestSize];
    FSHA1 Hash;
    if (!Bytes.IsEmpty())
        Hash.Update(Bytes.GetData(), Bytes.Num());
    Hash.Final();
    Hash.GetHash(Digest);
    return BytesToHex(Digest, FSHA1::DigestSize).ToLower();
}

bool AppendAscii(
    const FString& Text,
    TArray<uint8>& OutBytes,
    FString& OutError)
{
    for (const TCHAR Character : Text)
    {
        if (Character > 127)
        {
            OutError = TEXT("Cache companion contains a non-ASCII field.");
            OutBytes.Reset();
            return false;
        }
        OutBytes.Add(static_cast<uint8>(Character));
    }
    return true;
}

bool LoadMembers(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath,
    TArray<FCacheArtifactMember>& OutMembers,
    FString& OutError)
{
    OutMembers = MakeMembers(Contract, ArtifactPath);
    for (FCacheArtifactMember& Member : OutMembers)
    {
        if (!FFileHelper::LoadFileToArray(Member.Bytes, *Member.Path))
        {
            OutError = FString::Printf(
                TEXT("Required cache family member cannot be read: %s"),
                *Member.Path);
            OutMembers.Reset();
            return false;
        }
    }
    return true;
}

bool BuildCompanion(
    const FExtensionCacheContract& Contract,
    const FString& ExpectedFingerprint,
    const TArray<FCacheArtifactMember>& Members,
    TArray<uint8>& OutCompanion,
    FString& OutError)
{
    OutCompanion.Reset();
    const FString Header = FString::Printf(
        TEXT("format=%s\nfingerprint=%s\nfamily=%s\nmember_count=%d\n"),
        *Contract.FormatId,
        *ExpectedFingerprint,
        *Contract.FamilyId,
        Members.Num());
    if (!AppendAscii(Header, OutCompanion, OutError))
        return false;

    for (int32 Index = 0; Index < Members.Num(); ++Index)
    {
        const FCacheArtifactMember& Member = Members[Index];
        const FString Fields = FString::Printf(
            TEXT("member_%d_role=%s\n")
            TEXT("member_%d_size=%d\n")
            TEXT("member_%d_sha1=%s\n"),
            Index,
            *Member.Role,
            Index,
            Member.Bytes.Num(),
            Index,
            *ComputeDigest(Member.Bytes));
        if (!AppendAscii(Fields, OutCompanion, OutError))
            return false;
    }
    return true;
}

bool LoadMembersAndBuildCompanion(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath,
    const FString& ExpectedFingerprint,
    TArray<FCacheArtifactMember>& OutMembers,
    TArray<uint8>& OutCompanion,
    FString& OutError)
{
    return LoadMembers(Contract, ArtifactPath, OutMembers, OutError)
        && BuildCompanion(
            Contract,
            ExpectedFingerprint,
            OutMembers,
            OutCompanion,
            OutError);
}

bool VerifyExactBytes(
    const FString& Path,
    const TArray<uint8>& Expected,
    const TCHAR* Label,
    FString& OutError)
{
    TArray<uint8> Actual;
    if (!FFileHelper::LoadFileToArray(Actual, *Path)
        || Actual.Num() != Expected.Num()
        || ComputeDigest(Actual) != ComputeDigest(Expected)
        || Actual != Expected)
    {
        OutError = FString::Printf(
            TEXT("%s bytes differ from the validated source: %s"),
            Label,
            *Path);
        return false;
    }
    return true;
}

bool VerifyCompanion(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath,
    const TArray<uint8>& Expected,
    FString& OutError)
{
    const FString CompanionPath = GetCompanionPath(Contract, ArtifactPath);
    if (!IFileManager::Get().FileExists(*CompanionPath))
    {
        OutError = FString::Printf(
            TEXT("Cache companion is missing: %s"), *CompanionPath);
        return false;
    }
    return VerifyExactBytes(
        CompanionPath,
        Expected,
        TEXT("Cache companion"),
        OutError);
}

bool VerifyPublishedMembers(
    const TArray<FCacheArtifactMember>& Members,
    FString& OutError)
{
    for (const FCacheArtifactMember& Member : Members)
    {
        if (!VerifyExactBytes(
            Member.Path,
            Member.Bytes,
            TEXT("Published cache family member"),
            OutError))
        {
            return false;
        }
    }
    return true;
}

FString MakeUniqueSiblingTempPath(const FString& TargetPath)
{
    return FString::Printf(
        TEXT("%s.%s.tmp"),
        *TargetPath,
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

void DeletePathsBestEffort(const TArray<FString>& Paths)
{
    for (const FString& Path : Paths)
        IFileManager::Get().Delete(*Path, false, true, true);
}

bool DeletePublishTarget(
    const FString& Path,
    FString& OutError,
    IFileManager& FileManager = IFileManager::Get())
{
    const FString WritePath = FileManager.ConvertToAbsolutePathForExternalAppForWrite(*Path);
    if (FileManager.DirectoryExists(*WritePath))
    {
        OutError = FString::Printf(
            TEXT("Cache publish target is a directory: %s"), *WritePath);
        return false;
    }
    if (FileManager.FileExists(*WritePath)
        && !FileManager.Delete(*WritePath, false, true, true))
    {
        OutError = FString::Printf(
            TEXT("Stale cache publish target cannot be deleted: %s"), *WritePath);
        return false;
    }
    if (FileManager.FileExists(*WritePath))
    {
        OutError = FString::Printf(
            TEXT("Stale cache publish target remains after deletion: %s"),
            *WritePath);
        return false;
    }
    return true;
}

#if WITH_DEV_AUTOMATION_TESTS
class FCachePublishSandboxFileManager final : public FFileManagerGeneric
{
public:
    const FString SourcePath = TEXT("/Project/Script/PrecompiledScript.Cache");
    const FString WritePath = TEXT("/Project/Saved/Cooked/Windows/Project/Script/PrecompiledScript.Cache");
    bool bSourceExists = true;
    bool bWriteExists = true;
    bool bWriteIsDirectory = false;
    bool bDeleteSucceeds = true;
    bool bKeepAfterDelete = false;
    FString DeletedPath;

    FString ConvertToAbsolutePathForExternalAppForWrite(const TCHAR* Filename) override
    {
        return SourcePath == Filename ? WritePath : FString(Filename);
    }

    bool DirectoryExists(const TCHAR* Filename) override
    {
        return WritePath == Filename && bWriteIsDirectory;
    }

    bool FileExists(const TCHAR* Filename) override
    {
        return (SourcePath == Filename && (bSourceExists || bWriteExists))
            || (WritePath == Filename && bWriteExists);
    }

    bool Delete(const TCHAR* Filename, bool, bool, bool) override
    {
        DeletedPath = Filename;
        if (!bDeleteSucceeds)
            return false;
        if (!bKeepAfterDelete && WritePath == Filename)
            bWriteExists = false;
        return true;
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCachePublishSandboxTest,
    "SoloAS.ExtensionCache.PublishSandboxOverlay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCachePublishSandboxTest::RunTest(const FString&)
{
    FCachePublishSandboxFileManager Files;
    FString Error;
    TestTrue(TEXT("Delete the write overlay despite the retained source cache"),
        DeletePublishTarget(Files.SourcePath, Error, Files));
    TestEqual(TEXT("Delete targets the actual write path"), Files.DeletedPath, Files.WritePath);
    TestFalse(TEXT("Write overlay was removed"), Files.bWriteExists);
    TestTrue(TEXT("Source cache is retained and remains readable"),
        Files.bSourceExists && Files.FileExists(*Files.SourcePath));
    TestTrue(TEXT("Missing overlay also succeeds with a retained source cache"),
        DeletePublishTarget(Files.SourcePath, Error, Files));
    TestTrue(TEXT("No error"), Error.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCachePublishDeleteFailureTest,
    "SoloAS.ExtensionCache.PublishDeleteFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCachePublishDeleteFailureTest::RunTest(const FString&)
{
    FCachePublishSandboxFileManager Files;
    FString Error;
    Files.bDeleteSucceeds = false;
    TestFalse(TEXT("A real deletion failure rejects publication"),
        DeletePublishTarget(Files.SourcePath, Error, Files));
    TestTrue(TEXT("Error identifies the write target"),
        Error.Contains(TEXT("cannot be deleted")) && Error.Contains(Files.WritePath));
    Files.bDeleteSucceeds = true;
    Files.bKeepAfterDelete = true;
    TestFalse(TEXT("A residual write target rejects publication"),
        DeletePublishTarget(Files.SourcePath, Error, Files));
    TestTrue(TEXT("Residual target error is retained"), Error.Contains(TEXT("remains after deletion")));
    Files.bWriteIsDirectory = true;
    TestFalse(TEXT("A directory is never a publish target"),
        DeletePublishTarget(Files.SourcePath, Error, Files));
    TestTrue(TEXT("Directory error is retained"), Error.Contains(TEXT("is a directory")));
    return true;
}
#endif

bool WriteSnapshotMembers(
    const FString& SnapshotPath,
    const TArray<FCacheArtifactMember>& Members,
    TArray<FString>& OutPaths,
    FString& OutError)
{
    OutPaths.Reset();
    for (const FCacheArtifactMember& Member : Members)
    {
        const FString MemberPath = SnapshotPath + Member.Suffix;
        OutPaths.Add(MemberPath);
        if (!FFileHelper::SaveArrayToFile(Member.Bytes, *MemberPath)
            || !VerifyExactBytes(
                MemberPath,
                Member.Bytes,
                TEXT("Validated cache snapshot"),
                OutError))
        {
            DeletePathsBestEffort(OutPaths);
            if (OutError.IsEmpty())
            {
                OutError = FString::Printf(
                    TEXT("Validated cache snapshot write failed: %s"),
                    *MemberPath);
            }
            OutPaths.Reset();
            return false;
        }
    }
    return true;
}
}

FValidatedCacheSnapshot::~FValidatedCacheSnapshot()
{
    Reset();
}

FValidatedCacheSnapshot::FValidatedCacheSnapshot(
    FValidatedCacheSnapshot&& Other) noexcept
    : SnapshotPath(MoveTemp(Other.SnapshotPath))
    , SnapshotMemberPaths(MoveTemp(Other.SnapshotMemberPaths))
{
    Other.SnapshotPath.Reset();
    Other.SnapshotMemberPaths.Reset();
}

FValidatedCacheSnapshot& FValidatedCacheSnapshot::operator=(
    FValidatedCacheSnapshot&& Other) noexcept
{
    if (this != &Other)
    {
        Reset();
        SnapshotPath = MoveTemp(Other.SnapshotPath);
        SnapshotMemberPaths = MoveTemp(Other.SnapshotMemberPaths);
        Other.SnapshotPath.Reset();
        Other.SnapshotMemberPaths.Reset();
    }
    return *this;
}

bool FValidatedCacheSnapshot::IsValid() const
{
    return !SnapshotPath.IsEmpty() && !SnapshotMemberPaths.IsEmpty();
}

const FString& FValidatedCacheSnapshot::GetPath() const
{
    check(IsValid());
    return SnapshotPath;
}

void FValidatedCacheSnapshot::Reset()
{
    DeletePathsBestEffort(SnapshotMemberPaths);
    SnapshotPath.Reset();
    SnapshotMemberPaths.Reset();
}

bool PrepareExtensionCachePublish(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath,
    FString& OutError)
{
    OutError.Reset();
    if (!ValidateOperationInputs(Contract, ArtifactPath, OutError))
        return false;

    const TArray<FCacheArtifactMember> Members =
        MakeMembers(Contract, ArtifactPath);
    for (const FCacheArtifactMember& Member : Members)
    {
        if (!DeletePublishTarget(Member.Path, OutError))
            return false;
    }
    return DeletePublishTarget(
        GetCompanionPath(Contract, ArtifactPath), OutError);
}

bool CreateValidatedCacheSnapshot(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath,
    const FString& ExpectedFingerprint,
    const FString& SnapshotDirectory,
    FValidatedCacheSnapshot& OutSnapshot,
    FString& OutError)
{
    OutError.Reset();
    OutSnapshot.Reset();
    if (!ValidateOperationInputs(Contract, ArtifactPath, OutError)
        || !ValidateFingerprint(ExpectedFingerprint, OutError))
    {
        return false;
    }
    if (SnapshotDirectory.IsEmpty())
    {
        OutError = TEXT("Validated cache snapshot directory is empty.");
        return false;
    }

    TArray<FCacheArtifactMember> Members;
    TArray<uint8> ExpectedCompanion;
    if (!LoadMembersAndBuildCompanion(
        Contract,
        ArtifactPath,
        ExpectedFingerprint,
        Members,
        ExpectedCompanion,
        OutError)
        || !VerifyCompanion(
            Contract,
            ArtifactPath,
            ExpectedCompanion,
            OutError))
    {
        return false;
    }

    if (!IFileManager::Get().MakeDirectory(*SnapshotDirectory, true))
    {
        OutError = FString::Printf(
            TEXT("Validated cache snapshot directory cannot be created: %s"),
            *SnapshotDirectory);
        return false;
    }

    const FString SnapshotPath = FPaths::Combine(
        SnapshotDirectory,
        FString::Printf(
            TEXT("%s.%s.snapshot"),
            *FPaths::GetCleanFilename(ArtifactPath),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    if (!WriteSnapshotMembers(
        SnapshotPath,
        Members,
        OutSnapshot.SnapshotMemberPaths,
        OutError))
    {
        return false;
    }
    OutSnapshot.SnapshotPath = SnapshotPath;
    return true;
}

bool WriteExtensionCacheCompanion(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath,
    const FString& ExpectedFingerprint,
    FString& OutError)
{
    OutError.Reset();
    if (!ValidateOperationInputs(Contract, ArtifactPath, OutError)
        || !ValidateFingerprint(ExpectedFingerprint, OutError))
    {
        return false;
    }

    TArray<FCacheArtifactMember> Members;
    TArray<uint8> Content;
    if (!LoadMembersAndBuildCompanion(
        Contract,
        ArtifactPath,
        ExpectedFingerprint,
        Members,
        Content,
        OutError))
    {
        return false;
    }

    const FString CompanionPath = GetCompanionPath(Contract, ArtifactPath);
    const FString TempPath = MakeUniqueSiblingTempPath(CompanionPath);
    if (!FFileHelper::SaveArrayToFile(Content, *TempPath)
        || !VerifyExactBytes(
            TempPath,
            Content,
            TEXT("Cache companion temp"),
            OutError))
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        if (OutError.IsEmpty())
        {
            OutError = FString::Printf(
                TEXT("Cache companion temp write failed: %s"), *TempPath);
        }
        return false;
    }
    if (!IFileManager::Get().Move(
        *CompanionPath,
        *TempPath,
        true,
        true,
        false,
        true))
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        OutError = FString::Printf(
            TEXT("Cache companion replace failed: %s"), *CompanionPath);
        return false;
    }
    if (!VerifyPublishedMembers(Members, OutError)
        || !VerifyExactBytes(
            CompanionPath,
            Content,
            TEXT("Cache companion"),
            OutError))
    {
        IFileManager::Get().Delete(*CompanionPath, false, true, true);
        return false;
    }
    return true;
}
}
