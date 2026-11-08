// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_c403894e_Register();
void SAC_c403894e_Unregister();

class FSAC_c403894eModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_c403894e_Register(); }
    void ShutdownModule() override { SAC_c403894e_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_c403894eModule, SAC_c403894e)
