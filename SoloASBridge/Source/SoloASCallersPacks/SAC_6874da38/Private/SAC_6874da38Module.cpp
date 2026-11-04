#include "Modules/ModuleManager.h"

void SAC_6874da38_Register();
void SAC_6874da38_Unregister();

class FSAC_6874da38Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_6874da38_Register(); }
    void ShutdownModule() override { SAC_6874da38_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_6874da38Module, SAC_6874da38)
