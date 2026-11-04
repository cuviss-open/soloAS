#include "Modules/ModuleManager.h"

void SAC_8f9cc8d6_Register();
void SAC_8f9cc8d6_Unregister();

class FSAC_8f9cc8d6Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_8f9cc8d6_Register(); }
    void ShutdownModule() override { SAC_8f9cc8d6_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_8f9cc8d6Module, SAC_8f9cc8d6)
