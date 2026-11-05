// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_d4206f9e_Register();
void SAC_d4206f9e_Unregister();

class FSAC_d4206f9eModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_d4206f9e_Register(); }
    void ShutdownModule() override { SAC_d4206f9e_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_d4206f9eModule, SAC_d4206f9e)
