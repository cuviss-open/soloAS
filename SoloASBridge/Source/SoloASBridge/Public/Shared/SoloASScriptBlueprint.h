#pragma once

#include "CoreMinimal.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#if WITH_EDITOR
#include "UObject/ObjectKey.h"
#endif

#include "SoloASScriptBlueprint.generated.h"

class FReferenceCollector;
class FBlueprintActionDatabaseRegistrar;
#if WITH_EDITOR
class FSoloASBlueprintDefaultObjectScope
{
public:
    virtual ~FSoloASBlueprintDefaultObjectScope() = default;
    virtual void OnDefaultCreated(UClass* Class, UObject* Object) = 0;
};
#endif

UCLASS()
class SOLOASBRIDGE_API USoloASScriptBlueprint final : public UBlueprint
{
    GENERATED_BODY()

public:
#if WITH_EDITOR
    virtual void GetInstanceActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
    virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
    virtual EBlueprintCompileOptions GetDefaultCompileOptions() const override;
    // True requires the whole exact current metadata/CDO group unreachable.
    bool CanOrderTerminalMetadataFinish(bool bRequireUnreachable) const;
    virtual void BeginDestroy() override;
    virtual bool IsReadyForFinishDestroy() override;
    virtual void FinishDestroy() override;
#endif

private:
#if WITH_EDITOR
    // Exact serial identities only; these dependencies never retain GC objects.
    TArray<FObjectKey> FinishDestroyDependencies;
    bool bFinishDestroyGateArmed = false;
    bool bFinishDestroyGateBlockedLogged = false;
    bool bFinishDestroyGateReadyLogged = false;
#endif
};

UCLASS()
class SOLOASBRIDGE_API USoloASScriptBlueprintGeneratedClass final
    : public UBlueprintGeneratedClass
{
    GENERATED_BODY()

public:
    virtual void Bind() override;
    virtual bool IsFunctionImplementedInScript(FName Name) const override;
    virtual void GetLifetimeBlueprintReplicationList(TArray<FLifetimeProperty>& Out) const override;
    virtual void DestroyPersistentUberGraphFrame(UObject* Object, bool bSkipSuperClass = false) const override;
    virtual UObject* CreateDefaultObject() override;
    virtual void PostLoadDefaultObject(UObject* Object) override;
#if WITH_EDITOR
    virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
    virtual void PreSave(FObjectPreSaveContext SaveContext) override;
    bool CanOrderTerminalMetadataFinish(bool bRequireUnreachable) const;
    virtual void BeginDestroy() override;
    virtual bool IsReadyForFinishDestroy() override;
    virtual void FinishDestroy() override;
    bool AttachDefaultObjectScope(FSoloASBlueprintDefaultObjectScope* Scope);
    void DetachDefaultObjectScope(FSoloASBlueprintDefaultObjectScope* Scope);
#endif
    virtual void InitPropertiesFromCustomList(
        uint8* DataPtr,
        const uint8* DefaultDataPtr
    ) override;

    static void AddReferencedObjectsInScriptUbergraphFrames(
        UObject* Instance,
        FReferenceCollector& Collector
    );

private:
#if WITH_EDITOR
    // Saved before class teardown clears its current default-object pointer.
    FObjectKey FinishDestroyBlueprintKey;
    FObjectKey FinishDestroyDefaultKey;
    TArray<FObjectKey> FinishDestroyReceiverDependencies;
    bool bFinishDestroyGateArmed = false;
    bool bFinishDestroyGateBlockedLogged = false;
    bool bFinishDestroyGateReadyLogged = false;

    // Compiler-owned, non-persistent and deliberately excluded from duplication.
    FSoloASBlueprintDefaultObjectScope* DefaultObjectScope = nullptr;
#endif
    UPROPERTY()
    TArray<FName> NativePropertyOverrideIds;

    UPROPERTY()
    bool bNativePropertyOverrideIdsValid = false;
};

namespace SoloASBridge
{
DECLARE_MULTICAST_DELEGATE_OneParam(FOnScriptClassRegistered, UClass*);
using FScriptReplicationListProvider = TFunction<bool(const UClass*, TArray<FLifetimeProperty>&)>;
/** Runtime supplies the script list; Bridge owns dispatch for real Blueprint descendants. */
SOLOASBRIDGE_API void SetScriptReplicationListProvider(FScriptReplicationListProvider Provider);

/**
 * Registers a non-native AngelScript class identity with the Bridge.
 * The first successful call locks the exact native ScriptMetaClass identity.
 */
SOLOASBRIDGE_API bool RegisterScriptClass(
    UClass* ScriptClass,
    const UClass* ScriptMetaClass
);

/** Registers candidate carrier identity without publishing paths or registration events. */
SOLOASBRIDGE_API bool PrepareScriptClassRegistration(
    UClass* ScriptClass,
    const UClass* ScriptMetaClass
);

/** Returns whether Class is an exact registered AngelScript class identity. */
SOLOASBRIDGE_API bool IsRegisteredScriptClass(const UClass* Class);

/** Returns whether Class or one of its ancestors is a registered script class. */
SOLOASBRIDGE_API bool HasRegisteredScriptClassInAncestry(const UClass* Class);

/** Copies live published script class identities in stable path order. */
SOLOASBRIDGE_API void GetRegisteredScriptClasses(TArray<UClass*>& OutClasses);

/** Notifies editor integration after a new script class identity is registered. */
SOLOASBRIDGE_API FOnScriptClassRegistered& OnScriptClassRegistered();
}
