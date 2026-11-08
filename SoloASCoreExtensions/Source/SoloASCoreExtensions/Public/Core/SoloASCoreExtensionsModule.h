#if __has_include("Extensions/AngelscriptExtensionHooks.h")
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR
#include "AngelscriptManager.h"
#endif
#include "Modules/ModuleInterface.h"
#include "Templates/UniquePtr.h"

class UASClass;


class FSoloASCoreExtensionsRegistration;

class SOLOASCOREEXTENSIONS_API FSoloASCoreExtensionsModule final : public IModuleInterface
{
public:
    FSoloASCoreExtensionsModule();
    virtual ~FSoloASCoreExtensionsModule() override;
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    // Script functions keep native pointers for the VM lifetime.
    virtual bool SupportsDynamicReloading() override { return false; }



private:
    friend class FCoreExtensionRegistrationLifecycleTest;
    TUniquePtr<FSoloASCoreExtensionsRegistration> ExtensionRegistration;


};

namespace SoloASCoreExtensions::ScriptInterfaces
{
/** Copies overrides into a caller-created current-generation instance. Both
 * objects must be ordinary, childless UObject Blueprint instances. Validation
 * failure leaves NewInstance untouched; references and holder replacement remain
 * the caller's responsibility. New fields/defaults retain the new CDO values.
 */
SOLOASCOREEXTENSIONS_API bool TransferBlueprintGenerationValues(
    UObject* OldInstance, UObject* NewInstance, FString& Error);
}

#else
#pragma once
#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#if WITH_EDITOR
#include "AngelscriptManager.h"
#endif
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
#include "Containers/Ticker.h"
#include "AngelscriptCoreExtensionPoints.h"
#endif
class SOLOASCOREEXTENSIONS_API FSoloASCoreExtensionsModule final : public IModuleInterface
{
public:
    FSoloASCoreExtensionsModule();
    virtual ~FSoloASCoreExtensionsModule() override;
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    virtual bool SupportsDynamicReloading() override { return false; }
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
    enum class EExplicitFullReloadResult : uint8
    { Rejected, Reserved, AwaitingWatcher, CommitReady, Succeeded, Failed, Cancelled, Expired };
    struct FExplicitFullReloadHandle
    {
        uint64 Generation = 0;
        FString CanonicalAbsolutePath;
        bool IsValid() const { return Generation != 0 && !CanonicalAbsolutePath.IsEmpty(); }
    };
    struct FExplicitFullReloadReceipt
    {
        uint64 Generation = 0;
        EExplicitFullReloadResult Result = EExplicitFullReloadResult::Rejected;
        bool bObservedActualFullReload = false;
        bool bVmCommitted = false;
        FString Error;
        TOptional<AngelscriptCoreExtensionPoints::FReloadResultView> CoreResult;
    };
    EExplicitFullReloadResult BeginExplicitFullReload(const FString& Filename, double TimeoutSeconds, FExplicitFullReloadHandle& OutHandle);
    EExplicitFullReloadResult CommitExplicitFullReload(const FExplicitFullReloadHandle& Handle, const FString& ExpectedFingerprint);
    EExplicitFullReloadResult CancelExplicitFullReload(const FExplicitFullReloadHandle& Handle);
    FExplicitFullReloadReceipt GetExplicitFullReloadReceipt(const FExplicitFullReloadHandle& Handle) const;
private:
    friend class FCEExplicitReloadTest;
    void StartExplicitFullReloadTransaction();
    void StopExplicitFullReloadTransaction();
    bool TickExplicitFullReload(float DeltaTime);
    FDelegateHandle ExplicitReloadClient;
    FTSTicker::FDelegateHandle ExplicitReloadTicker;
    FExplicitFullReloadHandle ActiveExplicitReload;
    FExplicitFullReloadReceipt LastExplicitReload;
    FString ExplicitReloadRelativePath;
    FString ExplicitReloadExpectedContentIdentity;
    double ExplicitReloadDeadline = 0;
    uint64 NextExplicitReloadGeneration = 1;
    bool bExplicitReloadQueued = false;
    bool bExplicitReloadSubmitting = false;
#endif
};
#endif

namespace SoloASCoreExtensions
{
SOLOASCOREEXTENSIONS_API bool QueryPackagingState(FString& OutFingerprint, FString& OutError);
SOLOASCOREEXTENSIONS_API bool ValidatePackagingInputs(const FString& Directory, FString& OutError);
}
