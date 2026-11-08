#include "Modules/ModuleManager.h"

void SAC_c76a72be_Register();
void SAC_c76a72be_Unregister();

class FSAC_c76a72beModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_c76a72be_Register(); }
    void ShutdownModule() override { SAC_c76a72be_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_c76a72beModule, SAC_c76a72be)
