// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_6d02b31d_Register();
void SAC_6d02b31d_Unregister();

class FSAC_6d02b31dModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_6d02b31d_Register(); }
    void ShutdownModule() override { SAC_6d02b31d_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_6d02b31dModule, SAC_6d02b31d)
