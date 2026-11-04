#include "Modules/ModuleManager.h"

void SAC_79a82bb4_Register();
void SAC_79a82bb4_Unregister();

class FSAC_79a82bb4Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_79a82bb4_Register(); }
    void ShutdownModule() override { SAC_79a82bb4_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_79a82bb4Module, SAC_79a82bb4)
