// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_b9d97282_Register();
void SAC_b9d97282_Unregister();

class FSAC_b9d97282Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_b9d97282_Register(); }
    void ShutdownModule() override { SAC_b9d97282_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_b9d97282Module, SAC_b9d97282)
