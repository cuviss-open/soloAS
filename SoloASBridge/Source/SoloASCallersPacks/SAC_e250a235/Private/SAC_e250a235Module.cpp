#include "Modules/ModuleManager.h"

void SAC_e250a235_Register();
void SAC_e250a235_Unregister();

class FSAC_e250a235Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_e250a235_Register(); }
    void ShutdownModule() override { SAC_e250a235_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_e250a235Module, SAC_e250a235)
