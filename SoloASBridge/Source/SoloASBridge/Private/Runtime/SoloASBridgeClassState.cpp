#include "Runtime/SoloASBridgeClassState.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeRWLock.h"
#include "Templates/Atomic.h"
#include "UObject/Class.h"
#include "UObject/ObjectKey.h"

namespace
{
struct FClassStateRecord
{
    const void* Owner = nullptr;
    void* State = nullptr;
};

struct FClassStateRegistry
{
    FRWLock Lock;
    TMap<FObjectKey, FClassStateRecord> Records;
    TAtomic<bool> bHasRecords{false};
};

FClassStateRegistry& GetClassStateRegistry()
{
    static FClassStateRegistry Registry;
    return Registry;
}
}

namespace SoloASBridge
{
void* GetClassState(const UClass* Class, const void* Owner)
{
    if (Class == nullptr || Owner == nullptr)
    {
        return nullptr;
    }

    FClassStateRegistry& Registry = GetClassStateRegistry();
    if (!Registry.bHasRecords.Load()) return nullptr;
    FReadScopeLock ScopeLock(Registry.Lock);
    const FClassStateRecord* Record = Registry.Records.Find(FObjectKey(Class));
    return Record != nullptr && Record->Owner == Owner
        ? Record->State
        : nullptr;
}

bool SetClassState(const UClass* Class, const void* Owner, void* State)
{
    if (Class == nullptr || Owner == nullptr || State == nullptr)
    {
        return false;
    }

    FClassStateRegistry& Registry = GetClassStateRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    FClassStateRecord* Record = Registry.Records.Find(FObjectKey(Class));
    if (Record != nullptr)
    {
        if (Record->Owner != Owner)
        {
            return false;
        }

        Record->State = State;
        return true;
    }

    Registry.Records.Add(FObjectKey(Class), { Owner, State });
    Registry.bHasRecords.Store(true);
    return true;
}

bool ClearClassState(
    const UClass* Class,
    const void* Owner,
    const void* ExpectedState
)
{
    if (Class == nullptr || Owner == nullptr || ExpectedState == nullptr)
    {
        return false;
    }

    FClassStateRegistry& Registry = GetClassStateRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    const FObjectKey Key(Class);
    const FClassStateRecord* Record = Registry.Records.Find(Key);
    if (
        Record == nullptr
        || Record->Owner != Owner
        || Record->State != ExpectedState
    )
    {
        return false;
    }

    const bool bRemoved = Registry.Records.Remove(Key) == 1;
    Registry.bHasRecords.Store(!Registry.Records.IsEmpty());
    return bRemoved;
}

void ClearClassStates(const void* Owner)
{
    if (Owner == nullptr)
    {
        return;
    }

    FClassStateRegistry& Registry = GetClassStateRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    for (auto Iterator = Registry.Records.CreateIterator(); Iterator; ++Iterator)
    {
        if (Iterator.Value().Owner == Owner)
        {
            Iterator.RemoveCurrent();
        }
    }
    Registry.bHasRecords.Store(!Registry.Records.IsEmpty());
}
}

namespace SoloASBridge::Private
{
void PruneClassStates()
{
    FClassStateRegistry& Registry = GetClassStateRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    for (auto Iterator = Registry.Records.CreateIterator(); Iterator; ++Iterator)
    {
        if (Iterator.Key().ResolveObjectPtr() == nullptr)
        {
            Iterator.RemoveCurrent();
        }
    }
    Registry.bHasRecords.Store(!Registry.Records.IsEmpty());
}

void ClearAllClassStates()
{
    FClassStateRegistry& Registry = GetClassStateRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    Registry.Records.Reset();
    Registry.bHasRecords.Store(false);
}
}
