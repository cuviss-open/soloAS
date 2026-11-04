#include "Modules/ModuleManager.h"

void SAC_f1fb7939_Register();
void SAC_f1fb7939_Unregister();

class FSAC_f1fb7939Module : public IModuleInterface
{
public:
    void StartupModule() override { SAC_f1fb7939_Register(); }
    void ShutdownModule() override { SAC_f1fb7939_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_f1fb7939Module, SAC_f1fb7939)
