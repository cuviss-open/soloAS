#include "Modules/ModuleManager.h"

void SAC_51b1db90_Register();
void SAC_51b1db90_Unregister();

class FSAC_51b1db90Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_51b1db90_Register(); }
    void ShutdownModule() override { SAC_51b1db90_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_51b1db90Module, SAC_51b1db90)
