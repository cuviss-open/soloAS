// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_a5efe256_Register();
void SAC_a5efe256_Unregister();

class FSAC_a5efe256Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_a5efe256_Register(); }
    void ShutdownModule() override { SAC_a5efe256_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_a5efe256Module, SAC_a5efe256)
