#include "Modules/ModuleManager.h"

void SAC_3c0b1b40_Register();
void SAC_3c0b1b40_Unregister();

class FSAC_3c0b1b40Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_3c0b1b40_Register(); }
    void ShutdownModule() override { SAC_3c0b1b40_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_3c0b1b40Module, SAC_3c0b1b40)
