// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_f68e3a0f_Register();
void SAC_f68e3a0f_Unregister();

class FSAC_f68e3a0fModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_f68e3a0f_Register(); }
    void ShutdownModule() override { SAC_f68e3a0f_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_f68e3a0fModule, SAC_f68e3a0f)
