// Copyright Cuviss Studio. All Rights Reserved.
// Exact SDK ABI for the existing PropertyEditor lookup and linker hooks.
using FExperienceName = const FString&;
using FSetLinkerFunction = void (*)(UObject*, FLinkerLoad*, int32, bool);
constexpr const char* EnumLookupWindowsSymbol = "?TryFindTypeSlow@UClass@@SAPEAVUField@@PEAV1@AEBVFString@@W4EFindFirstObjectOptions@@@Z";
constexpr const char* EnumLookupMacSymbol = "__ZN6UClass15TryFindTypeSlowEPS_RK7FString23EFindFirstObjectOptions";
constexpr const char* SetLinkerWindowsSymbol = "?SetLinker@UObject@@QEAAXPEAVFLinkerLoad@@H_N@Z";
constexpr const char* SetLinkerMacSymbol = "__ZN7UObject9SetLinkerEP11FLinkerLoadib";
void InvokeSetLinker(FSetLinkerFunction Function, UObject* Object, FLinkerLoad* Linker, int32 Index,
    bool Detach, bool Silence) { Function(Object, Linker, Index, Detach); }
