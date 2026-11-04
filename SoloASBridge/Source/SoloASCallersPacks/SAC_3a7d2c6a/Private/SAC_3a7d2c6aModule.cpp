#include "Modules/ModuleManager.h"

void SAC_3a7d2c6a_Register();
void SAC_3a7d2c6a_Unregister();

class FSAC_3a7d2c6aModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_3a7d2c6a_Register(); }
    void ShutdownModule() override { SAC_3a7d2c6a_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_3a7d2c6aModule, SAC_3a7d2c6a)
