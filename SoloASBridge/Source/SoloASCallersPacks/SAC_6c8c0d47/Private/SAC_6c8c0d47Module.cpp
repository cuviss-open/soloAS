// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_6c8c0d47_Register();
void SAC_6c8c0d47_Unregister();

class FSAC_6c8c0d47Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_6c8c0d47_Register(); }
    void ShutdownModule() override { SAC_6c8c0d47_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_6c8c0d47Module, SAC_6c8c0d47)
