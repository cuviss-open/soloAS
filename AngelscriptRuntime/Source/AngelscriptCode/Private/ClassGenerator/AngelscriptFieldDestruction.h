// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FAngelscriptModuleDesc;
class UASClass;
class asITypeInfo;
class UObject;
class UClass;
class FProperty;
class FStructProperty;

struct FSameCookieReconstructionField
{
    const FStructProperty* Property = nullptr;
    UASClass* Owner = nullptr;
    asITypeInfo* OwnerType = nullptr;
    void* Storage = nullptr;
    int32 Offset = 0;
    uint64 Flags = 0;
};

/** Caller-owned metadata only; old values remain live throughout all old AS reads. */
struct FSameCookieReconstructionFieldPlan
{
    UObject* Object = nullptr;
    UASClass* Class = nullptr;
    UClass* ActualClass = nullptr;
    asITypeInfo* Cookie = nullptr;
    int32 ObjectIndex = INDEX_NONE, ObjectSerial = 0;
    int32 ClassIndex = INDEX_NONE, ClassSerial = 0;
    TArray<const FProperty*> DestructorChain;
    TArray<FSameCookieReconstructionField> Fields;
    bool bCompleted = false;
};

bool CaptureSameCookieReconstructionFields(UObject* Object, UASClass* Class,
    asITypeInfo* Cookie, FSameCookieReconstructionFieldPlan& OutPlan);
bool CompleteSameCookieReconstructionFields(FSameCookieReconstructionFieldPlan& Plan);

void PrepareAngelscriptFieldDestruction(TSharedRef<FAngelscriptModuleDesc> Module);
void ValidateAngelscriptFieldDestruction(UASClass* Class, asITypeInfo* ScriptType);
void VerifyAngelscriptFieldDestructorLinks(const UASClass* Class);
