// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_99343b85_Register();
void SAC_99343b85_Unregister();

class FSAC_99343b85Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_99343b85_Register(); }
    void ShutdownModule() override { SAC_99343b85_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_99343b85Module, SAC_99343b85)
