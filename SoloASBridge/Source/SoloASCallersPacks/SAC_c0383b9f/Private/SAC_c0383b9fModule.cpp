// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_c0383b9f_Register();
void SAC_c0383b9f_Unregister();

class FSAC_c0383b9fModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_c0383b9f_Register(); }
    void ShutdownModule() override { SAC_c0383b9f_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_c0383b9fModule, SAC_c0383b9f)
