// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "RuntimeServices/ModKitDebugAccess.h"

#include "FeatureMods/ModKitCompilationManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"

namespace
{
	using namespace AngelscriptCoreExtensionPoints;
	FDelegateHandle DebugAccessHandle;
	FDelegateHandle DebugAccessPreExitHandle;

	struct FDebugPolicy
	{
		TArray<FModKitDebugAccessRule> Rules;
		TArray<FString> GlobalOperations;
		TArray<FString> NativeOperations;
	};
	TSharedPtr<const FDebugPolicy, ESPMode::ThreadSafe> DebugAccessPolicy;
#if WITH_DEV_AUTOMATION_TESTS
	TWeakPtr<FDebugPolicy, ESPMode::ThreadSafe> FixturePolicy;
	bool bCaptureFixturePolicy = false;
#endif

	FString NormalizeSourceIdentity(FString Source)
	{
		Source.ReplaceInline(TEXT("\\"), TEXT("/"));
		FPaths::NormalizeFilename(Source);
		while (Source.StartsWith(TEXT("/"))) Source.RightChopInline(1);
		return Source;
	}

	const TCHAR* OperationToken(EDebugAccessOperation Operation)
	{
		switch (Operation)
		{
		case EDebugAccessOperation::StartDebugging: case EDebugAccessOperation::StopDebugging:
		case EDebugAccessOperation::Disconnect: case EDebugAccessOperation::StopPIE:
		case EDebugAccessOperation::BreakOptions: case EDebugAccessOperation::RequestBreakFilters:
		case EDebugAccessOperation::BreakFilters: case EDebugAccessOperation::PingAlive:
		case EDebugAccessOperation::DebugServerVersion: return TEXT("session");
		case EDebugAccessOperation::SetBreakpoint: case EDebugAccessOperation::ClearBreakpoints:
		case EDebugAccessOperation::SetDataBreakpoints: case EDebugAccessOperation::ClearDataBreakpoints:
			return TEXT("breakpoint");
		case EDebugAccessOperation::Pause: case EDebugAccessOperation::Continue:
		case EDebugAccessOperation::StepOver: case EDebugAccessOperation::StepIn:
		case EDebugAccessOperation::StepOut: case EDebugAccessOperation::EngineBreak:
		case EDebugAccessOperation::HasStopped: case EDebugAccessOperation::HasContinued:
			return TEXT("execution-control");
		case EDebugAccessOperation::RequestCallStack: case EDebugAccessOperation::CallStack:
			return TEXT("callstack");
		case EDebugAccessOperation::RequestVariables: case EDebugAccessOperation::Variables:
		case EDebugAccessOperation::Value: case EDebugAccessOperation::Member:
		case EDebugAccessOperation::Scope: case EDebugAccessOperation::Getter:
			return TEXT("variables");
		case EDebugAccessOperation::RequestEvaluate: case EDebugAccessOperation::Evaluate:
			return TEXT("evaluation");
		case EDebugAccessOperation::GoToDefinition: return TEXT("source");
		case EDebugAccessOperation::RequestDebugDatabase: case EDebugAccessOperation::DebugDatabase:
		case EDebugAccessOperation::DebugDatabaseFinished: case EDebugAccessOperation::DebugDatabaseSettings:
			return TEXT("debug-database");
		default: return nullptr;
		}
	}

	bool Allows(const FDebugPolicy& Snapshot, const FDebugAccessView& View)
	{
		const TCHAR* Token = OperationToken(View.Operation);
		if (!Token || !View.SessionId || (View.Phase != EDebugAccessPhase::Control
			&& View.Phase != EDebugAccessPhase::Read && View.Phase != EDebugAccessPhase::Publish)) return false;
		if (View.Scope == EDebugAccessScope::Global) return Snapshot.GlobalOperations.Contains(Token);
		if (View.Scope == EDebugAccessScope::Native) return Snapshot.NativeOperations.Contains(Token);
		if (View.Scope != EDebugAccessScope::Script || !View.ModuleGenerationId.Generation
			|| View.ModuleGenerationId.ModuleName.IsEmpty() || View.SourceIdentity.IsEmpty()) return false;
		const FString Source = NormalizeSourceIdentity(View.SourceIdentity);
		for (const FModKitDebugAccessRule& Rule : Snapshot.Rules)
			if (Rule.ModuleName == View.ModuleGenerationId.ModuleName && Rule.SourceIdentity == Source
				&& Rule.AllowedOperations.Contains(Token)) return true;
		return false;
	}
}

void FModKitDebugAccess::Startup()
{
	check(IsInGameThread());
	if (DebugAccessHandle.IsValid()) return;
	const UModKitDebugAccessSettings* Settings = GetDefault<UModKitDebugAccessSettings>();
	auto Snapshot = MakeShared<FDebugPolicy, ESPMode::ThreadSafe>();
#if WITH_DEV_AUTOMATION_TESTS
	if (bCaptureFixturePolicy) FixturePolicy = Snapshot;
#endif
	Snapshot->Rules = Settings->DebugAccessRules;
	Snapshot->GlobalOperations = Settings->GlobalAllowedOperations;
	Snapshot->NativeOperations = Settings->NativeAllowedOperations;
	for (FModKitDebugAccessRule& Rule : Snapshot->Rules)
		Rule.SourceIdentity = NormalizeSourceIdentity(Rule.SourceIdentity);
	DebugAccessPolicy = Snapshot;
	DebugAccessHandle = RegisterDebugAccess([Snapshot](const FDebugAccessView& View) { return Allows(*Snapshot, View); });
	if (!DebugAccessHandle.IsValid())
		UE_LOG(LogAngelscriptModKit, Fatal, TEXT("ModKit debug access provider unavailable"));
	DebugAccessPreExitHandle = FCoreDelegates::OnPreExit.AddStatic(&FModKitDebugAccess::Shutdown);
}

void FModKitDebugAccess::Shutdown()
{
	check(IsInGameThread());
	if (DebugAccessHandle.IsValid())
	{
		const ERemoveResult Result = UnregisterDebugAccess(DebugAccessHandle);
		if (Result != ERemoveResult::Removed) return;
		DebugAccessHandle.Reset();
	}
	if (DebugAccessPreExitHandle.IsValid())
	{
		FCoreDelegates::OnPreExit.Remove(DebugAccessPreExitHandle);
		DebugAccessPreExitHandle.Reset();
	}
	DebugAccessPolicy.Reset();
}

bool FModKitDebugAccess::IsAllowed(const FDebugAccessView& View)
{
	return DebugAccessPolicy.IsValid() && Allows(*DebugAccessPolicy, View);
}

bool FModKitDebugAccess::IsRetained()
{
	return DebugAccessHandle.IsValid();
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

bool FModKitDebugAccess::ConfigureForTest(const TArray<FModKitDebugAccessRule>& Rules,
	const TArray<FString>& GlobalOperations, const TArray<FString>& NativeOperations)
{
	Shutdown();
	if (IsRetained()) return false;
	auto* Settings = GetMutableDefault<UModKitDebugAccessSettings>();
	const auto SavedRules = Settings->DebugAccessRules;
	const auto SavedGlobal = Settings->GlobalAllowedOperations;
	const auto SavedNative = Settings->NativeAllowedOperations;
	Settings->DebugAccessRules = Rules;
	Settings->GlobalAllowedOperations = GlobalOperations;
	Settings->NativeAllowedOperations = NativeOperations;
	bCaptureFixturePolicy = true;
	Startup();
	bCaptureFixturePolicy = false;
	Settings->DebugAccessRules = SavedRules;
	Settings->GlobalAllowedOperations = SavedGlobal;
	Settings->NativeAllowedOperations = SavedNative;
	return IsRetained();
}

bool FModKitDebugAccess::DenyGlobalOperationForTest(const FString& Operation)
{
	check(IsInGameThread());
	auto Snapshot = FixturePolicy.Pin();
	return Snapshot.IsValid() && Snapshot->GlobalOperations.Remove(Operation) != 0;
}

AngelscriptCoreExtensionPoints::ERemoveResult FModKitDebugAccess::RevokeForTest()
{
	const auto Result = UnregisterDebugAccess(DebugAccessHandle);
	if (Result == ERemoveResult::Removed)
	{
		DebugAccessHandle.Reset();
		if (DebugAccessPreExitHandle.IsValid()) FCoreDelegates::OnPreExit.Remove(DebugAccessPreExitHandle);
		DebugAccessPreExitHandle.Reset();
		DebugAccessPolicy.Reset();
		FixturePolicy.Reset();
	}
	return Result;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitDebugPolicyTest, "AngelscriptModKit.DebugAccess.Policy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FModKitDebugPolicyTest::RunTest(const FString&)
{
	FDebugPolicy Snapshot;
	FModKitDebugAccessRule Rule;
	Rule.ModuleName = TEXT("FeatureMods.Sample.Script");
	Rule.SourceIdentity = TEXT("FeatureMods/Sample/Script.as");
	Rule.AllowedOperations = { TEXT("variables") };
	Snapshot.Rules.Add(Rule);
	FDebugAccessView View;
	View.Operation = EDebugAccessOperation::Variables;
	View.Phase = EDebugAccessPhase::Read;
	View.SessionId = 1;
	View.Scope = EDebugAccessScope::Script;
	View.ModuleGenerationId = { Rule.ModuleName, 17 };
	View.SourceIdentity = Rule.SourceIdentity;
	TestTrue(TEXT("Exact script identity is allowed"), Allows(Snapshot, View));
	View.ModuleGenerationId.Generation = 0;
	TestFalse(TEXT("Missing generation is denied"), Allows(Snapshot, View));
	View.ModuleGenerationId.Generation = 17;
	View.SourceIdentity = TEXT("FeatureMods/Other/Script.as");
	TestFalse(TEXT("Other source is denied"), Allows(Snapshot, View));
	View.Scope = EDebugAccessScope::Global;
	TestFalse(TEXT("Script permission does not grant global access"), Allows(Snapshot, View));
	Snapshot.GlobalOperations.Add(TEXT("variables"));
	TestTrue(TEXT("Explicit global permission is honored"), Allows(Snapshot, View));
	View.Scope = EDebugAccessScope::Native;
	TestFalse(TEXT("Global permission does not grant native access"), Allows(Snapshot, View));
	View.Operation = EDebugAccessOperation::Count;
	TestFalse(TEXT("Unknown operation is denied"), Allows(Snapshot, View));
	View.Operation = EDebugAccessOperation::Variables;
	View.SessionId = 0;
	TestFalse(TEXT("Missing session is denied"), Allows(Snapshot, View));
	View.SessionId = 1;
	View.Scope = EDebugAccessScope::Script;
	View.SourceIdentity = Rule.SourceIdentity;
	for (const auto Phase : {EDebugAccessPhase::Control, EDebugAccessPhase::Read, EDebugAccessPhase::Publish})
	{
		View.Phase = Phase;
		for (int32 Index = 0; Index < int32(EDebugAccessOperation::Count); ++Index)
		{
			View.Operation = EDebugAccessOperation(Index);
			const TCHAR* Token = OperationToken(View.Operation);
			TestEqual(TEXT("Every protocol operation honors its exact configured category in every phase"),
				Allows(Snapshot, View), Token && FString(Token) == TEXT("variables"));
		}
	}
	View.Operation = EDebugAccessOperation::Getter;
	View.ModuleGenerationId.ModuleName = TEXT("FeatureMods.Other.Script");
	TestFalse(TEXT("A source permission never authorizes another module"), Allows(Snapshot, View));
	View.ModuleGenerationId.ModuleName = Rule.ModuleName;
	View.SourceIdentity = TEXT("FeatureMods/Sample/Other.as");
	TestFalse(TEXT("A module permission never authorizes another source or its getter"), Allows(Snapshot, View));
	View.Scope = EDebugAccessScope::Unknown;
	TestFalse(TEXT("Unknown identity is denied even with an otherwise permitted operation"), Allows(Snapshot, View));
	TestTrue(TEXT("The default production ACL provider remains installed"), FModKitDebugAccess::IsRetained());
	TestFalse(TEXT("The production provider rejects an unidentified protocol request"), FModKitDebugAccess::IsAllowed(View));
	return true;
}
#endif
