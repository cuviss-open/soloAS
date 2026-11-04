#include "Modules/ModuleManager.h"

void SAC_9d95082a_Register();
void SAC_9d95082a_Unregister();

class FSAC_9d95082aModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_9d95082a_Register(); }
    void ShutdownModule() override { SAC_9d95082a_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_9d95082aModule, SAC_9d95082a)
