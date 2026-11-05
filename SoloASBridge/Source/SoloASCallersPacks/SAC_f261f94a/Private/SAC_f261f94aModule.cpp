// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_f261f94a_Register();
void SAC_f261f94a_Unregister();

class FSAC_f261f94aModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_f261f94a_Register(); }
    void ShutdownModule() override { SAC_f261f94a_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_f261f94aModule, SAC_f261f94a)
