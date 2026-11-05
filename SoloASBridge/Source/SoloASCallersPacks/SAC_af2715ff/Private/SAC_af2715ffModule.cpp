// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_af2715ff_Register();
void SAC_af2715ff_Unregister();

class FSAC_af2715ffModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_af2715ff_Register(); }
    void ShutdownModule() override { SAC_af2715ff_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_af2715ffModule, SAC_af2715ff)
