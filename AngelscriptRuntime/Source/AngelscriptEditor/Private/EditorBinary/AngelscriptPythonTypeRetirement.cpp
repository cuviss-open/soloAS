// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "EditorBinary/AngelscriptPythonTypeRetirement.h"
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "IPythonScriptPlugin.h"
#include "ClassGenerator/ASClass.h"
#include "ClassGenerator/AngelscriptClassGenerator.h"
#include "EngineGlobals.h"
#include "HAL/IConsoleManager.h"
#include "UObject/GCObject.h"
#include "PyWrapperTypeRegistry.h"
#include "PyGenUtil.h"
#include "PyWrapperBase.h"
#include "PyGIL.h"
#if WITH_PYTHON
namespace
{
FPyWrapperTypeRegistry* PythonTypeRetirementRegistry = nullptr;
FDelegateHandle Initialized, Reloaded;
TAutoConsoleVariable<int32> PythonTypeRetirementEnabled(TEXT("soloas.EditorPythonTypeRetirement"), 1,
    TEXT("Release Python metadata for deleted AS classes. 0 preserves engine registry ownership."));
using FWrappedType = TSharedPtr<PyGenUtil::FGeneratedWrappedType>;
using FTypeEntry = TPair<FSoftObjectPath, FWrappedType>;
struct FTypesTag
{
    using Type = void (*)(FPyWrapperTypeRegistry&, TArray<FTypeEntry>&);
    friend Type Resolve(FTypesTag);
    template<auto Member> static void Snapshot(FPyWrapperTypeRegistry& Value, TArray<FTypeEntry>& Out)
    {
        (Value.*Member).ForEach([&Out](const auto& Pair) { Out.Emplace(Pair.Key, Pair.Value); });
    }
};
template<class Tag, auto Member> struct TSnapshotAccess
{
    friend typename Tag::Type Resolve(Tag) { return &Tag::template Snapshot<Member>; }
};
template struct TSnapshotAccess<FTypesTag, &FPyWrapperTypeRegistry::GeneratedWrappedTypes>;

struct FRemoveTag
{
    using Type = void (*)(FPyWrapperTypeRegistry&, const FTypeEntry&, const FString&);
    friend Type Resolve(FRemoveTag);
    template<auto Types, auto Classes, auto Names, auto Orphans>
    static void Remove(FPyWrapperTypeRegistry& Value, const FTypeEntry& Entry, const FString& Name)
    {
        (Value.*Orphans).Add(Entry.Value);
        (Value.*Classes).Remove(Entry.Key);
        (Value.*Types).Remove(Entry.Key);
        (Value.*Names).RemoveIf(Name, [&Entry](const FSoftObjectPath& Existing) { return Existing == Entry.Key; });
    }
};
template<auto Types, auto Classes, auto Names, auto Orphans> struct TRemoveAccess
{
    friend FRemoveTag::Type Resolve(FRemoveTag) { return &FRemoveTag::Remove<Types, Classes, Names, Orphans>; }
};
template struct TRemoveAccess<&FPyWrapperTypeRegistry::GeneratedWrappedTypes,
    &FPyWrapperTypeRegistry::PythonWrappedClasses, &FPyWrapperTypeRegistry::PythonWrappedTypeNameToUnrealTypeName,
    &FPyWrapperTypeRegistry::OrphanedWrappedTypes>;

class FRegistryFinder final : public FReferenceCollector
{
    FGCObject* Previous = nullptr;
public:
    virtual bool IsIgnoringArchetypeRef() const override { return false; }
    virtual bool IsIgnoringTransient() const override { return false; }
    virtual void HandleObjectReference(UObject*&, const UObject*, const FProperty*) override
    {
        FGCObject* Current = FGCObject::GGCObjectReferencer->GetCurrentlySerializingObject();
        if (Current == Previous) return;
        Previous = Current;
        if (Current && Current->GetReferencerName() == TEXT("FPyWrapperTypeRegistry"))
            PythonTypeRetirementRegistry = static_cast<FPyWrapperTypeRegistry*>(Current);
    }
};
void FindRegistry()
{
    // Bootstrap once through the public reference collector; no addresses, offsets or vtable edits.
    if (PythonTypeRetirementRegistry || !FGCObject::GGCObjectReferencer) return;
    FRegistryFinder Finder;
    UGCObjectReferencer::AddReferencedObjects(FGCObject::GGCObjectReferencer, Finder);
    if (!PythonTypeRetirementRegistry) UE_LOG(LogTemp, Error, TEXT("ASR-PYTHON-REGISTRY-UNAVAILABLE: deleted AS types cannot retire."));
}
PyObject* DeletedArgs(PyObject*, PyObject*, void*) { PyErr_SetString(PyExc_ReferenceError, "AS class was deleted"); return nullptr; }
PyObject* DeletedNoArgs(PyObject*, void*) { PyErr_SetString(PyExc_ReferenceError, "AS class was deleted"); return nullptr; }
PyObject* DeletedKeywords(PyObject*, PyObject*, PyObject*, void*) { PyErr_SetString(PyExc_ReferenceError, "AS class was deleted"); return nullptr; }
PyObject* DeletedGet(PyObject*, void*) { PyErr_SetString(PyExc_ReferenceError, "AS class was deleted"); return nullptr; }
int DeletedSet(PyObject*, PyObject*, void*) { PyErr_SetString(PyExc_ReferenceError, "AS class was deleted"); return -1; }
PyObject* DeletedConstant(PyTypeObject*, const void*) { PyErr_SetString(PyExc_ReferenceError, "AS class was deleted"); return nullptr; }
void DisableMethod(FPyMethodWithClosureDef& Method)
{
    if (!Method.MethodName) return;
    Method.MethodCallback = (Method.MethodFlags & METH_KEYWORDS) ? PyCFunctionWithClosureCast(&DeletedKeywords)
        : (Method.MethodFlags & METH_NOARGS) ? PyCFunctionWithClosureCast(&DeletedNoArgs) : &DeletedArgs;
}
void DisableDescriptors(PyGenUtil::FGeneratedWrappedClassType& Type)
{
    for (auto& Method : Type.Methods.PyMethods) DisableMethod(Method);
    for (const auto& Method : Type.DynamicMethods) DisableMethod(Method->PyMethod);
    for (auto& GetSet : Type.GetSets.PyGetSets)
        if (GetSet.name) { if (GetSet.get) GetSet.get = &DeletedGet; if (GetSet.set) GetSet.set = &DeletedSet; }
    for (auto& Constant : Type.Constants.PyConstants)
        if (Constant.ConstantName) Constant.ConstantGetter = &DeletedConstant;
    for (const auto& Constant : Type.DynamicConstants) Constant->PyConstant.ConstantGetter = &DeletedConstant;
}
void SetUtf8(PyGenUtil::FUTF8Buffer& Buffer, const FString& Value)
{
    FTCHARToUTF8 Converted(*Value);
    Buffer.Append(Converted.Get(), Converted.Length() + 1);
}
void RetireDeletedTypes(bool bFullReload)
{
    if (!bFullReload) return;
    IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
    if (PythonTypeRetirementEnabled.GetValueOnGameThread() == 0 || !Python || !Python->IsPythonInitialized()) return;
    FindRegistry();
    if (!PythonTypeRetirementRegistry) return;
    TArray<FTypeEntry> Types;
    Resolve(FTypesTag{})(*PythonTypeRetirementRegistry, Types);
    FPyScopedGIL Gil;
    for (const FTypeEntry& Entry : Types)
    {
        const FWrappedType& Type = Entry.Value;
        const UASClass* Source = Type && Type->MetaData ? Cast<UASClass>(Type->MetaData->GetMetaType()) : nullptr;
        if (!Source || Source->ScriptTypePtr || !Source->HasAnyClassFlags(CLASS_Hidden)
            || Source->HasAnyClassFlags(CLASS_NewerVersionExists)) continue;
        const FString Name = UTF8_TO_TCHAR(Type->TypeName.GetData());
        // Use the public virtual reset phases and retain a deleted stub like engine asset deletion.
        // Retain the generated type storage: existing Python references receive a deleted stub.
        DisableDescriptors(static_cast<PyGenUtil::FGeneratedWrappedClassType&>(*Type));
        Type->Reset_CleansePyType();
        Type->Reset_CleanseSelf();
        SetUtf8(Type->TypeName, Name + TEXT("_DELETED"));
        SetUtf8(Type->TypeDoc, Source->GetPathName() + TEXT(" was deleted!"));
        // Class method/getset tables are already finalized; retain their backing storage.
        Type->PyType.tp_name = Type->TypeName.GetData();
        Type->PyType.tp_doc = Type->TypeDoc.GetData();
        PyType_Modified(&Type->PyType);
        PyObject* Doc = PyUnicode_FromString(Type->PyType.tp_doc);
        const bool Updated = Doc && PyDict_SetItemString(Type->PyType.tp_dict, "__doc__", Doc) == 0;
        Py_XDECREF(Doc);
        Type->FinalizedState = PyGenUtil::FGeneratedWrappedType::EFinalizedState::Finalized;
        Resolve(FRemoveTag{})(*PythonTypeRetirementRegistry, Entry, Name);
        if (!Updated || Type->MetaData)
            UE_LOG(LogTemp, Error, TEXT("ASR-PYTHON-TYPE-RETIREMENT-FAILED: %s"), *Name);
    }
}
}
#endif
namespace AngelscriptPythonTypeRetirement
{
void Initialize()
{
#if WITH_PYTHON
    if (!GIsEditor || IsRunningCommandlet()) return;
    if (IPythonScriptPlugin* Python = IPythonScriptPlugin::Get())
    {
        if (Python->IsPythonInitialized()) FindRegistry();
        else Initialized = Python->OnPythonInitialized().AddStatic(&FindRegistry);
    }
    Reloaded = FAngelscriptClassGenerator::OnPostReload.AddStatic(&RetireDeletedTypes);
#endif
}
void Shutdown()
{
#if WITH_PYTHON
    FAngelscriptClassGenerator::OnPostReload.Remove(Reloaded);
    if (IPythonScriptPlugin* Python = IPythonScriptPlugin::Get()) Python->OnPythonInitialized().Remove(Initialized);
    PythonTypeRetirementRegistry = nullptr;
#endif
}
}
#else
namespace AngelscriptPythonTypeRetirement { void Initialize() {} void Shutdown() {} }
#endif
