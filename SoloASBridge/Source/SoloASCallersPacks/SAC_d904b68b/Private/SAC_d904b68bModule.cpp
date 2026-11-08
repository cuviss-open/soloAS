#include "Modules/ModuleManager.h"

void SAC_d904b68b_Register();
void SAC_d904b68b_Unregister();

class FSAC_d904b68bModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_d904b68b_Register(); }
    void ShutdownModule() override { SAC_d904b68b_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_d904b68bModule, SAC_d904b68b)
