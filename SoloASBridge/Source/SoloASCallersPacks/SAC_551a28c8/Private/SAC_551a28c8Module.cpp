// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_551a28c8_Register();
void SAC_551a28c8_Unregister();

class FSAC_551a28c8Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_551a28c8_Register(); }
    void ShutdownModule() override { SAC_551a28c8_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_551a28c8Module, SAC_551a28c8)
