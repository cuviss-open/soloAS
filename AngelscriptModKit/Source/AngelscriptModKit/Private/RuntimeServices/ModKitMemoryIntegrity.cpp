// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
                                                                                 

#include "RuntimeServices/ModKitMemoryIntegrity.h"

#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "Core/ModKitSettings.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "RuntimeServices/ModKitDebugAccess.h"
#endif

namespace
{
	FTSTicker::FDelegateHandle GIntegrityHandle;
	FModKitRuntimeHashProvider GIntegrityProvider;
	bool bIntegrityBreached = false;
	bool bBaselineValid = false;
	TArray<uint8> BaselineHash;
	double LastCheckSeconds = 0.0;
	// The forced (validation) path recomputes at most once per second: the
	// disk-.text hash of a monolithic binary is expensive, and the ticker
	// calls every frame.
	double LastComputeSeconds = 0.0;
	bool bLastCurrentValid = false;
	TArray<uint8> LastCurrentHash;

	void RunIntegrityCheck()
	{
		const UModKitSettings* Settings = UModKitSettings::Get();
		if (!Settings->bMemoryIntegrityCheck)
		{
			return;
		}

		const double Now = FPlatformTime::Seconds();
		if (!Settings->bForceIntegrityCheckInDev)
		{
			// Interval-based sampling (S5): the deterministic acceptance path
			// uses bForceIntegrityCheckInDev; the interval form is the runtime
			// default and keeps the per-tick cost at zero between checks.
			const double Interval = FMath::Max(Settings->MemoryIntegrityIntervalSec, 0.1f);
			if (LastCheckSeconds > 0.0 && Now - LastCheckSeconds < Interval)
			{
				return;
			}
		}
		LastCheckSeconds = Now;

		// The baseline comes from the unwrapped default provider so a tamper
		// simulation on the check side cannot poison the reference value.
		if (!bBaselineValid)
		{
			bBaselineValid = FModKitRuntimeHash::ComputeDefault(BaselineHash);
			if (!bBaselineValid)
			{
				return;
			}
		}

		if (!bLastCurrentValid || Now - LastComputeSeconds >= 1.0)
		{
			TArray<uint8> CurrentHash;
			const bool bComputed = GIntegrityProvider
				? GIntegrityProvider(CurrentHash)
				: FModKitRuntimeHash::ComputeDefault(CurrentHash);
			LastComputeSeconds = Now;
			bLastCurrentValid = bComputed;
			if (bComputed)
			{
				LastCurrentHash = MoveTemp(CurrentHash);
			}
		}
		if (!bLastCurrentValid)
		{
			return;
		}

		if (!bIntegrityBreached && LastCurrentHash != BaselineHash)
		{
			bIntegrityBreached = true;
			UE_LOG(LogAngelscriptModKit, Warning,
				TEXT("MODKIT_MEMORY_INTEGRITY_VIOLATION keyId=%s"), *Settings->ScriptCacheKeyId);
		}
	}
}

void FModKitMemoryIntegrity::Startup()
{
	if (GIntegrityHandle.IsValid())
	{
		return;
	}
	const UModKitSettings* Settings = UModKitSettings::Get();
	if (!Settings->bMemoryIntegrityCheck)
	{
		return;
	}
	GIntegrityHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		RunIntegrityCheck();
		return true;
	}));
}

void FModKitMemoryIntegrity::Shutdown()
{
	if (GIntegrityHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GIntegrityHandle);
		GIntegrityHandle.Reset();
	}
	GIntegrityProvider = nullptr;
	bIntegrityBreached = false;
	bBaselineValid = false;
	BaselineHash.Reset();
	LastCheckSeconds = 0.0;
	LastComputeSeconds = 0.0;
	bLastCurrentValid = false;
	LastCurrentHash.Reset();
}

bool FModKitMemoryIntegrity::IsBreached()
{
	return bIntegrityBreached;
}

void FModKitMemoryIntegrity::SetIntegrityProvider(FModKitRuntimeHashProvider Provider)
{
	GIntegrityProvider = MoveTemp(Provider);
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitRuntimeServicesPolicySnapshotTest,
	"AngelscriptModKit.RuntimeServices.PolicySnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModKitRuntimeServicesPolicySnapshotTest::RunTest(const FString& Parameters)
{
	const UModKitSettings* Snapshot = UModKitSettings::Get();
	UModKitSettings* ConfigDefaults = GetMutableDefault<UModKitSettings>();
	TestTrue(TEXT("runtime policy snapshot is distinct from config defaults"), Snapshot != ConfigDefaults);
	if (Snapshot == nullptr || ConfigDefaults == nullptr)
	{
		return false;
	}

	TestEqual(TEXT("default timeout budget is five seconds"), ConfigDefaults->ModScriptExecutionTimeoutSec, 5.0f);
	TestTrue(TEXT("default timeout disable is enabled"), ConfigDefaults->bDisableModOnTimeout);
	TestEqual(TEXT("default timeout threshold is three"), ConfigDefaults->MaxConsecutiveTimeoutsBeforeDisable, 3);
	TestFalse(TEXT("development timeout override is disabled by default"), ConfigDefaults->bForceModKitTimeoutInDev);
	TestFalse(TEXT("memory integrity is disabled by default"), ConfigDefaults->bMemoryIntegrityCheck);
	TestEqual(TEXT("default integrity interval is ten seconds"), ConfigDefaults->MemoryIntegrityIntervalSec, 10.0f);
	TestFalse(TEXT("development integrity override is disabled by default"), ConfigDefaults->bForceIntegrityCheckInDev);
	TestEqual(TEXT("startup snapshot captures default timeout budget"), Snapshot->ModScriptExecutionTimeoutSec, ConfigDefaults->ModScriptExecutionTimeoutSec);
	TestEqual(TEXT("startup snapshot captures default timeout disable switch"), Snapshot->bDisableModOnTimeout, ConfigDefaults->bDisableModOnTimeout);
	TestEqual(TEXT("startup snapshot captures default timeout threshold"), Snapshot->MaxConsecutiveTimeoutsBeforeDisable, ConfigDefaults->MaxConsecutiveTimeoutsBeforeDisable);
	TestEqual(TEXT("startup snapshot captures default development timeout switch"), Snapshot->bForceModKitTimeoutInDev, ConfigDefaults->bForceModKitTimeoutInDev);
	TestEqual(TEXT("startup snapshot captures default integrity enable switch"), Snapshot->bMemoryIntegrityCheck, ConfigDefaults->bMemoryIntegrityCheck);
	TestEqual(TEXT("startup snapshot captures default integrity interval"), Snapshot->MemoryIntegrityIntervalSec, ConfigDefaults->MemoryIntegrityIntervalSec);
	TestEqual(TEXT("startup snapshot captures default development integrity switch"), Snapshot->bForceIntegrityCheckInDev, ConfigDefaults->bForceIntegrityCheckInDev);

	const float TimeoutBudget = Snapshot->ModScriptExecutionTimeoutSec;
	const bool bDisableOnTimeout = Snapshot->bDisableModOnTimeout;
	const int32 TimeoutThreshold = Snapshot->MaxConsecutiveTimeoutsBeforeDisable;
	const bool bForceTimeoutInDev = Snapshot->bForceModKitTimeoutInDev;
	const bool bCheckIntegrity = Snapshot->bMemoryIntegrityCheck;
	const float IntegrityInterval = Snapshot->MemoryIntegrityIntervalSec;
	const bool bForceIntegrityInDev = Snapshot->bForceIntegrityCheckInDev;
	const float OriginalTimeoutBudget = ConfigDefaults->ModScriptExecutionTimeoutSec;
	const bool bOriginalDisableOnTimeout = ConfigDefaults->bDisableModOnTimeout;
	const int32 OriginalTimeoutThreshold = ConfigDefaults->MaxConsecutiveTimeoutsBeforeDisable;
	const bool bOriginalForceTimeoutInDev = ConfigDefaults->bForceModKitTimeoutInDev;
	const bool bOriginalCheckIntegrity = ConfigDefaults->bMemoryIntegrityCheck;
	const float OriginalIntegrityInterval = ConfigDefaults->MemoryIntegrityIntervalSec;
	const bool bOriginalForceIntegrityInDev = ConfigDefaults->bForceIntegrityCheckInDev;

	ConfigDefaults->ModScriptExecutionTimeoutSec = TimeoutBudget + 1.0f;
	ConfigDefaults->bDisableModOnTimeout = !bDisableOnTimeout;
	ConfigDefaults->MaxConsecutiveTimeoutsBeforeDisable = TimeoutThreshold + 1;
	ConfigDefaults->bForceModKitTimeoutInDev = !bForceTimeoutInDev;
	ConfigDefaults->bMemoryIntegrityCheck = !bCheckIntegrity;
	ConfigDefaults->MemoryIntegrityIntervalSec = IntegrityInterval + 1.0f;
	ConfigDefaults->bForceIntegrityCheckInDev = !bForceIntegrityInDev;

	TestTrue(TEXT("runtime policy object is retained"), UModKitSettings::Get() == Snapshot);
	TestEqual(TEXT("timeout budget takes effect only after restart"), UModKitSettings::Get()->ModScriptExecutionTimeoutSec, TimeoutBudget);
	TestEqual(TEXT("timeout disable switch takes effect only after restart"), UModKitSettings::Get()->bDisableModOnTimeout, bDisableOnTimeout);
	TestEqual(TEXT("timeout threshold takes effect only after restart"), UModKitSettings::Get()->MaxConsecutiveTimeoutsBeforeDisable, TimeoutThreshold);
	TestEqual(TEXT("development timeout switch takes effect only after restart"), UModKitSettings::Get()->bForceModKitTimeoutInDev, bForceTimeoutInDev);
	TestEqual(TEXT("integrity enable switch takes effect only after restart"), UModKitSettings::Get()->bMemoryIntegrityCheck, bCheckIntegrity);
	TestEqual(TEXT("integrity interval takes effect only after restart"), UModKitSettings::Get()->MemoryIntegrityIntervalSec, IntegrityInterval);
	TestEqual(TEXT("development integrity switch takes effect only after restart"), UModKitSettings::Get()->bForceIntegrityCheckInDev, bForceIntegrityInDev);

	ConfigDefaults->ModScriptExecutionTimeoutSec = OriginalTimeoutBudget;
	ConfigDefaults->bDisableModOnTimeout = bOriginalDisableOnTimeout;
	ConfigDefaults->MaxConsecutiveTimeoutsBeforeDisable = OriginalTimeoutThreshold;
	ConfigDefaults->bForceModKitTimeoutInDev = bOriginalForceTimeoutInDev;
	ConfigDefaults->bMemoryIntegrityCheck = bOriginalCheckIntegrity;
	ConfigDefaults->MemoryIntegrityIntervalSec = OriginalIntegrityInterval;
	ConfigDefaults->bForceIntegrityCheckInDev = bOriginalForceIntegrityInDev;

	AngelscriptCoreExtensionPoints::FDebugAccessView UnknownOperation;
	TestFalse(TEXT("unknown debug operations are denied"), FModKitDebugAccess::IsAllowed(UnknownOperation));
	return true;
}
#endif
