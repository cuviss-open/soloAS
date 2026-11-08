// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_7d25fadd_Register();
void SAC_7d25fadd_Unregister();

class FSAC_7d25faddModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_7d25fadd_Register(); }
    void ShutdownModule() override { SAC_7d25fadd_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_7d25faddModule, SAC_7d25fadd)
