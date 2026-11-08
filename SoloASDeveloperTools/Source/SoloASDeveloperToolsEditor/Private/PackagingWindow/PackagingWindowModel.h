#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace SoloASDeveloperTools
{
struct FPackagingCliResult
{
    int32 ExitCode = -1;
    FString Output;
    FString Error;
};

// Arguments remain separate until the platform process boundary. No shell is used.
class IPackagingWindowBackend
{
public:
    virtual ~IPackagingWindowBackend() = default;
    virtual bool Start(const TArray<FString>& Arguments, FString& Error) = 0;
    virtual bool Poll(FPackagingCliResult& Result) = 0;
    virtual bool IsBusy() const = 0;
};

TSharedRef<IPackagingWindowBackend> MakePackagingCliBackend();
FString QuotePackagingArgument(const FString& Argument);

class FPackagingWindowModel
{
public:
    FPackagingWindowModel(TSharedRef<IPackagingWindowBackend> InBackend,
        TSharedRef<IPackagingWindowBackend> InControl, FString InProject);
    bool Request(const FString& Command);
    bool QueryJob(const FString& Id);
    bool Cancel();
    bool PollJob();
    bool SaveProject(bool bPreview, const FString& Platform = TEXT(""));
    bool SaveProfile(const FString& Name, bool bReplace);
    bool EditProfile(const FString& Name, const FString& NewName = TEXT(""));
    static bool IsValidProfileName(const FString& Name);
    bool CanPackage() const;
    bool CheckPackage(const FString& Path);
    void Tick();
    bool SetOverride(const FString& Field, const FString& JsonValue);
    void ClearOverrides();
    FString Value(const FString& Layer, const FString& Field) const;
    FString Source(const FString& Field) const;
    FString OverridesJson() const;
    bool IsBusy() const;
    bool CanCancel() const;
    bool NeedsJobPoll() const;

    FString Profile;
    FString JobId;
    FString State;
    FString Phase;
    FString Message;
    FString Log;
    FString History;
    FString Details;
    FString ProjectDiff;
    TArray<FString> Profiles;
    TSharedPtr<FJsonObject> Config;
    TSharedPtr<FJsonObject> Job;
    TSharedPtr<FJsonObject> Overrides;
    bool bReadyToPackage = false;
    bool bProjectPreviewReady = false;
    bool bCancelRequested = false;
    bool bLogTruncated = false;

private:
    bool Start(TSharedRef<IPackagingWindowBackend> Transport, const FString& Command,
        const TArray<FString>& Arguments);
    void Consume(const FPackagingCliResult& Result, const FString& Command);
    void SetJob(const TSharedPtr<FJsonObject>& Object);
    TSharedRef<IPackagingWindowBackend> Backend;
    TSharedRef<IPackagingWindowBackend> Control;
    FString Project;
    FString PendingCommand;
    FString PendingControl;
    FString PendingAction;
    int64 LogOffset = 0;
    bool bReadLogsNext = false;
    bool bLogEof = true;
    bool bRetryCancel = false;
};
}
