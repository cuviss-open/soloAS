// Copyright Cuviss Studio. All Rights Reserved.
void HookSetLinkerImport(UObject* Object, FLinkerLoad* Linker, int32 Index, bool Detach)
{ HookSetLinkerImportImpl(Object, Linker, Index, Detach, false); }
void HookSetLinkerEntry(UObject* Object, FLinkerLoad* Linker, int32 Index, bool Detach)
{ HookSetLinkerEntryImpl(Object, Linker, Index, Detach, false); }
