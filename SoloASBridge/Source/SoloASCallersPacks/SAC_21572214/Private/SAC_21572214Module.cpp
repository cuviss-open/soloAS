#include "Modules/ModuleManager.h"

void SAC_21572214_Register();
void SAC_21572214_Unregister();

class FSAC_21572214Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_21572214_Register(); }
    void ShutdownModule() override { SAC_21572214_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_21572214Module, SAC_21572214)
