#include "Modules/ModuleManager.h"

void SAC_a2973c6d_Register();
void SAC_a2973c6d_Unregister();

class FSAC_a2973c6dModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_a2973c6d_Register(); }
    void ShutdownModule() override { SAC_a2973c6d_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_a2973c6dModule, SAC_a2973c6d)
