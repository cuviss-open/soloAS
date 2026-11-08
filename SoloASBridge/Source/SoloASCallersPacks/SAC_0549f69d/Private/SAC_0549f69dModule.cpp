#include "Modules/ModuleManager.h"

void SAC_0549f69d_Register();
void SAC_0549f69d_Unregister();

class FSAC_0549f69dModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_0549f69d_Register(); }
    void ShutdownModule() override { SAC_0549f69d_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_0549f69dModule, SAC_0549f69d)
