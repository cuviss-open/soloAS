#include "Modules/ModuleManager.h"

void SAC_ccabc5b5_Register();
void SAC_ccabc5b5_Unregister();

class FSAC_ccabc5b5Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_ccabc5b5_Register(); }
    void ShutdownModule() override { SAC_ccabc5b5_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_ccabc5b5Module, SAC_ccabc5b5)
