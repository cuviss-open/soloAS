#include "Modules/ModuleManager.h"

void SAC_0d092bd1_Register();
void SAC_0d092bd1_Unregister();

class FSAC_0d092bd1Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_0d092bd1_Register(); }
    void ShutdownModule() override { SAC_0d092bd1_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_0d092bd1Module, SAC_0d092bd1)
