#include "Modules/ModuleManager.h"

void SAC_b80f5d8b_Register();
void SAC_b80f5d8b_Unregister();

class FSAC_b80f5d8bModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_b80f5d8b_Register(); }
    void ShutdownModule() override { SAC_b80f5d8b_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_b80f5d8bModule, SAC_b80f5d8b)
