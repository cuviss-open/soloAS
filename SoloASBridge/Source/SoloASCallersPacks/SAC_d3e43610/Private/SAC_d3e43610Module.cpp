// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_d3e43610_Register();
void SAC_d3e43610_Unregister();

class FSAC_d3e43610Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_d3e43610_Register(); }
    void ShutdownModule() override { SAC_d3e43610_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_d3e43610Module, SAC_d3e43610)
