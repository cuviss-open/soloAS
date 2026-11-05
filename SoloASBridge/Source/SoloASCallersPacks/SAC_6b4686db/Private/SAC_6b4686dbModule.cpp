// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_6b4686db_Register();
void SAC_6b4686db_Unregister();

class FSAC_6b4686dbModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_6b4686db_Register(); }
    void ShutdownModule() override { SAC_6b4686db_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_6b4686dbModule, SAC_6b4686db)
