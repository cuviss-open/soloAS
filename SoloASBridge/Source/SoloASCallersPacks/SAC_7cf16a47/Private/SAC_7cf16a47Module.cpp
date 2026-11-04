#include "Modules/ModuleManager.h"

void SAC_7cf16a47_Register();
void SAC_7cf16a47_Unregister();

class FSAC_7cf16a47Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_7cf16a47_Register(); }
    void ShutdownModule() override { SAC_7cf16a47_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_7cf16a47Module, SAC_7cf16a47)
