// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_4d574013_Register();
void SAC_4d574013_Unregister();

class FSAC_4d574013Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_4d574013_Register(); }
    void ShutdownModule() override { SAC_4d574013_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_4d574013Module, SAC_4d574013)
