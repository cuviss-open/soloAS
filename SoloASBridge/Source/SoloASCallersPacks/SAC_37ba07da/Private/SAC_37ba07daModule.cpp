#include "Modules/ModuleManager.h"

void SAC_37ba07da_Register();
void SAC_37ba07da_Unregister();

class FSAC_37ba07daModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_37ba07da_Register(); }
    void ShutdownModule() override { SAC_37ba07da_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_37ba07daModule, SAC_37ba07da)
