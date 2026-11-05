// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_5fa6903f_Register();
void SAC_5fa6903f_Unregister();

class FSAC_5fa6903fModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_5fa6903f_Register(); }
    void ShutdownModule() override { SAC_5fa6903f_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_5fa6903fModule, SAC_5fa6903f)
