// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptModuleGeneration.h"
#include "StartAngelscriptHeaders.h"
#include "as_module.h"
#include "EndAngelscriptHeaders.h"

uint64 AngelscriptCoreExtensionPoints::Private::GetModuleGeneration(const asCModule* Module)
{
    return Module ? Module->coreGenerationId : 0;
}
