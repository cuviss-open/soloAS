// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_34e0eb78_Register();
void SAC_34e0eb78_Unregister();

class FSAC_34e0eb78Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_34e0eb78_Register(); }
    void ShutdownModule() override { SAC_34e0eb78_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_34e0eb78Module, SAC_34e0eb78)
