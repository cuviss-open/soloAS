#include "Modules/ModuleManager.h"

void SAC_98b27e58_Register();
void SAC_98b27e58_Unregister();

class FSAC_98b27e58Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_98b27e58_Register(); }
    void ShutdownModule() override { SAC_98b27e58_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_98b27e58Module, SAC_98b27e58)
