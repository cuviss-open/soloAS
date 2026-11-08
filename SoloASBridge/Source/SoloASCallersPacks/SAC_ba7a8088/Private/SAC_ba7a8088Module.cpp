// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_ba7a8088_Register();
void SAC_ba7a8088_Unregister();

class FSAC_ba7a8088Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_ba7a8088_Register(); }
    void ShutdownModule() override { SAC_ba7a8088_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_ba7a8088Module, SAC_ba7a8088)
