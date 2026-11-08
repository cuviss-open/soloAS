#include "Modules/ModuleManager.h"

void SAC_8ce91f0e_Register();
void SAC_8ce91f0e_Unregister();

class FSAC_8ce91f0eModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_8ce91f0e_Register(); }
    void ShutdownModule() override { SAC_8ce91f0e_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_8ce91f0eModule, SAC_8ce91f0e)
