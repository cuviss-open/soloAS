// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

namespace AngelscriptActorReplacementTransaction
{
/** Returns false before mutation when the batch requires stock hierarchy replacement. */
bool Run(const TMap<UClass*, UClass*>& Classes, FString& OutError);
}
