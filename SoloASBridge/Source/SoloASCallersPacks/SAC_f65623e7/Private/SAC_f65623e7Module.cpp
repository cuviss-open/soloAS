// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_f65623e7_Register();
void SAC_f65623e7_Unregister();

class FSAC_f65623e7Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_f65623e7_Register(); }
    void ShutdownModule() override { SAC_f65623e7_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_f65623e7Module, SAC_f65623e7)
