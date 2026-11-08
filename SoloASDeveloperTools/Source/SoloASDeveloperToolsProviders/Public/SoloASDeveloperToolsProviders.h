// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeature.h"

namespace SoloASDeveloperTools
{
inline constexpr int32 ContractVersion = 3;

struct FPackagingStep
{
    FString Name;
    // precheck, after_generate, before_jit_sync, before_stage, after_package_check
    FString Phase;
    TArray<FString> Modes;
    TArray<FString> Inputs;
    TArray<FString> Artifacts;
};

struct FMcpTool
{
    FString Name;
    FString Description;
    FString InputSchemaJson = TEXT("{\"type\":\"object\"}");
    bool bReadOnly = true;
    bool bDestructive = false;
};

struct FCallResult
{
    bool bOk = false;
    FString Code;
    FString Message;
    FString ResultJson = TEXT("{}");
};

// All registration, discovery and calls run synchronously on the game thread.
// Never retain a provider pointer across calls or across module shutdown.
class SOLOASDEVELOPERTOOLSPROVIDERS_API IPackagingStepProvider : public IModularFeature
{
public:
    static FName FeatureName();
    virtual FString GetProviderId() const = 0;
    virtual FString GetVersion() const = 0;
    virtual TArray<FPackagingStep> GetSteps() const = 0;
    virtual FCallResult Execute(const FString& Name, const FString& JobId, const FString& ArgumentsJson) = 0;
    virtual void Cancel(const FString& JobId) = 0;
    // Stop accepting work; cancel/join owned workers and undo temporary changes.
    virtual void Quiesce() = 0;
};

class SOLOASDEVELOPERTOOLSPROVIDERS_API IMcpToolProvider : public IModularFeature
{
public:
    static FName FeatureName();
    virtual FString GetProviderId() const = 0;
    virtual FString GetVersion() const = 0;
    virtual TArray<FMcpTool> GetTools() const = 0;
    virtual FCallResult Call(const FString& Name, const FString& JobId, const FString& ArgumentsJson) = 0;
    virtual void Cancel(const FString& JobId) = 0;
    virtual void Quiesce() = 0;
};

// Register through these overloads, rather than directly through IModularFeatures.
// IDs and descriptors are immutable until unregister. Failed registration owns nothing.
SOLOASDEVELOPERTOOLSPROVIDERS_API bool Register(IPackagingStepProvider& Provider, FString& Error);
SOLOASDEVELOPERTOOLSPROVIDERS_API bool Register(IMcpToolProvider& Provider, FString& Error);
SOLOASDEVELOPERTOOLSPROVIDERS_API bool Unregister(IPackagingStepProvider& Provider);
SOLOASDEVELOPERTOOLSPROVIDERS_API bool Unregister(IMcpToolProvider& Provider);

// Stateless discovery observes providers loaded before or after the consumer.
// Request/response objects follow Docs/Contracts.md; no UObject or provider escapes.
SOLOASDEVELOPERTOOLSPROVIDERS_API FString HandleRequest(const FString& RequestJson, bool& bOk);
}
