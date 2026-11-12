                                                                                          
// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreTypes.h"
#include <atomic>

class asITypeInfo;
class asIScriptModule;

// Enable/Disable require a quiescent game-thread boundary. Each owner retains
// its own token; removing another/expired token never disables other owners.
ANGELSCRIPTCODE_API uint64 EnableAngelscriptStructLiveValues();
ANGELSCRIPTCODE_API void DisableAngelscriptStructLiveValues(uint64 Token);
ANGELSCRIPTCODE_API bool QueryAngelscriptStructLiveValues(const asITypeInfo* Type, int32& OutExternalLiveCount);

namespace AngelscriptStructLiveValues
{
extern ANGELSCRIPTCODE_API std::atomic<bool> Enabled;
ANGELSCRIPTCODE_API void TypeCreated(const asITypeInfo* Type);
ANGELSCRIPTCODE_API void TypeDestroyed(const asITypeInfo* Type);
ANGELSCRIPTCODE_API void TypeCompiled(const asITypeInfo* Type);
ANGELSCRIPTCODE_API void Invalidate(const asITypeInfo* Type);
ANGELSCRIPTCODE_API void InvalidateModule(const asIScriptModule* Module);
ANGELSCRIPTCODE_API void Completed(const asITypeInfo* Type, bool bConstructed);
}
                                                                                        
