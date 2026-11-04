#include "Modules/ModuleManager.h"

void SAC_f84562e9_Register();
void SAC_f84562e9_Unregister();

class FSAC_f84562e9Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_f84562e9_Register(); }
    void ShutdownModule() override { SAC_f84562e9_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_f84562e9Module, SAC_f84562e9)
