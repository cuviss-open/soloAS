// Copyright Cuviss Studio. All Rights Reserved.
#include "Editor/SoloASBridgeStateTree.h"
#include "StateTreeNodeClassCache.h"
#include "UObject/Class.h"
#include <functional>
#include <type_traits>
#include <utility>

namespace SoloASBridge::PrivateAccess
{
template<class Tag, auto Member>
struct TBoundOperation final
{
    friend typename Tag::Type Resolve(Tag) noexcept
    {
        return &Tag::template Invoke<Member>;
    }
};

template<class Tag, class... Args>
requires (std::is_pointer_v<typename Tag::Type>
    && std::is_function_v<std::remove_pointer_t<typename Tag::Type>>
    && std::is_invocable_v<typename Tag::Type, Args&&...>)
decltype(auto) CallStatic(Args&&... Arguments)
    noexcept(std::is_nothrow_invocable_v<typename Tag::Type, Args&&...>)
{
    return std::invoke(Resolve(Tag{}), std::forward<Args>(Arguments)...);
}

struct FStateTreeScriptClasses
{
    using Type = void (*)(FStateTreeNodeClassCache&, const TArray<UClass*>&, const TSet<FTopLevelAssetPath>&);
    friend Type Resolve(FStateTreeScriptClasses) noexcept;
    template<auto Member>
    static void Invoke(FStateTreeNodeClassCache& Cache, const TArray<UClass*>& Classes,
        const TSet<FTopLevelAssetPath>& PreviousScriptPaths)
    {
        for (auto& Root : Cache.*Member)
        {
            const UClass* Base = Cast<UClass>(Root.BaseStruct.Get());
            if (!Base) continue;
            if (!Root.bUpdated && !Classes.ContainsByPredicate([Base](UClass* Class)
            {
                return IsValid(Class) && Class->IsChildOf(Base);
            })) continue;
            // Public lazy fill preserves engine native/Blueprint discovery before compensation.
            TArray<TSharedPtr<FStateTreeNodeClassData>> NativeData;
            Cache.GetClasses(Base, NativeData);
            Root.ClassData.RemoveAll([&](const auto& Data)
            {
                return Data.IsValid() && PreviousScriptPaths.Contains(Data->GetStructPath());
            });
            TSet<FTopLevelAssetPath> Seen;
            for (const auto& Data : Root.ClassData)
                if (Data.IsValid()) Seen.Add(Data->GetStructPath());
            for (UClass* Class : Classes)
            {
                if (!IsValid(Class) || !Class->IsChildOf(Base)
                    || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists
                        | CLASS_Hidden | CLASS_HideDropDown) || Class->HasMetaData(TEXT("Hidden"))) continue;
                if (!Seen.Contains(Class->GetClassPathName()))
                {
                    Root.ClassData.Add(MakeShared<FStateTreeNodeClassData>(TNotNull<UStruct*>(Class)));
                    Seen.Add(Class->GetClassPathName());
                }
            }
        }
    }
};
template struct TBoundOperation<FStateTreeScriptClasses, &FStateTreeNodeClassCache::RootClasses>;
}

void SoloASBridge::Editor::SynchronizeStateTreeScriptClasses(FStateTreeNodeClassCache& Cache,
    const TArray<UClass*>& Classes, const TSet<FTopLevelAssetPath>& PreviousScriptPaths)
{
    check(IsInGameThread());
    PrivateAccess::CallStatic<PrivateAccess::FStateTreeScriptClasses>(Cache, Classes, PreviousScriptPaths);
}
