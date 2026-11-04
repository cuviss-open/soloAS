#include "Modules/ModuleManager.h"

void SAC_88f7fc40_Register();
void SAC_88f7fc40_Unregister();

class FSAC_88f7fc40Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_88f7fc40_Register(); }
    void ShutdownModule() override { SAC_88f7fc40_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_88f7fc40Module, SAC_88f7fc40)
