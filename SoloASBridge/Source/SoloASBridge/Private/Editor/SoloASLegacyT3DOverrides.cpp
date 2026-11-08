// Copyright Cuviss Studio. All Rights Reserved.
#include "CoreMinimal.h"
#include "SoloASEngineCompat.h"

// SOLOAS-COMPAT-BEGIN(<5.6) Replay editor T3D override annotations using the legacy override tree [test: SoloAS.EngineCompat.T3DImportBoundary] [id: CS.Compatibility.Bridge.LegacyT3DOverrides]
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC) && SOLOAS_UE_BEFORE(5, 6)
#include "Editor/SoloASEditorEntryHook.h"
#include "Editor/SoloASEditorObjectImportProperties.h"
#include "Factories.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FeedbackContext.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Serialization/ArchiveSerializedPropertyChain.h"
#include "UObject/OverridableManager.h"
#include "UObject/OverriddenPropertySet.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"

namespace SoloASBridge::Private
{
namespace
{
TAutoConsoleVariable<int32> CVarLegacyT3D(TEXT("soloas.EditorLegacyT3DOverrides"), 1,
    TEXT("Import supported UE 5.6+ override-state T3D in UE 5.4/5.5 editor text imports."));
FSoloASEditorEntryHook ImportHook;
FSoloASEditorEntryHook FactoryHook;
using FImport = const TCHAR* (*)(FImportObjectParams&);
using FProcess = void (*)(FCustomizableTextObjectFactory*, UObject*, EObjectFlags, const TCHAR*);

struct FOverrideLine
{
    TArray<FName> Objects;
    FString Property;
    FString ExportPath;
    FString Value;
    EOverriddenPropertyOperation Operation = EOverriddenPropertyOperation::None;
    bool bObject = false;
    bool bMapOnly = false;
    bool bShadow = false;
};

bool ReadOperation(const FString& Name, EOverriddenPropertyOperation& Operation, bool& Shadow)
{
    Shadow = Name == TEXT("SubObjectsShadowing");
    if (Shadow || Name == TEXT("None")) Operation = EOverriddenPropertyOperation::None;
    else if (Name == TEXT("Modified")) Operation = EOverriddenPropertyOperation::Modified;
    else if (Name == TEXT("Replace")) Operation = EOverriddenPropertyOperation::Replace;
    else if (Name == TEXT("Add")) Operation = EOverriddenPropertyOperation::Add;
    else if (Name == TEXT("Remove")) Operation = EOverriddenPropertyOperation::Remove;
    else return false;
    return true;
}

bool Reject(FFeedbackContext* Warn, const FString& Detail)
{
    if (Warn) Warn->Logf(ELogVerbosity::Error, TEXT("SOLOAS-COMPAT: legacy T3D override import rejected: %s"), *Detail);
    return false;
}

// Only inspect the assignment's left side. Strings may legitimately contain protocol words.
bool HasPropertyOperation(const FString& Line)
{
    int32 Equals, Open;
    return Line.FindChar(TEXT('='), Equals) && Line.FindChar(TEXT('<'), Open) && Open < Equals;
}

bool Parse(const TCHAR* Text, FString& NativeText, TArray<FOverrideLine>& Records, FFeedbackContext* Warn)
{
    TArray<FName> Objects;
    const TCHAR* Cursor = Text;
    FString Line;
    while (FParse::Line(&Cursor, Line))
    {
        FString Trimmed = Line.TrimStartAndEnd();
        const TCHAR* Header = *Trimmed;
        if (FParse::Command(&Header, TEXT("BEGIN")) && FParse::Command(&Header, TEXT("OBJECT")))
        {
            FName Name;
            if (!FParse::Value(Header, TEXT("Name="), Name)) return Reject(Warn, TEXT("object without a name"));
            Objects.Add(Name);
            FString Operation;
            FString ExportPath;
            FParse::Value(Header, TEXT("ExportPath="), ExportPath);
            if (FParse::Value(Header, TEXT("OverriddenOperation="), Operation))
            {
                FOverrideLine Record;
                Record.Objects = Objects; Record.bObject = true; Record.ExportPath = ExportPath;
                if (!ReadOperation(Operation, Record.Operation, Record.bShadow) || Record.bShadow)
                    return Reject(Warn, FString::Printf(TEXT("unsupported object operation %s"), *Operation));
                Records.Add(MoveTemp(Record));
                const int32 Start = Line.Find(TEXT("OverriddenOperation="), ESearchCase::IgnoreCase);
                int32 End = Start;
                while (End < Line.Len() && !FChar::IsWhitespace(Line[End])) ++End;
                Line.RemoveAt(Start, End - Start);
            }
            else if (!ExportPath.IsEmpty())
            {
                FOverrideLine Record;
                Record.Objects = Objects; Record.ExportPath = ExportPath; Record.bMapOnly = true;
                Records.Add(MoveTemp(Record));
            }
        }
        else
        {
            Header = *Trimmed;
            if (FParse::Command(&Header, TEXT("END")) && FParse::Command(&Header, TEXT("OBJECT")))
            {
                if (Objects.IsEmpty()) return Reject(Warn, TEXT("unbalanced object block"));
                Objects.Pop();
            }
            else if (Trimmed.StartsWith(TEXT("T3DExportPath=")))
            {
                FOverrideLine Record;
                Record.Objects = Objects; Record.bMapOnly = true;
                FParse::Value(*Trimmed, TEXT("T3DExportPath="), Record.ExportPath);
                Records.Add(MoveTemp(Record));
                Line.Reset();
            }
            else if (Trimmed.StartsWith(TEXT("T3DOverrides=")))
            {
                FOverrideLine Record;
                Record.Objects = Objects; Record.bObject = true;
                if (!ReadOperation(Trimmed.Mid(13), Record.Operation, Record.bShadow) || Record.bShadow)
                    return Reject(Warn, TEXT("unsupported root operation"));
                Records.Add(MoveTemp(Record));
                Line.Reset();
            }
            else if (HasPropertyOperation(Trimmed))
            {
                int32 Open = Trimmed.Find(TEXT("<"));
                int32 Close = Trimmed.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open);
                int32 Equals = Trimmed.Find(TEXT("="));
                if (Close == INDEX_NONE || Close > Equals) return Reject(Warn, TEXT("malformed property operation"));
                FOverrideLine Record;
                Record.Objects = Objects;
                Record.Property = Trimmed.Left(Open).TrimStartAndEnd();
                Record.Value = Trimmed.Mid(Equals + 1).TrimStartAndEnd();
                if (!ReadOperation(Trimmed.Mid(Open + 1, Close - Open - 1), Record.Operation, Record.bShadow))
                    return Reject(Warn, TEXT("operation has no legacy representation (including Externalized)"));
                Records.Add(MoveTemp(Record));
                Line.Reset();
            }
            else if (Trimmed.StartsWith(TEXT("SubObjectsShadow")) || Trimmed.StartsWith(TEXT("OverriddenOperation=")))
                return Reject(Warn, TEXT("unsupported standalone override directive"));
        }
        NativeText += Line;
        NativeText += TEXT("\r\n");
    }
    return Objects.IsEmpty() || Reject(Warn, TEXT("unterminated object block"));
}

UObject* FindOwner(UObject* Root, const TArray<FName>& Objects)
{
    UObject* Owner = Root;
    for (FName Name : Objects)
    {
        Owner = Owner ? StaticFindObjectFast(UObject::StaticClass(), Owner, Name) : nullptr;
        if (!Owner) break;
    }
    return Owner;
}

// Replay annotations bypass the native DefinedProperties list, so apply its reference mapping
// to the actual replayed value, including export paths of newly constructed clipboard objects.
void RemapValue(FProperty* Property, void* Data, const TMap<FSoftObjectPath, UObject*>& Remapper)
{
    if (auto* Soft = CastField<FSoftObjectProperty>(Property))
    {
        if (UObject* const* Target = Remapper.Find(Soft->GetPropertyValue(Data).ToSoftObjectPath()))
            if (*Target && (*Target)->IsA(Soft->PropertyClass)) Soft->SetPropertyValue(Data, FSoftObjectPtr(*Target));
    }
    else if (auto* Object = CastField<FObjectPropertyBase>(Property))
    {
        if (UObject* Value = Object->GetObjectPropertyValue(Data))
            if (UObject* const* Target = Remapper.Find(FSoftObjectPath(Value)))
                if (*Target && (*Target)->IsA(Object->PropertyClass)) Object->SetObjectPropertyValue(Data, *Target);
    }
    else if (auto* Struct = CastField<FStructProperty>(Property))
    {
        if (Struct->Struct == TBaseStructure<FSoftObjectPath>::Get())
        {
            auto* Path = static_cast<FSoftObjectPath*>(Data);
            if (UObject* const* Target = Remapper.Find(*Path)) *Path = FSoftObjectPath(*Target);
        }
        else for (TFieldIterator<FProperty> Field(Struct->Struct); Field; ++Field)
            for (int32 Index = 0; Index < Field->ArrayDim; ++Index)
                RemapValue(*Field, Field->ContainerPtrToValuePtr<void>(Data, Index), Remapper);
    }
    else if (auto* Array = CastField<FArrayProperty>(Property))
    {
        FScriptArrayHelper Values(Array, Data);
        for (int32 Index = 0; Index < Values.Num(); ++Index) RemapValue(Array->Inner, Values.GetRawPtr(Index), Remapper);
    }
    else if (auto* Map = CastField<FMapProperty>(Property))
    {
        FScriptMapHelper Values(Map, Data);
        for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index) if (Values.IsValidIndex(Index))
        {
            RemapValue(Map->KeyProp, Values.GetKeyPtr(Index), Remapper);
            RemapValue(Map->ValueProp, Values.GetValuePtr(Index), Remapper);
        }
        Values.Rehash();
    }
    else if (auto* Set = CastField<FSetProperty>(Property))
    {
        FScriptSetHelper Values(Set, Data);
        for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index) if (Values.IsValidIndex(Index))
            RemapValue(Set->ElementProp, Values.GetElementPtr(Index), Remapper);
        Values.Rehash();
    }
}

const TCHAR* ImportValue(FProperty* Property, const TCHAR* Text, void* Data, UObject* Owner,
    int32 Flags, FFeedbackContext* Warn, const TMap<FSoftObjectPath, UObject*>& Remapper)
{
    // Resolve export paths before native lookup: the source object may not exist on this machine.
    if (auto* Object = CastField<FObjectPropertyBase>(Property))
    {
        FSoftObjectPath Path;
        const TCHAR* Cursor = Text;
        if (Path.ImportTextItem(Cursor, Flags, Owner, nullptr))
            if (UObject* const* Target = Remapper.Find(Path))
                if (*Target && (*Target)->IsA(Object->PropertyClass))
                {
                    if (auto* Soft = CastField<FSoftObjectProperty>(Property)) Soft->SetPropertyValue(Data, FSoftObjectPtr(*Target));
                    else Object->SetObjectPropertyValue(Data, *Target);
                    return Cursor;
                }
    }
    const TCHAR* End = Property->ImportText_Direct(Text, Data, Owner, Flags, Warn);
    if (End) RemapValue(Property, Data, Remapper);
    return End;
}

FOverriddenPropertySet* RestoreObjectState(UObject& Object, EOverriddenPropertyOperation Operation, bool NeedsInstantiation)
{
#if SOLOAS_UE_BEFORE(5, 5)
    FOverriddenPropertySet* Set = &FOverridableManager::Get().SetOverriddenProperties(Object, Operation);
    Set->bNeedsSubobjectTemplateInstantiation = NeedsInstantiation;
    return Set;
#else
    return FOverridableManager::Get().SetOverriddenProperties(Object, Operation, NeedsInstantiation);
#endif
}

bool Replay(UObject& Root, const FOverrideLine& Record, int32 PortFlags, FFeedbackContext* Warn, const TMap<FSoftObjectPath, UObject*>& Remapper)
{
    if (Record.bMapOnly) return true;
    UObject* Owner = FindOwner(&Root, Record.Objects);
    if (!Owner) return Reject(Warn, TEXT("imported subobject could not be resolved"));
    auto& Manager = FOverridableManager::Get();
    if (Record.bObject)
    {
        RestoreObjectState(*Owner, Record.Operation, true);
        return true;
    }
    // Shadow references only collect subobjects in the new protocol; they never assign a value.
    if (Record.bShadow) return true;
    if (!Manager.GetOverriddenProperties(*Owner)) RestoreObjectState(*Owner, EOverriddenPropertyOperation::None, false);
    FOverriddenPropertySet* Set = Manager.GetOverriddenProperties(*Owner);
    if (!Set) return Reject(Warn, TEXT("override state could not be enabled"));
    TArray<FString> Parts;
    Record.Property.ParseIntoArray(Parts, TEXT("."));
    UStruct* Struct = Owner->GetClass();
    void* Container = Owner;
    FArchiveSerializedPropertyChain Chain;
    FProperty* Property = nullptr;
    int32 ArrayIndex = 0;
    for (int32 Index = 0; Index < Parts.Num(); ++Index)
    {
        FString FieldName = Parts[Index];
        ArrayIndex = 0;
        int32 Open;
        if (FieldName.FindChar(TEXT('('), Open))
        {
            if (!FieldName.EndsWith(TEXT(")"))) return Reject(Warn, TEXT("malformed static array index"));
            const FString IndexText = FieldName.Mid(Open + 1, FieldName.Len() - Open - 2);
            if (IndexText.IsEmpty() || !IndexText.IsNumeric()) return Reject(Warn, TEXT("invalid static array index"));
            ArrayIndex = FCString::Atoi(*IndexText);
            FieldName.LeftInline(Open);
        }
        FieldName = FieldName.TrimQuotes();
        Property = FindFProperty<FProperty>(Struct, *FieldName);
        if (!Property || ArrayIndex < 0 || ArrayIndex >= Property->ArrayDim)
            return Reject(Warn, FString::Printf(TEXT("unsupported property path %s"), *Record.Property));
        if (Index + 1 < Parts.Num())
        {
            const auto* StructProperty = CastField<FStructProperty>(Property);
            if (!StructProperty) return Reject(Warn, TEXT("a dotted path must traverse a struct"));
            Chain.PushProperty(Property, Property->IsEditorOnlyProperty());
            Container = Property->ContainerPtrToValuePtr<void>(Container, ArrayIndex);
            Struct = StructProperty->Struct;
        }
    }
    if (!Property) return Reject(Warn, TEXT("empty property path"));
    void* Data = Property->ContainerPtrToValuePtr<void>(Container, ArrayIndex);
    const bool Delta = Record.Operation == EOverriddenPropertyOperation::Add || Record.Operation == EOverriddenPropertyOperation::Remove;
    if (auto* Array = CastField<FArrayProperty>(Property); Array && Delta)
    {
        auto* Inner = CastField<FObjectPropertyBase>(Array->Inner);
        if (!Inner || !Inner->HasAnyPropertyFlags(CPF_PersistentInstance))
            return Reject(Warn, TEXT("array deltas require instanced object elements"));
        UObject* Element = nullptr;
        if (!ImportValue(Inner, *Record.Value, &Element, Owner, PortFlags, Warn, Remapper) || !Element)
            return Reject(Warn, TEXT("array delta object could not be resolved"));
        FScriptArrayHelper Helper(Array, Data);
        if (Record.Operation == EOverriddenPropertyOperation::Remove)
        {
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                UObject* Current = Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index));
                if (Current && Current->GetArchetype() == Element) { Helper.RemoveValues(Index); break; }
            }
        }
        else
        {
            int32 Target = INDEX_NONE;
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                UObject* Current = Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index));
                if (Current == Element || Current == Element->GetArchetype()) { Target = Index; break; }
            }
            if (Target == INDEX_NONE) Target = Helper.AddValue();
            Inner->SetObjectPropertyValue(Helper.GetRawPtr(Target), Element);
        }
        if (auto* Node = Set->SetOverriddenPropertyOperation(EOverriddenPropertyOperation::Modified, &Chain, Property))
            Set->SetSubPropertyOperation(Record.Operation, *Node, FOverriddenPropertyNodeID(*Element));
        return true;
    }
    if (auto* Map = CastField<FMapProperty>(Property); Map && (Delta || Record.Operation == EOverriddenPropertyOperation::Modified))
    {
        FScriptMapHelper Helper(Map, Data);
        void* Key = FMemory::Malloc(Map->KeyProp->GetSize(), Map->KeyProp->GetMinAlignment());
        void* Value = FMemory::Malloc(Map->ValueProp->GetSize(), Map->ValueProp->GetMinAlignment());
        Map->KeyProp->InitializeValue(Key); Map->ValueProp->InitializeValue(Value);
        ON_SCOPE_EXIT { Map->KeyProp->DestroyValue(Key); Map->ValueProp->DestroyValue(Value); FMemory::Free(Key); FMemory::Free(Value); };
        const TCHAR* Cursor = *Record.Value;
        if (Record.Operation != EOverriddenPropertyOperation::Remove && *Cursor++ != TEXT('(')) return Reject(Warn, TEXT("map delta requires a pair"));
        Cursor = ImportValue(Map->KeyProp, Cursor, Key, Owner, PortFlags, Warn, Remapper);
        if (!Cursor) return Reject(Warn, TEXT("invalid map key"));
#if SOLOAS_UE_AT_LEAST(5, 5)
        if (const auto* ObjectKey = CastField<FObjectPropertyBase>(Map->KeyProp); ObjectKey && !ObjectKey->GetObjectPropertyValue(Key))
            return Reject(Warn, TEXT("UE 5.5 cannot represent null object map keys in its override tree"));
#endif
        if (Record.Operation == EOverriddenPropertyOperation::Remove) Helper.RemovePair(Key);
        else
        {
            while (FChar::IsWhitespace(*Cursor)) ++Cursor;
            if (*Cursor++ != TEXT(',')) return Reject(Warn, TEXT("missing map value separator"));
            int32 Target = Helper.FindMapIndexWithKey(Key);
            void* TargetValue = Target != INDEX_NONE ? Helper.GetValuePtr(Target) : Value;
            Cursor = ImportValue(Map->ValueProp, Cursor, TargetValue, Owner, PortFlags, Warn, Remapper);
            if (!Cursor) return Reject(Warn, TEXT("invalid map value"));
            while (FChar::IsWhitespace(*Cursor)) ++Cursor;
            if (*Cursor != TEXT(')')) return Reject(Warn, TEXT("unterminated map pair"));
            if (Record.Operation == EOverriddenPropertyOperation::Add && Target == INDEX_NONE) Helper.AddPair(Key, Value);
        }
#if SOLOAS_UE_BEFORE(5, 5)
        FOverriddenPropertyNodeID KeyID;
        const auto* ObjectKey = CastField<FObjectProperty>(Map->KeyProp);
        if (UObject* KeyObject = ObjectKey ? ObjectKey->GetObjectPropertyValue(Key) : nullptr)
        {
            KeyID = FOverriddenPropertyNodeID(*KeyObject);
        }
        else
        {
            FString TextKey;
            Map->KeyProp->ExportTextItem_Direct(TextKey, Key, nullptr, nullptr, PPF_None);
            KeyID.Path = *TextKey;
        }
#else
        auto KeyID = FOverriddenPropertyNodeID::FromMapKey(Map->KeyProp, Key);
#endif
        if (auto* Node = Set->SetOverriddenPropertyOperation(EOverriddenPropertyOperation::Modified, &Chain, Property))
            Set->SetSubPropertyOperation(Record.Operation, *Node, KeyID);
        return true;
    }
    if (Delta) return Reject(Warn, TEXT("add/remove requires a container"));
    if (!ImportValue(Property, *Record.Value, Data, Owner, PortFlags, Warn, Remapper)) return Reject(Warn, TEXT("invalid property value"));
    Set->SetOverriddenPropertyOperation(Record.Operation, &Chain, Property);
    return true;
}

const TCHAR* Import(FImportObjectParams& Params)
{
    const auto Native = reinterpret_cast<FImport>(ImportHook.Original());
    if (!IsInGameThread() || CVarLegacyT3D.GetValueOnAnyThread() == 0 || !Params.SourceText) return Native(Params);
    FString OriginalText(Params.SourceText);
    bool Protocol = SoloASBridge::HasUnsupportedT3DOverrideTokens(Params.SourceText);
    if (!Protocol)
    {
        const TCHAR* Cursor = Params.SourceText;
        FString Line;
        while (FParse::Line(&Cursor, Line)) if (HasPropertyOperation(Line)) { Protocol = true; break; }
    }
    if (!Protocol) return Native(Params);
    FString NativeText;
    TArray<FOverrideLine> Records;
    if (!Parse(Params.SourceText, NativeText, Records, Params.Warn)) return nullptr;
    UObject* Owner = Params.SubobjectOuter;
    if (!Owner || Params.ObjectStruct != Owner->GetClass() || Params.DestData != reinterpret_cast<uint8*>(Owner))
    { Reject(Params.Warn, TEXT("override text requires an object import")); return nullptr; }
    FImportObjectParams NativeParams = Params;
    NativeParams.SourceText = *NativeText;
    NativeParams.bShouldCallEditChange = false;
    if (Params.bShouldCallEditChange) Owner->PreEditChange(nullptr);
    if (!Native(NativeParams)) return nullptr;
    TMap<FSoftObjectPath, UObject*> Remapper = Params.ObjectRemapper ? *Params.ObjectRemapper : TMap<FSoftObjectPath, UObject*>();
    for (const FOverrideLine& Record : Records)
        if (!Record.ExportPath.IsEmpty())
            if (UObject* Imported = FindOwner(Owner, Record.Objects)) Remapper.Add(FSoftObjectPath(Record.ExportPath), Imported);
    for (const FOverrideLine& Record : Records) if (!Replay(*Owner, Record, PPF_Delimited | PPF_CheckReferences | (GIsImportingT3D ? PPF_AttemptNonQualifiedSearch : 0), Params.Warn, Remapper)) return nullptr;
    if (Params.bShouldCallEditChange)
    {
        Owner->PostEditImport();
        Owner->PostEditChange();
    }
    return Params.SourceText + OriginalText.Len();
}

void Process(FCustomizableTextObjectFactory* Factory, UObject* Parent, EObjectFlags Flags, const TCHAR* Text)
{
    const auto Native = reinterpret_cast<FProcess>(FactoryHook.Original());
    if (!IsInGameThread() || CVarLegacyT3D.GetValueOnAnyThread() == 0 || !SoloASBridge::HasUnsupportedT3DOverrideTokens(Text))
    { Native(Factory, Parent, Flags, Text); return; }
    // The factory consumes root object headers before ImportObjectProperties sees them.
    // Move their operation into the object's property block; the importer consumes this directive.
    FString CheckedText;
    TArray<FOverrideLine> CheckedRecords;
    if (!Parse(Text, CheckedText, CheckedRecords, GWarn)) return;
    FString Converted;
    const TCHAR* Cursor = Text;
    FString Line;
    while (FParse::Line(&Cursor, Line))
    {
        FString Trimmed = Line.TrimStartAndEnd();
        const TCHAR* Header = *Trimmed;
        FString Operation;
        if (FParse::Command(&Header, TEXT("BEGIN")) && FParse::Command(&Header, TEXT("OBJECT")))
        {
            const bool HasOperation = FParse::Value(Header, TEXT("OverriddenOperation="), Operation);
            FString ExportPath;
            FParse::Value(Header, TEXT("ExportPath="), ExportPath);
            if (HasOperation)
            {
                const int32 Start = Line.Find(TEXT("OverriddenOperation="), ESearchCase::IgnoreCase);
                int32 End = Start;
                while (End < Line.Len() && !FChar::IsWhitespace(Line[End])) ++End;
                Line.RemoveAt(Start, End - Start);
            }
            Converted += Line + TEXT("\r\n");
            if (HasOperation) Converted += TEXT("T3DOverrides=") + Operation + TEXT("\r\n");
            if (!ExportPath.IsEmpty()) Converted += TEXT("T3DExportPath=\"") + ExportPath + TEXT("\"\r\n");
        }
        else Converted += Line + TEXT("\r\n");
    }
    Native(Factory, Parent, Flags, *Converted);
}
}

void InitializeLegacyT3DOverrides()
{
    const bool Installed = ImportHook.Install(TEXT("?ImportObjectProperties@@YAPEB_WAEAUFImportObjectParams@@@Z"),
        "_Z22ImportObjectPropertiesR19FImportObjectParams", reinterpret_cast<void*>(&Import));
    if (Installed) FactoryHook.Install(TEXT("?ProcessBuffer@FCustomizableTextObjectFactory@@QEAAXPEAVUObject@@W4EObjectFlags@@PEB_W@Z"),
        "_ZN30FCustomizableTextObjectFactory13ProcessBufferEP7UObject12EObjectFlagsPKDs", reinterpret_cast<void*>(&Process));
}
void ShutdownLegacyT3DOverrides()
{
    FactoryHook.Remove();
    ImportHook.Remove();
}
}
#else
namespace SoloASBridge::Private
{
void InitializeLegacyT3DOverrides() {}
void ShutdownLegacyT3DOverrides() {}
}
#endif
// SOLOAS-COMPAT-END
