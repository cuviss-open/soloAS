#include "Runtime/SoloASObjectPropertyInitialization.h"

#include "Shared/SoloASPrivateAccess.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectThreadContext.h"
#include "UObject/UnrealType.h"

namespace
{
bool MatchesConstructedStorage(const FProperty& Property, const void* Data, const UObject* Object)
{
    const UClass* OwnerClass = Cast<UClass>(Property.GetOwnerStruct());
    return OwnerClass != nullptr && Object != nullptr
        && !Object->HasAnyInternalFlags(EInternalObjectFlags::PendingConstruction)
        && Object->IsA(OwnerClass)
        && Property.ContainerPtrToValuePtr<void>(Object) == Data;
}

// Translation-unit-only adapter: no engine references or binding details escape.
struct FObjectInitializerAccess final
{
    static bool ContainsConstructedSubobjectStorage(
        const FObjectInitializer& Initializer, const FProperty& Property, const void* Data);
};
}

namespace SoloASBridge::PrivateAccess
{
struct FComponentInitializersTag
{
    using Type = bool (*)(const FObjectInitializer&, const FProperty&, const void*);
    friend Type Resolve(FComponentInitializersTag) noexcept;

    template<auto Member>
    static bool Invoke(const FObjectInitializer& Initializer, const FProperty& Property, const void* Data)
    {
        for (const auto& Component : (Initializer.*Member).SubobjectInits)
        {
            if (MatchesConstructedStorage(Property, Data, Component.Subobject))
            {
                return true;
            }
        }
        return false;
    }
};
template struct TBoundOperation<FComponentInitializersTag, &FObjectInitializer::ComponentInits>;
}

namespace
{
bool FObjectInitializerAccess::ContainsConstructedSubobjectStorage(
    const FObjectInitializer& Initializer, const FProperty& Property, const void* Data)
{
    using namespace SoloASBridge::PrivateAccess;
    return CallStatic<FComponentInitializersTag>(Initializer, Property, Data);
}
}

bool SoloASBridge::IsConstructedObjectPropertyStorage(const FProperty& Property, const void* Data)
{
    const UClass* OwnerClass = Cast<UClass>(Property.GetOwnerStruct());
    const FObjectInitializer* Initializer = FUObjectThreadContext::Get().TopInitializer();
    if (Data == nullptr || OwnerClass == nullptr || Initializer == nullptr
        || Property.HasAnyPropertyFlags(CPF_Parm) || Property.ArrayDim != 1)
    {
        return false;
    }
    if (MatchesConstructedStorage(Property, Data, Initializer->GetObj()))
    {
        return true;
    }
    return FObjectInitializerAccess::ContainsConstructedSubobjectStorage(*Initializer, Property, Data);
}
