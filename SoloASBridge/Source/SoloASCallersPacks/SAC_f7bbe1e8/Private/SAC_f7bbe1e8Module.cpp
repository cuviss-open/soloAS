// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_f7bbe1e8_Register();
void SAC_f7bbe1e8_Unregister();

class FSAC_f7bbe1e8Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_f7bbe1e8_Register(); }
    void ShutdownModule() override { SAC_f7bbe1e8_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_f7bbe1e8Module, SAC_f7bbe1e8)
