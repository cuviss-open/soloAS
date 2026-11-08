// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_2f807e29_Register();
void SAC_2f807e29_Unregister();

class FSAC_2f807e29Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_2f807e29_Register(); }
    void ShutdownModule() override { SAC_2f807e29_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_2f807e29Module, SAC_2f807e29)
