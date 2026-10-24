#pragma once

#include "Modules/ModuleInterface.h"

class SOLOASBRIDGE_API FSoloASBridgeModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
    // Importing editor modules can still hold this module's code on a failed restore.
    virtual bool SupportsDynamicReloading() override { return false; }
#endif
};
