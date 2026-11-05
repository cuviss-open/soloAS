// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_2dd5364e_Register();
void SAC_2dd5364e_Unregister();

class FSAC_2dd5364eModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_2dd5364e_Register(); }
    void ShutdownModule() override { SAC_2dd5364e_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_2dd5364eModule, SAC_2dd5364e)
