// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_7d068d7d_Register();
void SAC_7d068d7d_Unregister();

class FSAC_7d068d7dModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_7d068d7d_Register(); }
    void ShutdownModule() override { SAC_7d068d7d_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_7d068d7dModule, SAC_7d068d7d)
