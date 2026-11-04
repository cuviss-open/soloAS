#include "Modules/ModuleManager.h"

void SAC_8e358815_Register();
void SAC_8e358815_Unregister();

class FSAC_8e358815Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_8e358815_Register(); }
    void ShutdownModule() override { SAC_8e358815_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_8e358815Module, SAC_8e358815)
