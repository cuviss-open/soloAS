#include "Modules/ModuleManager.h"

void SAC_4f708b72_Register();
void SAC_4f708b72_Unregister();

class FSAC_4f708b72Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_4f708b72_Register(); }
    void ShutdownModule() override { SAC_4f708b72_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_4f708b72Module, SAC_4f708b72)
