// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_22b8c031_Register();
void SAC_22b8c031_Unregister();

class FSAC_22b8c031Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_22b8c031_Register(); }
    void ShutdownModule() override { SAC_22b8c031_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_22b8c031Module, SAC_22b8c031)
