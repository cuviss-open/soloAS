// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_59250736_Register();
void SAC_59250736_Unregister();

class FSAC_59250736Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_59250736_Register(); }
    void ShutdownModule() override { SAC_59250736_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_59250736Module, SAC_59250736)
