#include "Modules/ModuleManager.h"

void SAC_3809a56c_Register();
void SAC_3809a56c_Unregister();

class FSAC_3809a56cModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_3809a56c_Register(); }
    void ShutdownModule() override { SAC_3809a56c_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_3809a56cModule, SAC_3809a56c)
