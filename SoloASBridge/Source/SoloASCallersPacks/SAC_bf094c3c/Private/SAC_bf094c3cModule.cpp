// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_bf094c3c_Register();
void SAC_bf094c3c_Unregister();

class FSAC_bf094c3cModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_bf094c3c_Register(); }
    void ShutdownModule() override { SAC_bf094c3c_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_bf094c3cModule, SAC_bf094c3c)
