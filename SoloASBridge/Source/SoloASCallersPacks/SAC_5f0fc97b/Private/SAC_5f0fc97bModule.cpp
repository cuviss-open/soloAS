#include "Modules/ModuleManager.h"

void SAC_5f0fc97b_Register();
void SAC_5f0fc97b_Unregister();

class FSAC_5f0fc97bModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_5f0fc97b_Register(); }
    void ShutdownModule() override { SAC_5f0fc97b_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_5f0fc97bModule, SAC_5f0fc97b)
