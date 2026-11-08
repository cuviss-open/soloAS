// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_12d00590_Register();
void SAC_12d00590_Unregister();

class FSAC_12d00590Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_12d00590_Register(); }
    void ShutdownModule() override { SAC_12d00590_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_12d00590Module, SAC_12d00590)
