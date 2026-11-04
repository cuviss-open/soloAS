// category=A; basis=UE-AS fork StateTreeNodeClassCache::CacheClasses script-node discovery; StateTree remains optional.
// Copyright Cuviss Studio. All Rights Reserved.
// Match only already loaded identities; GetStruct() can load Blueprint packages.
struct FStateTreeLoadedStruct
{
    using Type = FTopLevelAssetPath (*)(const FStateTreeNodeClassData&);
    friend Type Resolve(FStateTreeLoadedStruct) noexcept;
    template<auto Member> static FTopLevelAssetPath Invoke(const FStateTreeNodeClassData& Data)
    {
        const UStruct* Struct = (Data.*Member).Get();
        return Struct ? Struct->GetStructPathName() : FTopLevelAssetPath();
    }
};
template struct TBoundOperation<FStateTreeLoadedStruct, &FStateTreeNodeClassData::Struct>;
FTopLevelAssetPath StateTreeDataPath(const FStateTreeNodeClassData& Data)
{ return CallStatic<FStateTreeLoadedStruct>(Data); }
TSharedPtr<FStateTreeNodeClassData> MakeStateTreeData(UClass* Class)
{ return MakeShared<FStateTreeNodeClassData>(Class); }
