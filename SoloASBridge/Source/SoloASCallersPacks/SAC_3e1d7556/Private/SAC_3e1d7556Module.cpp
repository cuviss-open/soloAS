#include "Modules/ModuleManager.h"

void SAC_3e1d7556_Register();
void SAC_3e1d7556_Unregister();

class FSAC_3e1d7556Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_3e1d7556_Register(); }
    void ShutdownModule() override { SAC_3e1d7556_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_3e1d7556Module, SAC_3e1d7556)
