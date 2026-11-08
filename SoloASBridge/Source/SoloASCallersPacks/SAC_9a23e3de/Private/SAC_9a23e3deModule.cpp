// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_9a23e3de_Register();
void SAC_9a23e3de_Unregister();

class FSAC_9a23e3deModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_9a23e3de_Register(); }
    void ShutdownModule() override { SAC_9a23e3de_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_9a23e3deModule, SAC_9a23e3de)
