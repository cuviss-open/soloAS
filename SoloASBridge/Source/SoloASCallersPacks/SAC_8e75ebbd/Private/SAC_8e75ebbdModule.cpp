#include "Modules/ModuleManager.h"

void SAC_8e75ebbd_Register();
void SAC_8e75ebbd_Unregister();

class FSAC_8e75ebbdModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_8e75ebbd_Register(); }
    void ShutdownModule() override { SAC_8e75ebbd_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_8e75ebbdModule, SAC_8e75ebbd)
