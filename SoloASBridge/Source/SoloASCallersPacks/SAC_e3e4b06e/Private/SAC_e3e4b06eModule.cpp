#include "Modules/ModuleManager.h"

void SAC_e3e4b06e_Register();
void SAC_e3e4b06e_Unregister();

class FSAC_e3e4b06eModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_e3e4b06e_Register(); }
    void ShutdownModule() override { SAC_e3e4b06e_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_e3e4b06eModule, SAC_e3e4b06e)
