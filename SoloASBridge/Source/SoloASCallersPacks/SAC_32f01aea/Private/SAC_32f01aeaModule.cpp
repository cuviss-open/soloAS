// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_32f01aea_Register();
void SAC_32f01aea_Unregister();

class FSAC_32f01aeaModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_32f01aea_Register(); }
    void ShutdownModule() override { SAC_32f01aea_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_32f01aeaModule, SAC_32f01aea)
