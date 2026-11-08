// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_34745ed5_Register();
void SAC_34745ed5_Unregister();

class FSAC_34745ed5Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_34745ed5_Register(); }
    void ShutdownModule() override { SAC_34745ed5_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_34745ed5Module, SAC_34745ed5)
