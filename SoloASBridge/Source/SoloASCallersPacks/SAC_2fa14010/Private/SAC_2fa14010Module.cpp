// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_2fa14010_Register();
void SAC_2fa14010_Unregister();

class FSAC_2fa14010Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_2fa14010_Register(); }
    void ShutdownModule() override { SAC_2fa14010_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_2fa14010Module, SAC_2fa14010)
