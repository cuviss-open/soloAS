// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_ac5ec40a_Register();
void SAC_ac5ec40a_Unregister();

class FSAC_ac5ec40aModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_ac5ec40a_Register(); }
    void ShutdownModule() override { SAC_ac5ec40a_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_ac5ec40aModule, SAC_ac5ec40a)
