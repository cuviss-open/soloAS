#include "Modules/ModuleManager.h"

void SAC_bb437010_Register();
void SAC_bb437010_Unregister();

class FSAC_bb437010Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_bb437010_Register(); }
    void ShutdownModule() override { SAC_bb437010_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_bb437010Module, SAC_bb437010)
