#include "Modules/ModuleManager.h"

void SAC_774c043b_Register();
void SAC_774c043b_Unregister();

class FSAC_774c043bModule : public IModuleInterface
{
public:
    void StartupModule() override { SAC_774c043b_Register(); }
    void ShutdownModule() override { SAC_774c043b_Unregister(); }
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FSAC_774c043bModule, SAC_774c043b)
