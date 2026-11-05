// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_8014fbda_Register();
void SAC_8014fbda_Unregister();

class FSAC_8014fbdaModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_8014fbda_Register(); }
    void ShutdownModule() override { SAC_8014fbda_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_8014fbdaModule, SAC_8014fbda)
