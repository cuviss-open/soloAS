// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Modules/ModuleManager.h"

void SAC_7690dbdf_Register();
void SAC_7690dbdf_Unregister();

class FSAC_7690dbdfModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_7690dbdf_Register(); }
    void ShutdownModule() override { SAC_7690dbdf_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_7690dbdfModule, SAC_7690dbdf)
