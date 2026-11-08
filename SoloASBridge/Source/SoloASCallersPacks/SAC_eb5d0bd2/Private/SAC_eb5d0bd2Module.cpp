// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_eb5d0bd2_Register();
void SAC_eb5d0bd2_Unregister();

class FSAC_eb5d0bd2Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_eb5d0bd2_Register(); }
    void ShutdownModule() override { SAC_eb5d0bd2_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_eb5d0bd2Module, SAC_eb5d0bd2)
