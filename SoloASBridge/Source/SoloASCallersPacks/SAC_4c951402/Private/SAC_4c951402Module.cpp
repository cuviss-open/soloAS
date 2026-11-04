#include "Modules/ModuleManager.h"

void SAC_4c951402_Register();
void SAC_4c951402_Unregister();

class FSAC_4c951402Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_4c951402_Register(); }
    void ShutdownModule() override { SAC_4c951402_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_4c951402Module, SAC_4c951402)
