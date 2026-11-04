#include "Modules/ModuleManager.h"

void SAC_71de6bed_Register();
void SAC_71de6bed_Unregister();

class FSAC_71de6bedModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_71de6bed_Register(); }
    void ShutdownModule() override { SAC_71de6bed_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_71de6bedModule, SAC_71de6bed)
