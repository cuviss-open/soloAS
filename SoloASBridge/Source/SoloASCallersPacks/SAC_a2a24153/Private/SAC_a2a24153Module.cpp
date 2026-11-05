// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_a2a24153_Register();
void SAC_a2a24153_Unregister();

class FSAC_a2a24153Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_a2a24153_Register(); }
    void ShutdownModule() override { SAC_a2a24153_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_a2a24153Module, SAC_a2a24153)
