#include "Modules/ModuleManager.h"

void SAC_c104c5bb_Register();
void SAC_c104c5bb_Unregister();

class FSAC_c104c5bbModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_c104c5bb_Register(); }
    void ShutdownModule() override { SAC_c104c5bb_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_c104c5bbModule, SAC_c104c5bb)
