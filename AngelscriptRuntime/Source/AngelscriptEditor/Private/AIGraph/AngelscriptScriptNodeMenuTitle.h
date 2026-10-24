// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Class.h"

namespace AngelscriptAIGraph
{
inline FText GetScriptNodeMenuTitle(const UClass& Class)
{
	return FText::FromString(FName::NameToDisplayString(Class.GetName(), false));
}
}
