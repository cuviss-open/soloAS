// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "Templates/SharedPointer.h"

class asITypeInfo;
class asIScriptModule;
struct FAngelscriptModuleDesc;

namespace AngelscriptRetiredModules
{
asIScriptModule* RetainType(asITypeInfo* Type);
void ReleaseModule(asIScriptModule* Module);
bool DeferDiscard(const TSharedRef<FAngelscriptModuleDesc>& Module);
void Startup();
void Shutdown();
}
