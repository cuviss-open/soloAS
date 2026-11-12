                                                                                                       
// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
class asIScriptEngine;
class UScriptStruct;

                                                                                                     
                                                                                                         
#include "AngelscriptCoreAdmission.h"
namespace AngelscriptCoreExtensionPoints
{
struct FPreprocessSource
{
    FString RelativePath;
    FString AbsolutePath;
    FString Source;
};
struct FPreprocessDependency
{
    FString RelativePath;
    FString AbsolutePath;
};
using FPreprocessDependencyProvider = TFunction<void(TConstArrayView<FPreprocessSource>, TArray<FPreprocessDependency>&)>;
ANGELSCRIPTCODE_API FDelegateHandle RegisterPreprocessDependencyProvider(FName FeatureId, FPreprocessDependencyProvider Provider);
ANGELSCRIPTCODE_API ERemoveResult UnregisterPreprocessDependencyProvider(FDelegateHandle Handle);
extern ANGELSCRIPTCODE_API bool GHasPreprocessDependencyProvider;
ANGELSCRIPTCODE_API void CollectPreprocessDependencies(TConstArrayView<FPreprocessSource> Sources, TArray<FPreprocessDependency>& Out);
}
                                                                                                       
                                                                                                        
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
struct FVerifiedCacheFileView
{
    FStringView RelativePath;
    FStringView ContentIdentity;
    TConstArrayView<uint8> Bytes;
};
struct FVerifiedCacheSetView
{
    FStringView Format;
    FStringView BuildIdentity;
    FStringView ConfigurationIdentity;
    FGuid DataGuid;
    TConstArrayView<uint8> Manifest;
    TConstArrayView<FVerifiedCacheFileView> Files;
};
// Owned snapshot; callers receive only a const shared owner, never VM-backed bytes.
struct FProducedModuleCacheFile
{
    FString ModuleName;
    FString RelativePath;
    FString ContentIdentity;
    FGuid DataGuid;
    TArray<FString> Dependencies;
    TArray<uint8> Bytes;
};
struct FProducedModuleCacheSet
{
    FString BuildIdentity;
    FString ConfigurationIdentity;
    FGuid SetGuid;
    TArray<FProducedModuleCacheFile> Files;
    FString NativeTypeLayoutsJson;
    TArray<uint8> Manifest;
};
// Synchronous GT safe point. Descriptors must name their exact live ScriptModule
// in ProducerEngine; the caller keeps VM/descriptors unchanged for the call.
// Serializes only the selected modules using the Runtime cache format, preserves
// each serializer-created DataGuid, and seals native layouts into a complete v1
// manifest. No registration, VM mutation or signing. Busy/Invalid clear Output;
// success owns all bytes and retains no VM, descriptor or provider resources.
ANGELSCRIPTCODE_API ECacheAdmission ProduceModuleCacheSet(asIScriptEngine* ProducerEngine,
    TConstArrayView<TSharedRef<FAngelscriptModuleDesc>> Modules,
    TSharedPtr<const FProducedModuleCacheSet>& Output, FString& Error);
struct FModuleCacheSetHandle { uint64 Id = 0; };
// A copy of a handle is not another hold. Clone explicitly before copying values.
struct FModuleGenerationLease { uint64 Id = 0; FModuleGenerationId Module; };
struct FCacheLoadResult
{
    FString Error;
    bool bSucceeded = false;
    bool bVmCommitted = false;
    FModuleCacheSetHandle Set;
    TArray<FModuleGenerationId> CommittedModules;
};
struct FCacheRemoveResult
{
    FString Error;
    bool bSucceeded = false;
    TArray<FModuleGenerationId> RemovedModules;
};
ANGELSCRIPTCODE_API FDelegateHandle RegisterModuleCacheClient(FName FeatureId, TFunction<void()> OnClosing = {});
// Capture-only client: shares the owner/revoke protocol without retaining the
// H5/H6 compile surface. All client kinds use UnregisterModuleCacheClient.
ANGELSCRIPTCODE_API FDelegateHandle RegisterModuleGenerationLeaseClient(FName FeatureId, TFunction<void()> OnClosing);
ANGELSCRIPTCODE_API ERemoveResult UnregisterModuleCacheClient(FDelegateHandle Client);
// Calls OnClosing once, before execution shutdown. Busy retains all holds and
// resources. New holds/actions are refused once this client starts closing.
ANGELSCRIPTCODE_API ERemoveResult CloseModuleCacheClient(FDelegateHandle Client);
#if WITH_DEV_AUTOMATION_TESTS
// Seal immediately after serialization, on the GT in the same unmodified VM.
// Returns the exact JSON array for the signed v1 manifest's nativeTypeLayouts:
// {declaration: canonical native/script-qualified type key, size: integer,
//  alignment: power-of-two integer, flags: "0x" plus 16 hexadecimal digits}.
// The helper hashes immutable cache copies and checks original live type identities;
// it neither creates template instances nor registers a client. Only VM intrinsic
// references may omit this field. External native values/templates require it.
ANGELSCRIPTCODE_API ECacheAdmission SealModuleCacheNativeTypeLayouts(asIScriptEngine* ProducerEngine,
    TConstArrayView<FVerifiedCacheFileView> Files, FString& NativeTypeLayoutsJson, FString& Error);
#endif
ANGELSCRIPTCODE_API ECacheAdmission TryLoadModuleCacheSet(FDelegateHandle Client,
    const FVerifiedCacheSetView& Set, FCacheLoadResult& Result);
ANGELSCRIPTCODE_API ECacheAdmission TryRemoveModuleCacheSet(FDelegateHandle Client,
    FModuleCacheSetHandle Set, FCacheRemoveResult& Result);
                                                                                                     
                                                                                                          
struct FSourceRemoveResult
{
    FString Error;
    bool bSucceeded = false;
    bool bVmCommitted = false;
    TArray<FModuleGenerationId> RemovedGenerations;
};
// Synchronous GT writer action. Only exact source generations published through
// this transient client's compile surface are eligible. Busy changes no ownership
// or input and keeps callbacks alive; stale/foreign/duplicate identities are Invalid.
// Closing refuses new compilation/admission; pure removal of existing owned
// generations remains available to drain teardown before Unregister.
ANGELSCRIPTCODE_API ECacheAdmission TryRemoveSourceGenerations(FDelegateHandle Client,
    TConstArrayView<FModuleGenerationId> ExactGenerations, FSourceRemoveResult& Result);
                                                                                                        
                                                                                                       
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
                                                                                                     

                                                                                            
namespace AngelscriptCoreExtensionPoints
{
struct FTypeScope
{
    FString Namespace;
    TArray<FString> Dependencies;
};
// The provider resolves trusted source ownership once, before preprocessing.
// Returned snapshots are owned by candidate/live modules, never by VM pointers.
using FTypeScopeProvider = TFunction<bool(FStringView AbsolutePath, FTypeScope& Scope)>;
ANGELSCRIPTCODE_API FDelegateHandle RegisterTypeScope(FTypeScopeProvider Provider);
ANGELSCRIPTCODE_API ERemoveResult UnregisterTypeScope(FDelegateHandle Handle);
namespace Private
{
ANGELSCRIPTCODE_API TSharedPtr<const FTypeScope> ResolveTypeScope(FStringView AbsolutePath);
ANGELSCRIPTCODE_API void PrepareTypeScope(FAngelscriptModuleDesc& Module);
}
}
                                                                                          

                                                                                                   
namespace AngelscriptCoreExtensionPoints
{
enum class EDebugPolicyRegistration : uint8 { Registered, Conflict, TooLate };
ANGELSCRIPTCODE_API EDebugPolicyRegistration RegisterDebugStartPolicy(
    TFunction<bool()> DenyStart, FDelegateHandle& OutHandle);
ANGELSCRIPTCODE_API ERemoveResult UnregisterDebugStartPolicy(FDelegateHandle Handle);
}
                                                                                                 

                                                                                              
namespace AngelscriptCoreExtensionPoints
{
enum class EDebugAccessPhase : uint8 { Control, Read, Publish };
enum class EDebugAccessScope : uint8 { Unknown, Script, Native, Global };
enum class EDebugAccessOperation : uint8
{
    Diagnostics, RequestDebugDatabase, DebugDatabase, StartDebugging, StopDebugging,
    Pause, Continue, RequestCallStack, CallStack, ClearBreakpoints, SetBreakpoint,
    HasStopped, HasContinued, StepOver, StepIn, StepOut, EngineBreak,
    RequestVariables, Variables, RequestEvaluate, Evaluate, GoToDefinition,
    BreakOptions, RequestBreakFilters, BreakFilters, Disconnect,
    DebugDatabaseFinished, AssetDatabaseInit, AssetDatabase, AssetDatabaseFinished,
    FindAssets, DebugDatabaseSettings, PingAlive, DebugServerVersion,
    CreateBlueprint, ReplaceAssetDefinition, SetDataBreakpoints, ClearDataBreakpoints,
    StopPIE, Value, Member, Scope, Getter, Count
};
struct FDebugAccessView
{
    EDebugAccessOperation Operation = EDebugAccessOperation::Value;
    EDebugAccessPhase Phase = EDebugAccessPhase::Read;
    uint64 SessionId = 0;
    EDebugAccessScope Scope = EDebugAccessScope::Unknown;
    FModuleGenerationId ModuleGenerationId;
    FString SourceIdentity;
};
ANGELSCRIPTCODE_API FDelegateHandle RegisterDebugAccess(TFunction<bool(const FDebugAccessView&)> Allow);
ANGELSCRIPTCODE_API ERemoveResult UnregisterDebugAccess(FDelegateHandle Handle);
}
                                                                                            

                                                                                                          
namespace AngelscriptCoreExtensionPoints
{
enum class ECompileSurfaceAdmission : uint8 { Ready, Busy, Invalid, Unavailable };
struct FCompileSurfaceHandle { uint64 Id = 0; };
// Register before Runtime initialization. The shared client owner also owns H5 sets.
#if WITH_DEV_AUTOMATION_TESTS
struct FTransientScriptSection { FString Name; FString Code; };
ANGELSCRIPTCODE_API ECompileSurfaceAdmission CompileTransientModule(FDelegateHandle Client,
    FCompileSurfaceHandle Surface, FStringView ModuleName,
    TConstArrayView<FTransientScriptSection> Sections, FModuleGenerationId& Module, FString& Error);
#endif
ANGELSCRIPTCODE_API FDelegateHandle RegisterTransientCompileClient(FName FeatureId, TFunction<void()> OnClosing = {});
ANGELSCRIPTCODE_API ERemoveResult UnregisterTransientCompileClient(FDelegateHandle Client);
// GT only; release synchronously on the GT. The handle grants no native mutation permit.
// FullReload admits new source modules and restricted replacement within the same scope/client.
// SoftReloadOnly requires a generation owned by this client; cache-owned modules cannot be replaced.
// Initial and borrowed cache inputs reject; the supplemental action budget is 128 MiB.
ANGELSCRIPTCODE_API ECompileSurfaceAdmission AcquireTransientCompileSurface(FDelegateHandle Client,
    FCompileSurfaceHandle& Surface, FString& Error);
ANGELSCRIPTCODE_API ERemoveResult ReleaseTransientCompileSurface(FDelegateHandle Client,
    FCompileSurfaceHandle Surface);
// Without an active client action, compilation follows the original path.
// UE reflected class generation remains owned by the Manager compilation pipeline.
}
                                                                                                        

                                                                                                  
namespace AngelscriptCoreExtensionPoints
{
ANGELSCRIPTCODE_API UFunction* FindBlueprintEventByScriptName(UClass* Class, const FString& ScriptName);
}
                                                                                                
