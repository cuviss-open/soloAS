// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned hot-patch explicit trigger for the
// AngelscriptModKit module. Zero new Runtime/Bridge registration points: the
// trigger re-verifies the staged bytes (MK-T702 chain via
// FModKitHotPatchLand::ValidatePatch) and drives the public
// FAngelscriptManager::CompileModules(SoftReloadOnly) entry, with the Runtime
// classifier as the single verdict source.

#pragma once

#include "CoreMinimal.h"

   
                                                                            
                                                                           
                                                                         
                                                                         
                                                                              
                                                                      
                                                                       
  
                                                                            
                                                                           
                                                                       
                                                                    
                                                                                  
                                                                  
                                                                           
                     
                                                                             
                                                                         
   
struct FModKitHotPatchTrigger
{
	/** Deferred retains the exact prepared bytes; the ticker retries before any terminal ledger state. */
	static bool Apply(const FString& PatchId, bool& bDeferred);
	static void Shutdown();
};
