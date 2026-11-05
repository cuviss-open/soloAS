// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_ecbab458_Register();
void SAC_ecbab458_Unregister();

class FSAC_ecbab458Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_ecbab458_Register(); }
    void ShutdownModule() override { SAC_ecbab458_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_ecbab458Module, SAC_ecbab458)
