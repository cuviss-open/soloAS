// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_66e47cd0_Register();
void SAC_66e47cd0_Unregister();

class FSAC_66e47cd0Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_66e47cd0_Register(); }
    void ShutdownModule() override { SAC_66e47cd0_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_66e47cd0Module, SAC_66e47cd0)
