// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#if !__has_include("Extensions/AngelscriptExtensionHooks.h")
#include "Core/SoloASCoreExtensionsModule.h"
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
#include "AngelscriptCoreExtensionPoints.h"
#include "ClassGenerator/AngelscriptClassGenerator.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

void FSoloASCoreExtensionsModule::StartExplicitFullReloadTransaction()
{
    bool bEnabled = true;
    if (GConfig) GConfig->GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableExplicitReloadRequest"), bEnabled, GEngineIni);
    if (!bEnabled) return;
    ExplicitReloadClient = AngelscriptCoreExtensionPoints::RegisterExplicitReloadClient(TEXT("extensions.explicit-reload-request"));
    if (!ExplicitReloadClient.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("Explicit reload client registration failed."));
        return;
    }
    ExplicitReloadTicker = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FSoloASCoreExtensionsModule::TickExplicitFullReload));
}

void FSoloASCoreExtensionsModule::StopExplicitFullReloadTransaction()
{
    check(!bExplicitReloadSubmitting);
    if (ExplicitReloadTicker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(ExplicitReloadTicker);
    ExplicitReloadTicker.Reset();
    if (ActiveExplicitReload.IsValid())
    {
        LastExplicitReload = {ActiveExplicitReload.Generation, EExplicitFullReloadResult::Cancelled, false, false, TEXT("Explicit reload stopped.")};
        ActiveExplicitReload = {};
        bExplicitReloadQueued = false;
    }
    if (ExplicitReloadClient.IsValid())
    {
        const auto Result = AngelscriptCoreExtensionPoints::UnregisterExplicitReloadClient(ExplicitReloadClient);
        ensure(Result == AngelscriptCoreExtensionPoints::ERemoveResult::Removed);
        ExplicitReloadClient.Reset();
    }
}

FSoloASCoreExtensionsModule::EExplicitFullReloadResult FSoloASCoreExtensionsModule::BeginExplicitFullReload(
    const FString& Filename, double TimeoutSeconds, FExplicitFullReloadHandle& OutHandle)
{
    OutHandle = {};
    if (!IsInGameThread() || !ExplicitReloadClient.IsValid() || ActiveExplicitReload.IsValid()
        || TimeoutSeconds <= 0 || TimeoutSeconds > 60 || FPaths::IsRelative(Filename)) return EExplicitFullReloadResult::Rejected;
    FString Absolute = FPaths::ConvertRelativePathToFull(Filename);
    FPaths::NormalizeFilename(Absolute);
    FPaths::CollapseRelativeDirectories(Absolute);
    if (!Absolute.EndsWith(TEXT(".as")) || !IFileManager::Get().FileExists(*Absolute)) return EExplicitFullReloadResult::Rejected;
    FString Relative;
    for (FString Root : FAngelscriptManager::MakeAllScriptRoots())
    {
        Root = FPaths::ConvertRelativePathToFull(Root);
        FPaths::NormalizeDirectoryName(Root);
        Root += TEXT("/");
        if (!Absolute.StartsWith(Root, PLATFORM_WINDOWS ? ESearchCase::IgnoreCase : ESearchCase::CaseSensitive)) continue;
        Relative = Absolute.Mid(Root.Len());
        break;
    }
    if (Relative.IsEmpty() || Relative.Contains(TEXT(".."))) return EExplicitFullReloadResult::Rejected;
    ActiveExplicitReload = {NextExplicitReloadGeneration++, Absolute};
    ExplicitReloadRelativePath = MoveTemp(Relative);
    ExplicitReloadExpectedContentIdentity.Reset();
    ExplicitReloadDeadline = FPlatformTime::Seconds() + TimeoutSeconds;
    bExplicitReloadQueued = false;
    LastExplicitReload = {};
    OutHandle = ActiveExplicitReload;
    return EExplicitFullReloadResult::Reserved;
}

FSoloASCoreExtensionsModule::EExplicitFullReloadResult FSoloASCoreExtensionsModule::CommitExplicitFullReload(
    const FExplicitFullReloadHandle& Handle, const FString& ExpectedFingerprint)
{
    if (!IsInGameThread() || !ActiveExplicitReload.IsValid() || bExplicitReloadQueued
        || Handle.Generation != ActiveExplicitReload.Generation || Handle.CanonicalAbsolutePath != ActiveExplicitReload.CanonicalAbsolutePath)
        return EExplicitFullReloadResult::Rejected;
    if (FPlatformTime::Seconds() >= ExplicitReloadDeadline)
    {
        LastExplicitReload = {Handle.Generation, EExplicitFullReloadResult::Expired, false, false, TEXT("Explicit reload expired.")};
        ActiveExplicitReload = {};
        return EExplicitFullReloadResult::Expired;
    }
    ExplicitReloadExpectedContentIdentity = ExpectedFingerprint;
    bExplicitReloadQueued = true;
    return EExplicitFullReloadResult::CommitReady;
}

FSoloASCoreExtensionsModule::EExplicitFullReloadResult FSoloASCoreExtensionsModule::CancelExplicitFullReload(const FExplicitFullReloadHandle& Handle)
{
    if (!IsInGameThread() || bExplicitReloadSubmitting || !ActiveExplicitReload.IsValid() || Handle.Generation != ActiveExplicitReload.Generation
        || Handle.CanonicalAbsolutePath != ActiveExplicitReload.CanonicalAbsolutePath) return EExplicitFullReloadResult::Rejected;
    LastExplicitReload = {Handle.Generation, EExplicitFullReloadResult::Cancelled, false, false, TEXT("Explicit reload cancelled.")};
    ActiveExplicitReload = {};
    bExplicitReloadQueued = false;
    return EExplicitFullReloadResult::Cancelled;
}

FSoloASCoreExtensionsModule::FExplicitFullReloadReceipt FSoloASCoreExtensionsModule::GetExplicitFullReloadReceipt(const FExplicitFullReloadHandle& Handle) const
{
    if (Handle.Generation == LastExplicitReload.Generation) return LastExplicitReload;
    return {};
}

bool FSoloASCoreExtensionsModule::TickExplicitFullReload(float)
{
    using namespace AngelscriptCoreExtensionPoints;
    if (!ActiveExplicitReload.IsValid()) return true;
    if (FPlatformTime::Seconds() >= ExplicitReloadDeadline)
    {
        LastExplicitReload = {ActiveExplicitReload.Generation, EExplicitFullReloadResult::Expired, false, false, TEXT("Explicit reload expired.")};
        ActiveExplicitReload = {};
        bExplicitReloadQueued = false;
        return true;
    }
    if (!bExplicitReloadQueued) return true;
    FExplicitReloadRequest Request;
    Request.RequestId = ActiveExplicitReload.Generation;
    Request.Files.Add({ExplicitReloadRelativePath, ActiveExplicitReload.CanonicalAbsolutePath, false, ExplicitReloadExpectedContentIdentity});
    FReloadResultView Result;
    FString Error;
    bool bObservedFullReload = false;
    const FDelegateHandle ReloadHandle = FAngelscriptClassGenerator::OnPostReload.AddLambda(
        [&bObservedFullReload](bool bFullReload) { bObservedFullReload |= bFullReload; });
    EReloadAdmission Admission;
    {
        TGuardValue<bool> Submitting(bExplicitReloadSubmitting, true);
        Admission = TryExplicitReload(ExplicitReloadClient, Request,
            [&Result](const FReloadResultView& View) { Result = View; }, Error);
    }
    FAngelscriptClassGenerator::OnPostReload.Remove(ReloadHandle);
    if (Admission == EReloadAdmission::Busy) return true;
    const bool bSucceeded = Admission == EReloadAdmission::Accepted && Result.Terminal == EReloadTerminal::Committed && bObservedFullReload;
    LastExplicitReload = {ActiveExplicitReload.Generation,
        bSucceeded ? EExplicitFullReloadResult::Succeeded
            : (Admission == EReloadAdmission::Accepted && Result.Terminal == EReloadTerminal::Cancelled
                ? EExplicitFullReloadResult::Cancelled : EExplicitFullReloadResult::Failed),
        bObservedFullReload, Admission == EReloadAdmission::Accepted && Result.bVmCommitted, MoveTemp(Error)};
    if (!bSucceeded && LastExplicitReload.Error.IsEmpty())
        LastExplicitReload.Error = LastExplicitReload.Result == EExplicitFullReloadResult::Cancelled
            ? TEXT("Runtime cancelled the explicit reload.")
            : (Result.bVmCommitted && !bObservedFullReload
                ? TEXT("Runtime committed a soft reload; no full reload occurred.")
                : TEXT("Explicit reload failed; inspect Runtime diagnostics."));
    if (Admission == EReloadAdmission::Accepted) LastExplicitReload.CoreResult.Emplace(MoveTemp(Result));
    ActiveExplicitReload = {};
    bExplicitReloadQueued = false;
    return true;
}
#endif
#endif
