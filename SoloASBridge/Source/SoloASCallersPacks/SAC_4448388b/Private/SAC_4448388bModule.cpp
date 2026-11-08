// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_4448388b_Register();
void SAC_4448388b_Unregister();

class FSAC_4448388bModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_4448388b_Register(); }
    void ShutdownModule() override { SAC_4448388b_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_4448388bModule, SAC_4448388b)
