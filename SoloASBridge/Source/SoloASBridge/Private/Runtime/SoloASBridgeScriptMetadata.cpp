#include "Runtime/SoloASBridgeScriptMetadata.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"

namespace
{
#include "SoloASBridgeScriptMetadata.soloasbridge.uht.inl"

struct FModuleMetadata
{
    FName ModuleName;
    TSharedPtr<const FSoloASBridgeScriptMetadata, ESPMode::ThreadSafe> Data;
    uint64 Sequence = 0;
    bool bLoaded = true;
};

struct FMetadataRegistry
{
    FCriticalSection Lock;
    TMap<const void*, FModuleMetadata> Modules;
    TSharedPtr<const FSoloASBridgeScriptMetadata, ESPMode::ThreadSafe> Snapshot;
    uint64 Sequence = 0;
    FDelegateHandle ModulesChanged;
    FDelegateHandle FeatureRegistered;
    FDelegateHandle FeatureUnregistered;
};

FMetadataRegistry& GetRegistry()
{
    static FMetadataRegistry* Registry = new FMetadataRegistry;
    return *Registry;
}

const ANSICHAR* CopyString(FSoloASBridgeScriptMetadata& Data, const ANSICHAR* Value)
{
    TArray<ANSICHAR>& Storage = Data.Strings.AddDefaulted_GetRef();
    Storage.Append(Value, FCStringAnsi::Strlen(Value) + 1);
    return Storage.GetData();
}

int32 CompareFunction(const ANSICHAR* LeftOwner, const ANSICHAR* LeftFunction,
    const ANSICHAR* RightOwner, const ANSICHAR* RightFunction)
{
    const int32 Order = FCStringAnsi::Strcmp(LeftOwner, RightOwner);
    return Order == 0 ? FCStringAnsi::Strcmp(LeftFunction, RightFunction) : Order;
}

template<typename T, typename FSameKey, typename FSameValue>
void Deduplicate(TArray<T>& Rows, FSameKey SameKey, FSameValue SameValue)
{
    int32 Count = 0;
    for (int32 Index = 0; Index < Rows.Num(); ++Index)
    {
        if (Count > 0 && SameKey(Rows[Count - 1], Rows[Index]))
        {
            if (!SameValue(Rows[Count - 1], Rows[Index]))
                UE_LOG(LogTemp, Warning, TEXT("Conflicting script metadata for %s::%s; newest registration wins."),
                    ANSI_TO_TCHAR(Rows[Index].OwnerName), ANSI_TO_TCHAR(Rows[Index].FunctionName));
            Rows[Count - 1] = Rows[Index];
        }
        else
        {
            if (Count != Index) Rows[Count] = Rows[Index];
            ++Count;
        }
    }
    Rows.SetNum(Count, EAllowShrinking::No);
}

void FeatureRegistered(const FName& Type, IModularFeature* Feature)
{
    if (Type != FSoloASBridgeScriptMetadataRegistration::FeatureName())
        return;
    const auto& Source = *static_cast<FSoloASBridgeScriptMetadataRegistration*>(Feature);
    SoloASBridge::RegisterScriptMetadata(Feature, Source.ModuleName, Source.Functions,
        Source.DefaultArguments, Source.PropertyTraits, Source.FunctionTraits);
}

void FeatureUnregistered(const FName& Type, IModularFeature* Feature)
{
    if (Type == FSoloASBridgeScriptMetadataRegistration::FeatureName())
        SoloASBridge::UnregisterScriptMetadata(Feature);
}

void ModuleChanged(FName Name, EModuleChangeReason Reason)
{
    if (Reason != EModuleChangeReason::ModuleLoaded && Reason != EModuleChangeReason::ModuleUnloaded)
        return;
    FMetadataRegistry& Registry = GetRegistry();
    FScopeLock Scope(&Registry.Lock);
    for (auto& Entry : Registry.Modules)
    {
        if (Entry.Value.ModuleName == Name)
        {
            Entry.Value.bLoaded = Reason == EModuleChangeReason::ModuleLoaded;
            Registry.Snapshot.Reset();
        }
    }
}
}

void SoloASBridge::RegisterScriptMetadata(
    const void* Owner, const ANSICHAR* ModuleName,
    TConstArrayView<FSoloASBridgeScriptFunctionDescriptor> Functions,
    TConstArrayView<FSoloASBridgeScriptDefaultArgumentDescriptor> DefaultArguments,
    TConstArrayView<FSoloASBridgeNativeFunctionPropertyTraitDescriptor> PropertyTraits,
    TConstArrayView<FSoloASBridgeNativeFunctionTraitDescriptor> FunctionTraits)
{
    check(Owner != nullptr && ModuleName != nullptr);
    auto Data = MakeShared<FSoloASBridgeScriptMetadata, ESPMode::ThreadSafe>();
    // Copy every string: a reader's snapshot may outlive the originating DLL.
    for (auto Row : Functions)
    {
        Row.OwnerName = CopyString(*Data, Row.OwnerName);
        Row.FunctionName = CopyString(*Data, Row.FunctionName);
        Data->Functions.Add(Row);
    }
    for (auto Row : DefaultArguments)
    {
        Row.OwnerName = CopyString(*Data, Row.OwnerName);
        Row.FunctionName = CopyString(*Data, Row.FunctionName);
        Row.ParameterName = CopyString(*Data, Row.ParameterName);
        Row.DefaultValueText = CopyString(*Data, Row.DefaultValueText);
        Data->DefaultArguments.Add(Row);
    }
    for (auto Row : PropertyTraits)
    {
        Row.OwnerName = CopyString(*Data, Row.OwnerName);
        Row.FunctionName = CopyString(*Data, Row.FunctionName);
        Row.PropertyName = CopyString(*Data, Row.PropertyName);
        Data->PropertyTraits.Add(Row);
    }
    for (auto Row : FunctionTraits)
    {
        Row.OwnerName = CopyString(*Data, Row.OwnerName);
        Row.FunctionName = CopyString(*Data, Row.FunctionName);
        Data->FunctionTraits.Add(Row);
    }
    FMetadataRegistry& Registry = GetRegistry();
    FScopeLock Scope(&Registry.Lock);
    Registry.Modules.Add(Owner, { FName(ANSI_TO_TCHAR(ModuleName)), Data, ++Registry.Sequence, true });
    Registry.Snapshot.Reset();
}

void SoloASBridge::UnregisterScriptMetadata(const void* Owner)
{
    FMetadataRegistry& Registry = GetRegistry();
    FScopeLock Scope(&Registry.Lock);
    if (Registry.Modules.Remove(Owner) != 0)
        Registry.Snapshot.Reset();
}

TSharedRef<const FSoloASBridgeScriptMetadata, ESPMode::ThreadSafe> SoloASBridge::GetScriptMetadata()
{
    FMetadataRegistry& Registry = GetRegistry();
    FScopeLock Scope(&Registry.Lock);
    if (!Registry.Snapshot.IsValid())
    {
        auto Data = MakeShared<FSoloASBridgeScriptMetadata, ESPMode::ThreadSafe>();
        Data->Functions.Append(GeneratedScriptFunctions, GeneratedScriptFunctionCount);
        Data->DefaultArguments.Append(GeneratedScriptDefaultArguments, GeneratedScriptDefaultArgumentCount);
        Data->PropertyTraits.Append(GeneratedNativeFunctionPropertyTraits, GeneratedNativeFunctionPropertyTraitCount);
        Data->FunctionTraits.Append(GeneratedNativeFunctionTraits, GeneratedNativeFunctionTraitCount);
        TArray<FModuleMetadata> Providers;
        for (const auto& Entry : Registry.Modules)
            if (Entry.Value.bLoaded)
                Providers.Add(Entry.Value);
        Providers.Sort([](const auto& Left, const auto& Right) { return Left.Sequence < Right.Sequence; });
        for (const auto& Provider : Providers)
        {
            const auto& Source = Provider.Data;
            Data->Sources.Add(Source);
            Data->Functions.Append(Source->Functions);
            Data->DefaultArguments.Append(Source->DefaultArguments);
            Data->PropertyTraits.Append(Source->PropertyTraits);
            Data->FunctionTraits.Append(Source->FunctionTraits);
        }
        Data->Functions.StableSort([](const auto& Left, const auto& Right)
        {
            return CompareFunction(Left.OwnerName, Left.FunctionName, Right.OwnerName, Right.FunctionName) < 0;
        });
        Data->DefaultArguments.StableSort([](const auto& Left, const auto& Right)
        {
            const int32 Order = CompareFunction(Left.OwnerName, Left.FunctionName, Right.OwnerName, Right.FunctionName);
            if (Order != 0) return Order < 0;
            return Left.ParameterOrdinal < Right.ParameterOrdinal;
        });
        Data->PropertyTraits.StableSort([](const auto& Left, const auto& Right)
        {
            const int32 Order = CompareFunction(Left.OwnerName, Left.FunctionName, Right.OwnerName, Right.FunctionName);
            if (Order != 0) return Order < 0;
            if (Left.ParameterOrdinal != Right.ParameterOrdinal) return Left.ParameterOrdinal < Right.ParameterOrdinal;
            return FCStringAnsi::Strcmp(Left.PropertyName, Right.PropertyName) < 0;
        });
        Data->FunctionTraits.StableSort([](const auto& Left, const auto& Right)
        {
            return CompareFunction(Left.OwnerName, Left.FunctionName, Right.OwnerName, Right.FunctionName) < 0;
        });
        const auto FunctionKey = [](const auto& Left, const auto& Right)
        {
            return CompareFunction(Left.OwnerName, Left.FunctionName, Right.OwnerName, Right.FunctionName) == 0;
        };
        Deduplicate(Data->Functions, FunctionKey, [](const auto&, const auto&) { return true; });
        Deduplicate(Data->FunctionTraits, FunctionKey,
            [](const auto& Left, const auto& Right) { return Left.Traits == Right.Traits; });
        Deduplicate(Data->DefaultArguments, [&](const auto& Left, const auto& Right)
        {
            return FunctionKey(Left, Right) && Left.ParameterOrdinal == Right.ParameterOrdinal;
        }, [](const auto& Left, const auto& Right)
        {
            return FCStringAnsi::Strcmp(Left.ParameterName, Right.ParameterName) == 0
                && FCStringAnsi::Strcmp(Left.DefaultValueText, Right.DefaultValueText) == 0;
        });
        Deduplicate(Data->PropertyTraits, [&](const auto& Left, const auto& Right)
        {
            return FunctionKey(Left, Right) && Left.ParameterOrdinal == Right.ParameterOrdinal
                && FCStringAnsi::Strcmp(Left.PropertyName, Right.PropertyName) == 0;
        }, [](const auto& Left, const auto& Right) { return Left.Traits == Right.Traits; });
        Registry.Snapshot = Data;
    }
    return Registry.Snapshot.ToSharedRef();
}

void SoloASBridge::InitializeScriptMetadata()
{
    FMetadataRegistry& Registry = GetRegistry();
    Registry.ModulesChanged = FModuleManager::Get().OnModulesChanged().AddStatic(&ModuleChanged);
    IModularFeatures& Features = IModularFeatures::Get();
    IModularFeatures::FScopedLockModularFeatureList FeatureLock;
    Registry.FeatureRegistered = Features.OnModularFeatureRegistered().AddStatic(&FeatureRegistered);
    Registry.FeatureUnregistered = Features.OnModularFeatureUnregistered().AddStatic(&FeatureUnregistered);
    for (auto* Feature : Features.GetModularFeatureImplementations<FSoloASBridgeScriptMetadataRegistration>(
        FSoloASBridgeScriptMetadataRegistration::FeatureName()))
    {
        FeatureRegistered(FSoloASBridgeScriptMetadataRegistration::FeatureName(), Feature);
    }
}

void SoloASBridge::ShutdownScriptMetadata()
{
    FMetadataRegistry& Registry = GetRegistry();
    FModuleManager::Get().OnModulesChanged().Remove(Registry.ModulesChanged);
    Registry.ModulesChanged.Reset();
    IModularFeatures& Features = IModularFeatures::Get();
    IModularFeatures::FScopedLockModularFeatureList FeatureLock;
    Features.OnModularFeatureRegistered().Remove(Registry.FeatureRegistered);
    Features.OnModularFeatureUnregistered().Remove(Registry.FeatureUnregistered);
    Registry.FeatureRegistered.Reset();
    Registry.FeatureUnregistered.Reset();
    FScopeLock Scope(&Registry.Lock);
    Registry.Modules.Empty();
    Registry.Snapshot.Reset();
}
