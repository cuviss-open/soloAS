// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_1eecdd5c_Register();
void SAC_1eecdd5c_Unregister();

class FSAC_1eecdd5cModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_1eecdd5c_Register(); }
    void ShutdownModule() override { SAC_1eecdd5c_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_1eecdd5cModule, SAC_1eecdd5c)
