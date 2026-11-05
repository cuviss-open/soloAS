// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_f6f164ad_Register();
void SAC_f6f164ad_Unregister();

class FSAC_f6f164adModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_f6f164ad_Register(); }
    void ShutdownModule() override { SAC_f6f164ad_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_f6f164adModule, SAC_f6f164ad)
