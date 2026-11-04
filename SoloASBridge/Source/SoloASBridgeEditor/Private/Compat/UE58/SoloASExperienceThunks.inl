// Copyright Cuviss Studio. All Rights Reserved.
void HookSetLinkerImport(UObject* Object, FLinkerLoad* Linker, int32 Index, bool Detach, bool Silence)
{ HookSetLinkerImportImpl(Object, Linker, Index, Detach, Silence); }
void HookSetLinkerEntry(UObject* Object, FLinkerLoad* Linker, int32 Index, bool Detach, bool Silence)
{ HookSetLinkerEntryImpl(Object, Linker, Index, Detach, Silence); }
