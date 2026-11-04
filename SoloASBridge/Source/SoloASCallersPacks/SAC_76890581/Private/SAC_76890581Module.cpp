#include "Modules/ModuleManager.h"

void SAC_76890581_Register();
void SAC_76890581_Unregister();

class FSAC_76890581Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_76890581_Register(); }
    void ShutdownModule() override { SAC_76890581_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_76890581Module, SAC_76890581)
