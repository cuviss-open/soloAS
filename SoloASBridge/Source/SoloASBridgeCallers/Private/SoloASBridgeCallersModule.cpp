#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Runtime/SoloASBridgeScriptMetadata.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Runtime/Launch/Resources/Version.h"
#if PLATFORM_WINDOWS && !IS_MONOLITHIC
#include "Windows/WindowsHWrapper.h"
#elif PLATFORM_MAC && !IS_MONOLITHIC
#include <dlfcn.h>
#endif

namespace
{
struct FNativeCallerModule
{
    const TCHAR* NativeModule;
    const TCHAR* Plugin;
    const TCHAR* CallerModule;
};
const FNativeCallerModule NativeCallerModules[] =
{
#include "NativeCallerModules.inl"
};
}

class FSoloASBridgeCallersModule : public IModuleInterface
{
public:
    void StartupModule() override
    {
        if (!LoadNativeMetadata())
            UE_LOG(LogTemp, Fatal, TEXT("SB-NATIVE-METADATA: engine-native metadata snapshot is missing or invalid."));
        ModulesChanged = FModuleManager::Get().OnModulesChanged().AddRaw(this, &FSoloASBridgeCallersModule::ModuleChanged);
        for (const FNativeCallerModule& Entry : NativeCallerModules)
            LoadCaller(Entry);
    }

    void ShutdownModule() override
    {
        FModuleManager::Get().OnModulesChanged().Remove(ModulesChanged);
        SoloASBridge::UnregisterScriptMetadata(this);
    }

    bool SupportsDynamicReloading() override { return false; }

private:
    FDelegateHandle ModulesChanged;
    TArray<TArray<ANSICHAR>> Strings;

    bool NativeBinaryMapped(FName Module)
    {
        FModuleManager& Manager = FModuleManager::Get();
        if (Manager.IsModuleLoaded(Module)) return true;
#if IS_MONOLITHIC
        return Manager.ModuleExists(*Module.ToString());
#else
        Manager.AddModule(Module);
        const FString Filename = Manager.GetModuleFilename(Module);
        if (Filename.IsEmpty()) return false;
#if PLATFORM_WINDOWS
        return GetModuleHandleW(*FPaths::GetCleanFilename(Filename)) != nullptr;
#elif PLATFORM_MAC
        void* Handle = dlopen(TCHAR_TO_UTF8(*Filename), RTLD_NOLOAD | RTLD_LAZY | RTLD_LOCAL);
        if (!Handle)
        {
            const FString RPath = TEXT("@rpath/") + FPaths::GetCleanFilename(Filename);
            Handle = dlopen(TCHAR_TO_UTF8(*RPath), RTLD_NOLOAD | RTLD_LAZY | RTLD_LOCAL);
        }
        if (!Handle) return false;
        dlclose(Handle);
        return true;
#else
        return false;
#endif
#endif
    }

    void LoadCaller(const FNativeCallerModule& Entry)
    {
        if (*Entry.Plugin)
        {
            TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Entry.Plugin);
            if (!Plugin || !Plugin->IsEnabled()) return;
        }
        if (!NativeBinaryMapped(Entry.NativeModule)) return;
        const bool Started = FModuleManager::Get().IsModuleLoaded(Entry.NativeModule);
        EModuleLoadResult Failure;
        if (!FModuleManager::Get().LoadModuleWithFailureReason(Entry.CallerModule, Failure))
            UE_LOG(LogTemp, Fatal, TEXT("SB-CALLER-FAILED native=%s caller=%s reason=%d"),
                Entry.NativeModule, Entry.CallerModule, static_cast<int32>(Failure));
        UE_LOG(LogTemp, Verbose, TEXT("SB-CALLER-READY native=%s caller=%s started=%d"),
            Entry.NativeModule, Entry.CallerModule, Started);
    }

    void ModuleChanged(FName Name, EModuleChangeReason Reason)
    {
        for (const FNativeCallerModule& Entry : NativeCallerModules)
        {
            if (Name != FName(Entry.NativeModule)) continue;
            if (Reason == EModuleChangeReason::ModuleLoaded)
                LoadCaller(Entry);
            else if (Reason == EModuleChangeReason::ModuleUnloaded
                && FModuleManager::Get().IsModuleLoaded(Entry.CallerModule))
                FModuleManager::Get().UnloadModule(Entry.CallerModule, false, false);
            return;
        }
    }

    const ANSICHAR* Text(const TSharedPtr<FJsonObject>& Row, const TCHAR* Key)
    {
        const FString Value = Row->GetStringField(Key);
        FTCHARToUTF8 Encoded(*Value);
        TArray<ANSICHAR>& Stored = Strings.AddDefaulted_GetRef();
        Stored.Append(Encoded.Get(), Encoded.Length() + 1);
        return Stored.GetData();
    }

    bool LoadNativeMetadata()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SoloASBridge"));
        if (!Plugin) return false;
                                                                                                                                                
        const FString File = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Config/NativeMetadata"),
            FString::Printf(TEXT("UE5%d.json.gz"), ENGINE_MINOR_VERSION));
        // SOLOAS-COMPAT-END
        TArray<uint8> Compressed;
        if (!FFileHelper::LoadFileToArray(Compressed, *File) || Compressed.Num() < 18) return false;
        const int32 Tail = Compressed.Num() - 4;
        const uint32 Size = uint32(Compressed[Tail]) | (uint32(Compressed[Tail + 1]) << 8)
            | (uint32(Compressed[Tail + 2]) << 16) | (uint32(Compressed[Tail + 3]) << 24);
        if (Size == 0 || Size > 64 * 1024 * 1024) return false;
        TArray<uint8> Bytes;
        Bytes.SetNumUninitialized(Size + 1);
        if (!FCompression::UncompressMemory(NAME_Gzip, Bytes.GetData(), Size,
                Compressed.GetData(), Compressed.Num())) return false;
        Bytes[Size] = 0;
        const FString Json = UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()));
        TSharedPtr<FJsonObject> Root;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root) return false;
        TArray<FSoloASBridgeScriptFunctionDescriptor> Functions;
        TArray<FSoloASBridgeScriptDefaultArgumentDescriptor> Defaults;
        TArray<FSoloASBridgeNativeFunctionPropertyTraitDescriptor> Properties;
        TArray<FSoloASBridgeNativeFunctionTraitDescriptor> Traits;
        for (const auto& Value : Root->GetArrayField(TEXT("functions")))
        {
            const auto& Row = Value->AsObject();
            Functions.Add({ Text(Row, TEXT("owner")), Text(Row, TEXT("name")) });
        }
        for (const auto& Value : Root->GetArrayField(TEXT("defaultArguments")))
        {
            const auto& Row = Value->AsObject();
            Defaults.Add({ Text(Row, TEXT("owner")), Text(Row, TEXT("function")), Text(Row, TEXT("parameter")),
                static_cast<uint32>(Row->GetNumberField(TEXT("parameterOrdinal"))), Text(Row, TEXT("value")) });
        }
        for (const auto& Value : Root->GetArrayField(TEXT("propertyTraits")))
        {
            const auto& Row = Value->AsObject();
            Properties.Add({ Text(Row, TEXT("owner")), Text(Row, TEXT("function")), Text(Row, TEXT("property")),
                static_cast<uint32>(Row->GetNumberField(TEXT("parameterOrdinal"))),
                static_cast<ESoloASBridgePropertyTrait>(static_cast<uint16>(Row->GetNumberField(TEXT("traitBits")))) });
        }
        for (const auto& Value : Root->GetArrayField(TEXT("functionTraits")))
        {
            const auto& Row = Value->AsObject();
            if (Row->GetBoolField(TEXT("hasNativeCaller"))) continue;
            Traits.Add({ Text(Row, TEXT("owner")), Text(Row, TEXT("function")),
                static_cast<ESoloASBridgeFunctionTrait>(static_cast<uint16>(Row->GetNumberField(TEXT("traitBits")))), nullptr });
        }
        SoloASBridge::RegisterScriptMetadata(this, "SoloASBridgeCallers", MakeArrayView(Functions),
            MakeArrayView(Defaults), MakeArrayView(Properties), MakeArrayView(Traits));
        return true;
    }
};

IMPLEMENT_MODULE(FSoloASBridgeCallersModule, SoloASBridgeCallers)
