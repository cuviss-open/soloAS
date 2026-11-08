#include "Modules/ModuleManager.h"

void SAC_32f6b6b3_Register();
void SAC_32f6b6b3_Unregister();

class FSAC_32f6b6b3Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_32f6b6b3_Register(); }
    void ShutdownModule() override { SAC_32f6b6b3_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_32f6b6b3Module, SAC_32f6b6b3)
