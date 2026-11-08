// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once
class UFunction;
namespace SoloASCoreExtensions::NativeEventSuper
{
bool IsEnabled();
void Start();
void Stop();
bool IsEligible(UFunction* Function);
}
