#pragma once
#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "AngelscriptAnyStructParameter.h"

#include "StartAngelscriptHeaders.h"
#include "as_context.h"
#include "as_scriptfunction.h"
#include "as_objecttype.h"
#include "as_generic.h"
#include "as_scriptengine.h"
#include "EndAngelscriptHeaders.h"

struct TASAnyReference
{
	TASAnyReference(void* InPtr)
		: Ptr(InPtr)
	{
	}

	template<typename T>
	TASAnyReference(const T& InReference)
		: Ptr((void*)&InReference)
	{
	}

	void* GetPointer() const
	{
		return Ptr;
	}

private:
	void* Ptr;
};

struct ANGELSCRIPTCODE_API FAngelscriptBindHelpers
{
	static UObject* GetObjectFromProperty(void* Container, asCScriptFunction* Function)
	{
		SIZE_T Offset = (SIZE_T)Function->userData;
		return *(UObject**)((SIZE_T)Container + Offset);
	}

	static UObject* GetUnresolvedObjectFromProperty(void* Container, asCScriptFunction* Function)
	{
		SIZE_T Offset = (SIZE_T)Function->userData;
		return ((FObjectPtr*)((SIZE_T)Container + Offset))->Get();
	}

	static void* GetValueFromProperty(void* Container, asCScriptFunction* Function)
	{
		SIZE_T Offset = (SIZE_T)Function->userData;
		return (void*)((SIZE_T)Container + Offset);
	}

	static void GetValueFromPropertyGetter_Struct(asIScriptGeneric* Generic)
	{
		asCScriptFunction* Function = (asCScriptFunction*)((asCGeneric*)Generic)->GetFunction();
		FProperty* Property = (FProperty*)Function->userData;
		void* ReturnPtr = (void*)*(asPWORD*)&((asCGeneric*)Generic)->stackPointer[2];
		((asCScriptEngine*)Function->engine)->ConstructScriptObject(ReturnPtr, (asCObjectType*)Function->returnType.GetTypeInfo());
		Property->CallGetter(Generic->GetObject(), ReturnPtr);
	}

	static void GetValueFromPropertyGetter_Handle(asIScriptGeneric* Generic)
	{
		asCScriptFunction* Function = (asCScriptFunction*)((asCGeneric*)Generic)->GetFunction();
		FProperty* Property = (FProperty*)Function->userData;
		void* ReturnPtr = &((asCGeneric*)Generic)->objectRegister;
		Property->CallGetter(Generic->GetObject(), ReturnPtr);
	}

	static void GetValueFromPropertyGetter_Primitive(asIScriptGeneric* Generic)
	{
		asCScriptFunction* Function = (asCScriptFunction*)((asCGeneric*)Generic)->GetFunction();
		FProperty* Property = (FProperty*)Function->userData;
		void* ReturnPtr = &((asCGeneric*)Generic)->returnVal;
		Property->CallGetter(Generic->GetObject(), ReturnPtr);
	}

	static void GetValueFromPropertyGetter_POD(asIScriptGeneric* Generic)
	{
		asCScriptFunction* Function = (asCScriptFunction*)((asCGeneric*)Generic)->GetFunction();
		FProperty* Property = (FProperty*)Function->userData;
		void* ReturnPtr = (void*)*(asPWORD*)&((asCGeneric*)Generic)->stackPointer[2];
		Property->CallGetter(Generic->GetObject(), ReturnPtr);
	}

	static void SetObjectFromProperty(void* Container, asCScriptFunction* Function, UObject* NewValue)
	{
		SIZE_T Offset = (SIZE_T)Function->userData;
		*(UObject**)((SIZE_T)Container + Offset) = NewValue;
	}

	static void SetValueFromProperty(void* Container, asCScriptFunction* Function, void* NewValue)
	{
		auto* Prop = (FProperty*)Function->userData;
		Prop->CopySingleValue(Prop->ContainerPtrToValuePtr<void>(Container), NewValue);
	}

	static void SetValueFromProperty_Native(void* Container, asCScriptFunction* Function, TASAnyReference NewValue)
	{
		FProperty* Prop = (FProperty*)Function->userData;
		Prop->CopySingleValue(Prop->ContainerPtrToValuePtr<void>(Container), NewValue.GetPointer());
	}

	static void SetValueFromProperty_Byte(void* Container, asCScriptFunction* Function, void* NewValue)
	{
		SIZE_T Offset = (SIZE_T)Function->userData;
		FMemory::Memcpy((void*)((SIZE_T)Container + Offset), NewValue, 1);
	}

	static void SetValueFromProperty_NativeByte(void* Container, asCScriptFunction* Function, TASAnyReference NewValue)
	{
		SIZE_T Offset = (SIZE_T)Function->userData;
		FMemory::Memcpy((void*)((SIZE_T)Container + Offset), NewValue.GetPointer(), 1);
	}

	static void SetValueFromProperty_DWord(void* Container, asCScriptFunction* Function, void* NewValue)
	{
		SIZE_T Offset = (SIZE_T)Function->userData;
		FMemory::Memcpy((void*)((SIZE_T)Container + Offset), NewValue, 4);
	}

	static void SetValueFromProperty_NativeDWord(void* Container, asCScriptFunction* Function, TASAnyReference NewValue)
	{
		SIZE_T Offset = (SIZE_T)Function->userData;
		FMemory::Memcpy((void*)((SIZE_T)Container + Offset), NewValue.GetPointer(), 4);
	}

	static void SetValueFromProperty_QWord(void* Container, asCScriptFunction* Function, void* NewValue)
	{
		SIZE_T Offset = (SIZE_T)Function->userData;
		FMemory::Memcpy((void*)((SIZE_T)Container + Offset), NewValue, 8);
	}

	static void SetValueFromProperty_NativeQWord(void* Container, asCScriptFunction* Function, TASAnyReference NewValue)
	{
		SIZE_T Offset = (SIZE_T)Function->userData;
		FMemory::Memcpy((void*)((SIZE_T)Container + Offset), NewValue.GetPointer(), 8);
	}

	static void SetValueFromProperty_ByteExtendToDWord(void* Container, asCScriptFunction* Function, void* NewValue)
	{
		uint32 ExtendedValue = 0;
		FMemory::Memcpy(&ExtendedValue, NewValue, 1);

		SIZE_T Offset = (SIZE_T)Function->userData;
		FMemory::Memcpy((void*)((SIZE_T)Container + Offset), &ExtendedValue, 4);
	}

	static void SetValueFromProperty_NativeByteExtendToDWord(void* Container, asCScriptFunction* Function, TASAnyReference NewValue)
	{
		uint32 ExtendedValue = 0;
		FMemory::Memcpy(&ExtendedValue, NewValue.GetPointer(), 1);

		SIZE_T Offset = (SIZE_T)Function->userData;
		FMemory::Memcpy((void*)((SIZE_T)Container + Offset), &ExtendedValue, 4);
	}

	static void SetObjectFromPropertySetter(void* Container, asCScriptFunction* Function, UObject* NewValue)
	{
		auto* Prop = (FProperty*)Function->userData;
		Prop->CallSetter(Container, &NewValue);
	}

	static void SetValueFromPropertySetter(void* Container, asCScriptFunction* Function, void* NewValue)
	{
		auto* Prop = (FProperty*)Function->userData;
		Prop->CallSetter(Container, NewValue);
	}

	static void SetValueFromPropertySetter_Native(void* Container, asCScriptFunction* Function, TASAnyReference NewValue)
	{
		auto* Prop = (FProperty*)Function->userData;
		Prop->CallSetter(Container, NewValue.GetPointer());
	}

	static UClass* GetStaticClassFromClass(asCScriptFunction* Function)
	{
		return (UClass*)Function->userData;
	}

	static bool GetBoolFromProperty(void* Container, asCScriptFunction* Function)
	{
		FBoolProperty* Prop = (FBoolProperty*)Function->userData;
		return Prop->GetPropertyValue_InContainer(Container);
	}

	static void SetBoolFromProperty(void* Container, asCScriptFunction* Function, bool Value)
	{
		FBoolProperty* Prop = (FBoolProperty*)Function->userData;
		return Prop->SetPropertyValue_InContainer(Container, Value);
	}

	static bool GetBoolFromPropertyGetter(void* Container, asCScriptFunction* Function)
	{
		FProperty* Property = (FProperty*)Function->userData;

		bool bValue = false;
		Property->CallGetter(Container, &bValue);
		return bValue;
	}

	static void SetBoolFromPropertySetter(void* Container, asCScriptFunction* Function, bool Value)
	{
		auto* Prop = (FProperty*)Function->userData;
		Prop->CallSetter(Container, &Value);
	}
};

struct FScriptStructType
{
	UScriptStruct* Struct = nullptr;
};

struct ANGELSCRIPTCODE_API FAngelscriptStructTypeHelpers
{
	static void Construct(FScriptStructType* Ptr)
	{
		new (Ptr) FScriptStructType();
	}

	static void CopyConstruct(FScriptStructType* Ptr, FScriptStructType& Other)
	{
		new (Ptr) FScriptStructType();
		Ptr->Struct = Other.Struct;
	}

	static UScriptStruct* GetStruct(FScriptStructType* Ptr)
	{
		return Ptr->Struct;
	}

	static bool IsValid(FScriptStructType* Ptr)
	{
		return Ptr->Struct != nullptr;
	}

	static bool OpEquals(FScriptStructType* Ptr, FScriptStructType& Other)
	{
		return Ptr->Struct == Other.Struct;
	}

	static bool OpEqualsStruct(FScriptStructType* Ptr, UScriptStruct* Other)
	{
		return Ptr->Struct == Other;
	}
};

struct ANGELSCRIPTCODE_API FAngelscriptInstancedStructHelpers
{
	static FInstancedStruct Make(void* Data, const int TypeId);
	static void ImplicitConstructAnyStruct(FAngelscriptAnyStructParameter* Self, void* Data, const int TypeId);
	static void ImplicitConstructAnyStructFromInstancedStruct(FAngelscriptAnyStructParameter* Self, const FInstancedStruct& InstancedStruct);
	static void InitializeAs_Struct(FInstancedStruct* Self, void* Data, const int TypeId);
	static void InitializeAs_Default(FInstancedStruct* Self, UScriptStruct* StructType)
	{
		Self->InitializeAs(StructType);
	}

	static struct FScriptStructWildcard& GetMemory(FInstancedStruct* Self, const UScriptStruct* StructType);

	static bool Contains(FInstancedStruct* Self, const UScriptStruct* StructType)
	{
		if (!Self->IsValid())
			return false;

		if (StructType != Self->GetScriptStruct())
			return false;

		return true;
	}
};
