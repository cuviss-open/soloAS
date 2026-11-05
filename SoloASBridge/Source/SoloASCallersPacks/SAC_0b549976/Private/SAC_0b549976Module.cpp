// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_0b549976_Register();
void SAC_0b549976_Unregister();

class FSAC_0b549976Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_0b549976_Register(); }
    void ShutdownModule() override { SAC_0b549976_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_0b549976Module, SAC_0b549976)
