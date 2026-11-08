#include "Modules/ModuleManager.h"

void SAC_5abcbf4e_Register();
void SAC_5abcbf4e_Unregister();

class FSAC_5abcbf4eModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_5abcbf4e_Register(); }
    void ShutdownModule() override { SAC_5abcbf4e_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_5abcbf4eModule, SAC_5abcbf4e)
