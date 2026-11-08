// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_e889b740_Register();
void SAC_e889b740_Unregister();

class FSAC_e889b740Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_e889b740_Register(); }
    void ShutdownModule() override { SAC_e889b740_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_e889b740Module, SAC_e889b740)
