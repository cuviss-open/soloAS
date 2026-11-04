#include "Modules/ModuleManager.h"

void SAC_c0c01674_Register();
void SAC_c0c01674_Unregister();

class FSAC_c0c01674Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_c0c01674_Register(); }
    void ShutdownModule() override { SAC_c0c01674_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_c0c01674Module, SAC_c0c01674)
