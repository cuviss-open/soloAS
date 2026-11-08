// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_004c0086_Register();
void SAC_004c0086_Unregister();

class FSAC_004c0086Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_004c0086_Register(); }
    void ShutdownModule() override { SAC_004c0086_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_004c0086Module, SAC_004c0086)
