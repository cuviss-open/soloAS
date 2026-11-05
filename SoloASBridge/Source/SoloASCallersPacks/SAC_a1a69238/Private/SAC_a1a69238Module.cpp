// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_a1a69238_Register();
void SAC_a1a69238_Unregister();

class FSAC_a1a69238Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_a1a69238_Register(); }
    void ShutdownModule() override { SAC_a1a69238_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_a1a69238Module, SAC_a1a69238)
