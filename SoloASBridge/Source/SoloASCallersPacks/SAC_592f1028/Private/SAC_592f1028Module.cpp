// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_592f1028_Register();
void SAC_592f1028_Unregister();

class FSAC_592f1028Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_592f1028_Register(); }
    void ShutdownModule() override { SAC_592f1028_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_592f1028Module, SAC_592f1028)
