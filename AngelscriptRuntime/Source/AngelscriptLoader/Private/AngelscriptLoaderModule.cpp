#include "AngelscriptLoaderModule.h"
#include "AngelscriptCodeModule.h"
                                                                                                           
#include "SoloASEngineCompat.h"
#if WITH_EDITOR && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 4
#include "AngelscriptManager.h"
#include "UObject/Package.h"
#endif
                                                                                                         

IMPLEMENT_MODULE(FAngelscriptLoaderModule, AngelscriptLoader);

void FAngelscriptLoaderModule::StartupModule()
{
	FAngelscriptCodeModule::InitializeAngelscript();
                                                                                                           
#if WITH_EDITOR && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 4
    if (GIsEditor && !IsRunningCommandlet() && FModuleManager::Get().IsModuleLoaded(TEXT("PythonScriptPlugin")))
    {
        // A real module load lets Python scan the already-created /Script/Angelscript package.
        UPackage* Package = FAngelscriptManager::GetPackage();
        const uint32 Flags = Package->GetPackageFlags();
        IModuleInterface* Module = FModuleManager::Get().LoadModule(TEXT("Angelscript"));
        const bool Unchanged = Package == FAngelscriptManager::GetPackage()
            && Flags == Package->GetPackageFlags();
        ensureMsgf(Module && Unchanged, TEXT("Python bootstrap must preserve AngelScript package ownership."));
        UE_LOG(LogTemp, Display, TEXT("ASR-PYTHON-BOOTSTRAP-UE54: loaded=%s package-identity-and-flags-unchanged=%s"),
            Module ? TEXT("true") : TEXT("false"), Unchanged ? TEXT("true") : TEXT("false"));
    }
#endif
                                                                                                         
}

void FAngelscriptLoaderModule::ShutdownModule()
{
}
