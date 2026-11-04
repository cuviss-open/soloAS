#include "Modules/ModuleManager.h"

void SAC_ad6cd2c9_Register();
void SAC_ad6cd2c9_Unregister();

class FSAC_ad6cd2c9Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_ad6cd2c9_Register(); }
    void ShutdownModule() override { SAC_ad6cd2c9_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_ad6cd2c9Module, SAC_ad6cd2c9)
