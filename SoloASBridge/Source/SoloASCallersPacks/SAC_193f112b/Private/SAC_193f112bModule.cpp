#include "Modules/ModuleManager.h"

void SAC_193f112b_Register();
void SAC_193f112b_Unregister();

class FSAC_193f112bModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_193f112b_Register(); }
    void ShutdownModule() override { SAC_193f112b_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_193f112bModule, SAC_193f112b)
