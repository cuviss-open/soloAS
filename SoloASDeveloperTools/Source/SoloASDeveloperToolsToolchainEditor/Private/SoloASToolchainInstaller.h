// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

namespace SoloASToolchain
{
struct FInstallStep
{
    FString Name;
    FString Script;
};
struct FInstallResult
{
    bool bSuccess = false;
    bool bCancelled = false;
    FString Error;
};
// The production and mock backends consume the same ordered, printable plan.
TArray<FInstallStep> InstallPlan(const FString& Root);
TArray<FInstallStep> UninstallPlan(const FString& Root);
FInstallResult RunInstallPlan(bool bAccepted, bool bDryRun, const TArray<FInstallStep>& Plan,
    TFunctionRef<bool()> Cancelled, TFunctionRef<bool(const FInstallStep&)> Execute,
    TFunctionRef<void(const FString&)> Log);
void ShowInstaller(bool bDryRun = false);
void UninstallManaged();
}
