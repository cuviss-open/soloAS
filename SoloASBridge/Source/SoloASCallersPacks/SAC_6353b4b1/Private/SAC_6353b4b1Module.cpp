// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_6353b4b1_Register();
void SAC_6353b4b1_Unregister();

class FSAC_6353b4b1Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_6353b4b1_Register(); }
    void ShutdownModule() override { SAC_6353b4b1_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_6353b4b1Module, SAC_6353b4b1)
