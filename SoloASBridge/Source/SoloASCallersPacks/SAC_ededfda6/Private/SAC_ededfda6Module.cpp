#include "Modules/ModuleManager.h"

void SAC_ededfda6_Register();
void SAC_ededfda6_Unregister();

class FSAC_ededfda6Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_ededfda6_Register(); }
    void ShutdownModule() override { SAC_ededfda6_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_ededfda6Module, SAC_ededfda6)
