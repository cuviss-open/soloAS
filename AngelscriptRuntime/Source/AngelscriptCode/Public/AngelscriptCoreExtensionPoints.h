                                                                                                       
// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
class asIScriptEngine;
class UScriptStruct;

                                                                                                     
                                                                                                        
#include "AngelscriptCoreAdmission.h"
#include "AngelscriptModuleGeneration.h"
enum class ECompileResult : uint8;
namespace AngelscriptCoreExtensionPoints
{
enum class EReloadAdmission : uint8 { Accepted, Busy, Invalid };
enum class EReloadTerminal : uint8 { Committed, Failed, Cancelled };
enum class EReloadFailureStage : uint8 { None, Preprocess, Admission, Compile, NativeReload, Shutdown };
struct FReloadFile
{
    FString RelativePath;
    FString AbsolutePath;
    bool bDeleted = false;
    // Optional SHA-1 of UTF-8 encoded decoded text, as reported by FReloadInput.
    FString ExpectedContentIdentity;
};
struct FReloadInput : FReloadFile { FString ContentIdentity; };
struct FExplicitReloadRequest
{
    uint64 RequestId = 0;
    bool bFullReload = true;
    TArray<FReloadFile> Files;
};
// Borrowed only during Complete; copy stable values before returning.
struct FReloadResultView
{
    uint64 RequestId = 0;
    uint64 BatchId = 0;
    EReloadTerminal Terminal = EReloadTerminal::Failed;
    EReloadFailureStage FailureStage = EReloadFailureStage::Preprocess;
    TOptional<ECompileResult> CompileResult;
    bool bVmCommitted = false;
    TArray<FReloadInput> Inputs;
    TArray<FModuleGenerationId> CommittedModules;
};
ANGELSCRIPTCODE_API FDelegateHandle RegisterExplicitReloadClient(FName FeatureId);
ANGELSCRIPTCODE_API ERemoveResult UnregisterExplicitReloadClient(FDelegateHandle Client);
ANGELSCRIPTCODE_API EReloadAdmission TryExplicitReload(FDelegateHandle Client,
    const FExplicitReloadRequest& Request,
    TFunctionRef<void(const FReloadResultView&)> Complete, FString& Error);
}
                                                                                                      

                                                                                                       
namespace AngelscriptCoreExtensionPoints
{
struct FReloadDiagnostic
{
    FString Filename;
    FString Message;
    int32 Line = 0;
    int32 Column = 0;
    bool bIsError = false;
    bool bIsInfo = false;
};
// Game-thread callbacks and diagnostics are borrowed for the call only.
// Registrations must be removed before the consumer unloads.
ANGELSCRIPTCODE_API FDelegateHandle RegisterAutomaticReloadObserver(FName FeatureId,
    TFunction<bool()> ShouldDefer, TFunction<void(uint64)> OnBegin,
    TFunction<void(uint64, bool, const TArray<FReloadDiagnostic>&)> OnComplete);
ANGELSCRIPTCODE_API ERemoveResult UnregisterAutomaticReloadObserver(FDelegateHandle Handle);
extern ANGELSCRIPTCODE_API bool GHasReloadObservers;
extern ANGELSCRIPTCODE_API bool GCollectingReloadDiagnostics;
ANGELSCRIPTCODE_API bool ShouldDeferAutomaticReload();
ANGELSCRIPTCODE_API void NotifyAutomaticReloadBegin();
ANGELSCRIPTCODE_API void NotifyAutomaticReloadComplete(bool bSuccess);
ANGELSCRIPTCODE_API void CaptureReloadDiagnostic(const FString& Filename, const FString& Message,
    int32 Line, int32 Column, bool bIsError, bool bIsInfo);
// A read-only game-thread check against this process's live DebugServer clients.
ANGELSCRIPTCODE_API bool HasDebugClientEndpoint(const FString& Endpoint);
}
                                                                                                     

                                                                                                   
namespace AngelscriptCoreExtensionPoints
{
struct FDebugTypeAlias
{
    FString InternalName;
    FString PublicName;
    FString SourceScope;
    FString ParentName;
};
// Game-thread snapshots only. The consumer owns its data and removes its provider before unloading.
ANGELSCRIPTCODE_API FDelegateHandle RegisterDebugTypeAliasProvider(FName FeatureId,
    TFunction<void(TArray<FDebugTypeAlias>&)> Collect);
ANGELSCRIPTCODE_API ERemoveResult UnregisterDebugTypeAliasProvider(FDelegateHandle Handle);
ANGELSCRIPTCODE_API void CollectDebugTypeAliases(TArray<FDebugTypeAlias>& Out);
}
                                                                                                 

                                                                                                       
namespace AngelscriptCoreExtensionPoints
{
enum class ECacheAdmission : uint8 { Completed, Busy, Invalid };
// A copy of a handle is not another hold. Clone explicitly before copying values.
struct FModuleGenerationLease { uint64 Id = 0; FModuleGenerationId Module; };
// Capture-only ownership: callbacks destroy captured values before releasing holds.
ANGELSCRIPTCODE_API FDelegateHandle RegisterModuleGenerationLeaseClient(FName FeatureId, TFunction<void()> OnClosing);
ANGELSCRIPTCODE_API ERemoveResult UnregisterModuleCacheClient(FDelegateHandle Client);
// Calls OnClosing once before shutdown; Busy preserves external invocation holds.
ANGELSCRIPTCODE_API ERemoveResult CloseModuleCacheClient(FDelegateHandle Client);
// GT only, including an existing execution reader. Neither operation acquires a
// writer or collects synchronously. Release after every captured value is destroyed.
// OnClosing must destroy captures before shutdown. Post-shutdown holds stay Busy
// and are preserved for process exit; crossing Runtime unload is unsupported.
ANGELSCRIPTCODE_API ECacheAdmission AcquireModuleGenerationLease(FDelegateHandle Client,
    const UScriptStruct* CaptureType, FModuleGenerationLease& Lease);
ANGELSCRIPTCODE_API ECacheAdmission CloneModuleGenerationLease(FDelegateHandle Client,
    const FModuleGenerationLease& Existing, FModuleGenerationLease& Lease);
ANGELSCRIPTCODE_API ERemoveResult ReleaseModuleGenerationLease(FDelegateHandle Client,
    const FModuleGenerationLease& Lease);
}
                                                                                                     
