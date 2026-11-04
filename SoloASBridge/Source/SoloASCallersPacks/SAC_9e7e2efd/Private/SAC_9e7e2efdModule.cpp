#include "Modules/ModuleManager.h"

void SAC_9e7e2efd_Register();
void SAC_9e7e2efd_Unregister();

class FSAC_9e7e2efdModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_9e7e2efd_Register(); }
    void ShutdownModule() override { SAC_9e7e2efd_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_9e7e2efdModule, SAC_9e7e2efd)
