// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_7ca7dea9_Register();
void SAC_7ca7dea9_Unregister();

class FSAC_7ca7dea9Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_7ca7dea9_Register(); }
    void ShutdownModule() override { SAC_7ca7dea9_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_7ca7dea9Module, SAC_7ca7dea9)
