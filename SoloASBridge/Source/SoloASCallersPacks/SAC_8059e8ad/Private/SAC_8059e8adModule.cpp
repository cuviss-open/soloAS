#include "Modules/ModuleManager.h"

void SAC_8059e8ad_Register();
void SAC_8059e8ad_Unregister();

class FSAC_8059e8adModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_8059e8ad_Register(); }
    void ShutdownModule() override { SAC_8059e8ad_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_8059e8adModule, SAC_8059e8ad)
