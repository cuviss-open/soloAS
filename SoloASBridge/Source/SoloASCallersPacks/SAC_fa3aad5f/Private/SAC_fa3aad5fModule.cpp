// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_fa3aad5f_Register();
void SAC_fa3aad5f_Unregister();

class FSAC_fa3aad5fModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_fa3aad5f_Register(); }
    void ShutdownModule() override { SAC_fa3aad5f_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_fa3aad5fModule, SAC_fa3aad5f)
