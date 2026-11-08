#include "Modules/ModuleManager.h"

void SAC_6e4c1453_Register();
void SAC_6e4c1453_Unregister();

class FSAC_6e4c1453Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_6e4c1453_Register(); }
    void ShutdownModule() override { SAC_6e4c1453_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_6e4c1453Module, SAC_6e4c1453)
