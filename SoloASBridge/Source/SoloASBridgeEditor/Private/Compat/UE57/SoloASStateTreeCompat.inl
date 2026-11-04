// category=A; basis=UE-AS fork StateTreeNodeClassCache::CacheClasses script-node discovery; StateTree remains optional.
// Copyright Cuviss Studio. All Rights Reserved.
FTopLevelAssetPath StateTreeDataPath(const FStateTreeNodeClassData& Data) { return Data.GetStructPath(); }
TSharedPtr<FStateTreeNodeClassData> MakeStateTreeData(UClass* Class)
{ return MakeShared<FStateTreeNodeClassData>(TNotNull<UStruct*>(Class)); }
