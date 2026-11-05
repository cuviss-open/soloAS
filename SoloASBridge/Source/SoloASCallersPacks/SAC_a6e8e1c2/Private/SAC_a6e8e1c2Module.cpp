// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_a6e8e1c2_Register();
void SAC_a6e8e1c2_Unregister();

class FSAC_a6e8e1c2Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_a6e8e1c2_Register(); }
    void ShutdownModule() override { SAC_a6e8e1c2_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_a6e8e1c2Module, SAC_a6e8e1c2)
