// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

namespace SoloASBridge::Maintenance
{
                                                                                            
// Game thread only. Duplicate IDs fail without replacing the existing entry.
// Static callbacks only: Bridge owns all stored state, including after module unload.
// Owners unregister in ShutdownModule; module unload also removes their entries.
SOLOASBRIDGEEDITOR_API bool RegisterMenuItem(FName Id, FName OwnerModule, const FText& Label, void (*Action)());
SOLOASBRIDGEEDITOR_API void UnregisterMenuItem(FName Id, FName OwnerModule);
                                                                                          
}
