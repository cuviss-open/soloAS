// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_ae1242a3_Register();
void SAC_ae1242a3_Unregister();

class FSAC_ae1242a3Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_ae1242a3_Register(); }
    void ShutdownModule() override { SAC_ae1242a3_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_ae1242a3Module, SAC_ae1242a3)
