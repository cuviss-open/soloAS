// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "Containers/Array.h"
#include "Templates/SharedPointer.h"
#include <atomic>

class asITypeInfo;
class asIScriptModule;
class asCModule;
struct FAngelscriptModuleDesc;

namespace AngelscriptRetiredModules
{
asIScriptModule* RetainType(asITypeInfo* Type);
                                                                                                          
                                                                                                        
                                                                                                       
// GT counters only: callers may already be executing inside a read scope.
asIScriptModule* RetainCaptureModule(asIScriptModule* Module);
void ReleaseCaptureModule(asIScriptModule* Module);
bool HasModuleHolds(asIScriptModule* Module);
                                                                                                     
void ReleaseModule(asIScriptModule* Module);
bool DeferDiscard(const TSharedRef<FAngelscriptModuleDesc>& Module);
// Defined with the script struct value registry (ASStruct.cpp): adds the modules whose struct
// types still have values constructed by them.
void AppendStructInstanceModules(TArray<asCModule*>& Modules);
void Startup();
void Shutdown();
}

                                                                                                       
namespace AngelscriptCoreExtensionPoints::Private
{
extern std::atomic<unsigned int> ModuleCacheClientCount;
void CloseModuleCacheClients();
bool HasModuleGenerationLeases();
bool IsModuleCacheOwned(asIScriptModule* Module);
}
                                                                                                     
