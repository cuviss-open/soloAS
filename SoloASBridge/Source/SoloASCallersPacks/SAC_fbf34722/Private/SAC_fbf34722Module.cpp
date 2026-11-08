// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_fbf34722_Register();
void SAC_fbf34722_Unregister();

class FSAC_fbf34722Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_fbf34722_Register(); }
    void ShutdownModule() override { SAC_fbf34722_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_fbf34722Module, SAC_fbf34722)
