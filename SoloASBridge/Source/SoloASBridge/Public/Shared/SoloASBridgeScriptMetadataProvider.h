// Copyright Cuviss Studio. All Rights Reserved.
#ifndef SOLOASBRIDGE_SCRIPT_METADATA_PROVIDER_H
#define SOLOASBRIDGE_SCRIPT_METADATA_PROVIDER_H

#include "CoreMinimal.h"
#include "Features/IModularFeatures.h"

enum class ESoloASBridgePropertyTrait : uint16
{
    None = 0,
    CppConst = 1,
    CppRef = 2,
    CppEnumAsByte = 4,
    RuntimeGenerated = 8,
    WorldContext = 16,
    ConstTemplateArg = 32,
};
ENUM_CLASS_FLAGS(ESoloASBridgePropertyTrait)

inline constexpr uint16 SoloASBridgePropertyTraitMask = 63;

enum class ESoloASBridgeFunctionTrait : uint16
{
    None = 0,
    ScriptCallable = 1,
    RuntimeGenerated = 2,
    WorldContext = 4,
    NotInAngelscript = 8,
    NoNativeCaller = 16, // A: UE-AS fork UHT rejects the native pointer for this declaration.
};
ENUM_CLASS_FLAGS(ESoloASBridgeFunctionTrait)

inline constexpr uint16 SoloASBridgeFunctionTraitMask = 31;

struct FSoloASBridgeScriptFunctionDescriptor
{
    const ANSICHAR* OwnerName;
    const ANSICHAR* FunctionName;
};

struct FSoloASBridgeScriptDefaultArgumentDescriptor
{
    const ANSICHAR* OwnerName;
    const ANSICHAR* FunctionName;
    const ANSICHAR* ParameterName;
    uint32 ParameterOrdinal;
    const ANSICHAR* DefaultValueText;
};

// Empty FunctionName denotes a native member; /Inner, /Key, /Value and /Element address container children.
struct FSoloASBridgeNativeFunctionPropertyTraitDescriptor
{
    const ANSICHAR* OwnerName;
    const ANSICHAR* FunctionName;
    const ANSICHAR* PropertyName;
    uint32 ParameterOrdinal;
    ESoloASBridgePropertyTrait Traits;
};

struct FSoloASBridgeNativeFunctionTraitDescriptor
{
    const ANSICHAR* OwnerName;
    const ANSICHAR* FunctionName;
    ESoloASBridgeFunctionTrait Traits;
    // A: UE-AS fork's native caller preserves the returned reference's address.
    void (*NativeReferenceCaller)(UObject* Target, void** Arguments, void* ReturnValue) = nullptr;
};

// Constructed before a module's StartupModule and destroyed with its DLL.
class FSoloASBridgeScriptMetadataRegistration : public IModularFeature
{
public:
    static FName FeatureName() { return FName(TEXT("SoloAS.ScriptMetadata")); }
    const ANSICHAR* ModuleName;
    TConstArrayView<FSoloASBridgeScriptFunctionDescriptor> Functions;
    TConstArrayView<FSoloASBridgeScriptDefaultArgumentDescriptor> DefaultArguments;
    TConstArrayView<FSoloASBridgeNativeFunctionPropertyTraitDescriptor> PropertyTraits;
    TConstArrayView<FSoloASBridgeNativeFunctionTraitDescriptor> FunctionTraits;

    FSoloASBridgeScriptMetadataRegistration(
        const ANSICHAR* InModuleName,
        TConstArrayView<FSoloASBridgeScriptFunctionDescriptor> InFunctions,
        TConstArrayView<FSoloASBridgeScriptDefaultArgumentDescriptor> InDefaultArguments,
        TConstArrayView<FSoloASBridgeNativeFunctionPropertyTraitDescriptor> InPropertyTraits,
        TConstArrayView<FSoloASBridgeNativeFunctionTraitDescriptor> InFunctionTraits,
        bool bAutoRegister = true)
        : ModuleName(InModuleName), Functions(InFunctions), DefaultArguments(InDefaultArguments),
          PropertyTraits(InPropertyTraits), FunctionTraits(InFunctionTraits)
    {
        if (bAutoRegister) Register();
    }
    virtual ~FSoloASBridgeScriptMetadataRegistration()
    {
        Unregister();
    }
    void Register()
    {
        if (!bRegistered)
        {
            bRegistered = true;
            IModularFeatures::Get().RegisterModularFeature(FeatureName(), this);
        }
    }
    void Unregister()
    {
        if (bRegistered)
        {
            bRegistered = false;
            IModularFeatures::Get().UnregisterModularFeature(FeatureName(), this);
        }
    }
    FSoloASBridgeScriptMetadataRegistration(const FSoloASBridgeScriptMetadataRegistration&) = delete;
    FSoloASBridgeScriptMetadataRegistration& operator=(const FSoloASBridgeScriptMetadataRegistration&) = delete;
private:
    bool bRegistered = false;
};

#endif
