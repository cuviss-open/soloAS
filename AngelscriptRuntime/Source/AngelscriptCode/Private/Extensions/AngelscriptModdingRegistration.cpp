// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.

// Cuviss-owned registration support for the Modding track registration points.

#include "Extensions/AngelscriptModdingRegistration.h"
#include "Extensions/AngelscriptModdingInternal.h"

namespace
{
	TMap<FDelegateHandle, FAngelscriptModdingCacheGenerationFilter::FScriptUnitExcludeHandler> GGameScriptUnitFilters;
	TMap<FDelegateHandle, FAngelscriptModdingCacheGenerationFilter::FScriptUnitExcludeHandler> GProjectCacheScriptUnitFilters;
	TArray<FAngelscriptCacheCompileSurfaceGate::FPreserveHandler> GCacheCompileSurfaceGateHandlers;
	TArray<FAngelscriptModdingCacheGenerationFilter::FExcludeHandler> GCacheGenerationFilterHandlers;
	TArray<FAngelscriptModdingCacheGenerationFilter::FExcludeHandler> GJitGenerationFilterHandlers;
	TArray<FAngelscriptModdingDebugServerStartGate::FSuppressHandler> GDebugServerStartGateHandlers;
	TMap<FDelegateHandle, FAngelscriptModdingDebugMessageAccessGate::FAccessHandler> GDebugMessageAccessHandlers;
	TArray<FAngelscriptModdingLeafDirectionAudit::FViolationHandler> GLeafDirectionAuditHandlers;
	TMap<FDelegateHandle, FAngelscriptModdingApiAllowlistAudit::FAuditHandler> GApiAllowlistAuditHandlers;
	TMap<FDelegateHandle, FAngelscriptModdingScriptExecutionBudget::FQueryBudget> GScriptExecutionBudgetHandlers;
	TMap<FDelegateHandle, FAngelscriptModdingScriptExecutionBudget::FTimeoutHandler> GScriptExecutionTimeoutHandlers;
	FAngelscriptModdingPrecompiledDataTransform::FPolicyHandler GPrecompiledDataTransformPolicyHandler;
	FAngelscriptModdingPrecompiledDataTransform::FBytesHandler GPrecompiledDataTransformBytesHandler;
	FDelegateHandle GPrecompiledDataTransformHandle;
	TMap<FDelegateHandle, FAngelscriptModdingMemoryIntegrity::FCheckHandler> GMemoryIntegrityHandlers;
	FAngelscriptModdingStaticJITGenerationGate::FPolicyHandler GStaticJITGenerationPolicyHandler;
	FAngelscriptModdingStaticJITGenerationGate::FCompleteHandler GStaticJITGenerationCompleteHandler;
	FDelegateHandle GStaticJITGenerationHandle;
	bool GStaticJITGenerationBatchSkipped = false;
}

void FAngelscriptCacheCompileSurfaceGate::Register(FPreserveHandler Handler)
{
	GCacheCompileSurfaceGateHandlers.Add(MoveTemp(Handler));
}

bool FAngelscriptCacheCompileSurfaceGateRuntime::AnyPreserve()
{
	for (const FPreserveHandler& Handler : GCacheCompileSurfaceGateHandlers)
	{
		if (Handler && Handler())
		{
			return true;
		}
	}
	return false;
}

void FAngelscriptModdingCacheGenerationFilter::Register(FExcludeHandler CacheExcludeHandler, FExcludeHandler JitExcludeHandler)
{
	GCacheGenerationFilterHandlers.Add(MoveTemp(CacheExcludeHandler));
	GJitGenerationFilterHandlers.Add(MoveTemp(JitExcludeHandler));
}

bool FAngelscriptModdingCacheGenerationFilterRuntime::AnyExclude(const FString& ModuleName, const TArray<FString>& RelativeFilenames)
{
	for (const FExcludeHandler& Handler : GCacheGenerationFilterHandlers)
	{
		if (Handler && Handler(ModuleName, RelativeFilenames))
		{
			return true;
		}
	}
	return false;
}

bool FAngelscriptModdingCacheGenerationFilterRuntime::AnyJitExclude(const FString& ModuleName, const TArray<FString>& RelativeFilenames)
{
	for (const FExcludeHandler& Handler : GJitGenerationFilterHandlers)
	{
		if (Handler && Handler(ModuleName, RelativeFilenames))
		{
			return true;
		}
	}
	return false;
}

FDelegateHandle FAngelscriptModdingCacheGenerationFilter::RegisterScriptUnitFilters(
	FScriptUnitExcludeHandler GameExcludeHandler, FScriptUnitExcludeHandler ProjectCacheExcludeHandler)
{
	FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
	GGameScriptUnitFilters.Add(Handle, MoveTemp(GameExcludeHandler));
	GProjectCacheScriptUnitFilters.Add(Handle, MoveTemp(ProjectCacheExcludeHandler));
	return Handle;
}

void FAngelscriptModdingCacheGenerationFilter::UnregisterScriptUnitFilters(FDelegateHandle Handle)
{
	GGameScriptUnitFilters.Remove(Handle);
	GProjectCacheScriptUnitFilters.Remove(Handle);
}

bool FAngelscriptModdingCacheGenerationFilterRuntime::IsGameCacheExcludedScriptUnit(const FString& ScriptUnitId)
{
	for (const auto& Entry : GGameScriptUnitFilters)
		if (Entry.Value && Entry.Value(ScriptUnitId)) return true;
	return false;
}

bool FAngelscriptModdingCacheGenerationFilterRuntime::IsGameCacheExcludedProjectScriptUnit(const FString& ScriptUnitId)
{
	for (const auto& Entry : GProjectCacheScriptUnitFilters)
		if (Entry.Value && Entry.Value(ScriptUnitId)) return true;
	return false;
}

void FAngelscriptModdingDebugServerStartGate::Register(FSuppressHandler Handler)
{
	GDebugServerStartGateHandlers.Add(MoveTemp(Handler));
}

bool FAngelscriptModdingDebugServerStartGateRuntime::AnySuppress()
{
	for (const FSuppressHandler& Handler : GDebugServerStartGateHandlers)
	{
		if (Handler && Handler())
		{
			return true;
		}
	}
	return false;
}

                                                                                                 
FDelegateHandle FAngelscriptModdingDebugMessageAccessGate::Register(FAccessHandler Handler)
{
	FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
	GDebugMessageAccessHandlers.Add(Handle, MoveTemp(Handler));
	return Handle;
}

void FAngelscriptModdingDebugMessageAccessGate::Unregister(FDelegateHandle Handle)
{
	GDebugMessageAccessHandlers.Remove(Handle);
}

bool FAngelscriptModdingDebugMessageAccessGateRuntime::HasHandlers()
{
	return GDebugMessageAccessHandlers.Num() != 0;
}

bool FAngelscriptModdingDebugMessageAccessGate::IsAllowed(const FMessageContext& Context)
{
	if (!FAngelscriptModdingDebugMessageAccessGateRuntime::HasHandlers())
	{
		return true;
	}
	for (const TPair<FDelegateHandle, FAccessHandler>& Pair : GDebugMessageAccessHandlers)
	{
		if (!Pair.Value || !Pair.Value(Context))
		{
			return false;
		}
	}
	return true;
}

const TCHAR* FAngelscriptModdingDebugMessageAccessGate::GetOperationToken(EOperation Operation)
{
	switch (Operation)
	{
	case EOperation::Session: return TEXT("session");
	case EOperation::Breakpoint: return TEXT("breakpoint");
	case EOperation::ExecutionControl: return TEXT("execution-control");
	case EOperation::CallStack: return TEXT("callstack");
	case EOperation::Variables: return TEXT("variables");
	case EOperation::Evaluation: return TEXT("evaluation");
	case EOperation::Source: return TEXT("source");
	case EOperation::DebugDatabase: return TEXT("debug-database");
	case EOperation::AssetDatabase: return TEXT("asset-database");
	default: return TEXT("unknown");
	}
}
                                                                                               

void FAngelscriptModdingLeafDirectionAudit::Register(FViolationHandler Handler)
{
	GLeafDirectionAuditHandlers.Add(MoveTemp(Handler));
}

bool FAngelscriptModdingLeafDirectionAuditRuntime::HasHandlers()
{
	return GLeafDirectionAuditHandlers.Num() != 0;
}

bool FAngelscriptModdingLeafDirectionAuditRuntime::AnyViolation(const FString& ModuleName, const TArray<FString>& DependencyModuleNames, FString& OutError)
{
	for (const FViolationHandler& Handler : GLeafDirectionAuditHandlers)
	{
		if (Handler && Handler(ModuleName, DependencyModuleNames, OutError))
		{
			return true;
		}
	}
	return false;
}

FDelegateHandle FAngelscriptModdingApiAllowlistAudit::Register(FAuditHandler Handler)
{
	FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
	GApiAllowlistAuditHandlers.Add(Handle, MoveTemp(Handler));
	return Handle;
}

void FAngelscriptModdingApiAllowlistAudit::Unregister(FDelegateHandle Handle)
{
	GApiAllowlistAuditHandlers.Remove(Handle);
}

bool FAngelscriptModdingApiAllowlistAuditRuntime::HasHandlers()
{
	return GApiAllowlistAuditHandlers.Num() != 0;
}

bool FAngelscriptModdingApiAllowlistAuditRuntime::AnyViolation(const FString& ModuleName, const FModApiUsage& Usage, FString& OutError)
{
	for (const auto& Pair : GApiAllowlistAuditHandlers)
	{
		const FAuditHandler& Handler = Pair.Value;
		if (Handler && Handler(ModuleName, Usage, OutError))
		{
			return true;
		}
	}
	return false;
}

FDelegateHandle FAngelscriptModdingScriptExecutionBudget::Register(FQueryBudget Handler)
{
	FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
	GScriptExecutionBudgetHandlers.Add(Handle, MoveTemp(Handler));
	return Handle;
}

void FAngelscriptModdingScriptExecutionBudget::Unregister(FDelegateHandle Handle)
{
	GScriptExecutionBudgetHandlers.Remove(Handle);
}

bool FAngelscriptModdingScriptExecutionBudgetRuntime::HasHandlers()
{
	return GScriptExecutionBudgetHandlers.Num() != 0;
}

bool FAngelscriptModdingScriptExecutionBudgetRuntime::AnyBudget(float& OutBudgetSeconds, const asIScriptContext& Context)
{
	for (const auto& Pair : GScriptExecutionBudgetHandlers)
	{
		const FQueryBudget& Handler = Pair.Value;
		if (Handler)
		{
			const float Budget = Handler(Context);
			if (Budget > 0.0f)
			{
				OutBudgetSeconds = Budget;
				return true;
			}
		}
	}
	return false;
}

FDelegateHandle FAngelscriptModdingScriptExecutionBudget::RegisterTimeout(FTimeoutHandler Handler)
{
	FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
	GScriptExecutionTimeoutHandlers.Add(Handle, MoveTemp(Handler));
	return Handle;
}

void FAngelscriptModdingScriptExecutionBudget::UnregisterTimeout(FDelegateHandle Handle)
{
	GScriptExecutionTimeoutHandlers.Remove(Handle);
}

void FAngelscriptModdingScriptExecutionBudgetRuntime::NotifyTimeout(const FString& ModuleName, double ElapsedSeconds)
{
	for (const auto& Pair : GScriptExecutionTimeoutHandlers)
	{
		const FTimeoutHandler& Handler = Pair.Value;
		if (Handler)
		{
			Handler(ModuleName, ElapsedSeconds);
		}
	}
}

FDelegateHandle FAngelscriptModdingPrecompiledDataTransform::Register(FPolicyHandler PolicyHandler, FBytesHandler BytesHandler)
{
	if (GPrecompiledDataTransformHandle.IsValid())
	{
		Unregister(GPrecompiledDataTransformHandle);
	}
	GPrecompiledDataTransformPolicyHandler = MoveTemp(PolicyHandler);
	GPrecompiledDataTransformBytesHandler = MoveTemp(BytesHandler);
	GPrecompiledDataTransformHandle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
	return GPrecompiledDataTransformHandle;
}

void FAngelscriptModdingPrecompiledDataTransform::Unregister(FDelegateHandle Handle)
{
	if (Handle != GPrecompiledDataTransformHandle)
	{
		return;
	}
	GPrecompiledDataTransformPolicyHandler = nullptr;
	GPrecompiledDataTransformBytesHandler = nullptr;
	GPrecompiledDataTransformHandle.Reset();
}

bool FAngelscriptModdingPrecompiledDataTransformRuntime::HasHandlers()
{
	return GPrecompiledDataTransformHandle.IsValid() && GPrecompiledDataTransformPolicyHandler != nullptr;
}

bool FAngelscriptModdingPrecompiledDataTransformRuntime::GetPolicy(EDirection Direction, FPolicy& OutPolicy)
{
	if (!HasHandlers())
	{
		return false;
	}
	return GPrecompiledDataTransformPolicyHandler(Direction, OutPolicy);
}

bool FAngelscriptModdingPrecompiledDataTransformRuntime::TransformBytes(EDirection Direction, FPolicy& Policy, TArray<uint8>& Bytes)
{
	if (!HasHandlers() || !GPrecompiledDataTransformBytesHandler)
	{
		return true;
	}
	return GPrecompiledDataTransformBytesHandler(Direction, Policy, Bytes);
}

FDelegateHandle FAngelscriptModdingMemoryIntegrity::Register(FCheckHandler Handler)
{
	FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
	GMemoryIntegrityHandlers.Add(Handle, MoveTemp(Handler));
	return Handle;
}

void FAngelscriptModdingMemoryIntegrity::Unregister(FDelegateHandle Handle)
{
	GMemoryIntegrityHandlers.Remove(Handle);
}

bool FAngelscriptModdingMemoryIntegrityRuntime::HasHandlers()
{
	return GMemoryIntegrityHandlers.Num() != 0;
}

void FAngelscriptModdingMemoryIntegrityRuntime::RunChecks()
{
	for (const auto& Pair : GMemoryIntegrityHandlers)
	{
		Pair.Value();
	}
}

FDelegateHandle FAngelscriptModdingStaticJITGenerationGate::Register(FPolicyHandler PolicyHandler, FCompleteHandler CompleteHandler)
{
	if (GStaticJITGenerationHandle.IsValid())
	{
		Unregister(GStaticJITGenerationHandle);
	}
	GStaticJITGenerationPolicyHandler = MoveTemp(PolicyHandler);
	GStaticJITGenerationCompleteHandler = MoveTemp(CompleteHandler);
	GStaticJITGenerationHandle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
	return GStaticJITGenerationHandle;
}

void FAngelscriptModdingStaticJITGenerationGate::Unregister(FDelegateHandle Handle)
{
	if (Handle != GStaticJITGenerationHandle)
	{
		return;
	}
	GStaticJITGenerationPolicyHandler = nullptr;
	GStaticJITGenerationCompleteHandler = nullptr;
	GStaticJITGenerationHandle.Reset();
}

bool FAngelscriptModdingStaticJITGenerationGateRuntime::HasHandlers()
{
	return GStaticJITGenerationHandle.IsValid() && GStaticJITGenerationPolicyHandler != nullptr;
}

bool FAngelscriptModdingStaticJITGenerationGateRuntime::GetPolicy(
	const TArray<FString>& ModuleNames,
	const TArray<FString>& RelativeFilenames,
	int32 EngineBuildIdentifier,
	FPolicy& OutPolicy)
{
	if (!HasHandlers())
	{
		return false;
	}
	return GStaticJITGenerationPolicyHandler(ModuleNames, RelativeFilenames, EngineBuildIdentifier, OutPolicy);
}

void FAngelscriptModdingStaticJITGenerationGateRuntime::NotifyComplete(
	const FString& ManifestPath,
	const TArray<FString>& OutputFilenames,
	const TArray<FString>& CoveredModuleNames,
	const TArray<FString>& CoveredFunctionIdentities)
{
	if (!HasHandlers() || !GStaticJITGenerationCompleteHandler)
	{
		return;
	}
	GStaticJITGenerationCompleteHandler(ManifestPath, OutputFilenames, CoveredModuleNames, CoveredFunctionIdentities);
}

void FAngelscriptModdingStaticJITGenerationGateRuntime::MarkBatchSkippedThisRun()
{
	GStaticJITGenerationBatchSkipped = true;
}

bool FAngelscriptModdingStaticJITGenerationGateRuntime::ConsumeBatchSkippedThisRun()
{
	const bool bSkipped = GStaticJITGenerationBatchSkipped;
	GStaticJITGenerationBatchSkipped = false;
	return bSkipped;
}
