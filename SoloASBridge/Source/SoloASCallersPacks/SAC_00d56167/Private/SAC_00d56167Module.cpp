#include "Modules/ModuleManager.h"

void SAC_00d56167_Register();
void SAC_00d56167_Unregister();

class FSAC_00d56167Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_00d56167_Register(); }
    void ShutdownModule() override { SAC_00d56167_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_00d56167Module, SAC_00d56167)
