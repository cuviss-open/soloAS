#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include "AngelscriptManager.h"
#include "AngelscriptType.h"
#include "AngelscriptBinds.h"

#if AS_ITERATOR_DEBUGGING
thread_local static TArray<void*, TInlineAllocator<16>> GFieldMapsBeingIterated;

static bool CheckObjectIteratorDebug(void* MapPtr)
{
	if (GFieldMapsBeingIterated.Contains(MapPtr))
	{
		FAngelscriptManager::Throw("FJsonObject is being modified during for loop iteration");
		return false;
	}

	return true;
}
#endif

static FString ValueTypeToString(EJson T)
{
	switch (T)
	{
	case EJson::None:
		return TEXT("None");
	case EJson::Null:
		return TEXT("Null");
	case EJson::String:
		return TEXT("String");
	case EJson::Number:
		return TEXT("Number");
	case EJson::Boolean:
		return TEXT("Boolean");
	case EJson::Array:
		return TEXT("Array");
	case EJson::Object:
		return TEXT("Object");
	default:
		return TEXT("<Invalid Type>");
	}
}

struct FJsonValueContainer
{
	TSharedPtr<FJsonValue> Value;

	FJsonValueContainer() = default;
	FJsonValueContainer(TSharedPtr<FJsonValue> InValue) : Value(InValue) {}

	EJson GetType() const
	{
		if (Value.IsValid())
			return Value->Type;
		else
			return EJson::None;
	}

	bool TryGetNumber(double& OutNumber) const
	{
		if (Value.IsValid())
			return Value->TryGetNumber(OutNumber);
		else
			return false;
	}

	bool TryGetNumber(float& OutNumber) const
	{
		if (Value.IsValid())
			return Value->TryGetNumber(OutNumber);
		else
			return false;
	}

	bool TryGetNumber(int32& OutNumber) const
	{
		if (Value.IsValid())
			return Value->TryGetNumber(OutNumber);
		else
			return false;
	}

	bool TryGetNumber(int64& OutNumber) const
	{
		if (Value.IsValid())
			return Value->TryGetNumber(OutNumber);
		else
			return false;
	}

	bool TryGetString(FString& OutString) const
	{
		if (Value.IsValid())
			return Value->TryGetString(OutString);
		else
			return false;
	}

	bool TryGetBool(bool& OutBool) const
	{
		if (Value.IsValid())
			return Value->TryGetBool(OutBool);
		else
			return false;
	}

	bool TryGetArray(struct FJsonValueArrayContainer& OutArray) const;

	bool TryGetObject(struct FJsonObjectContainer& Object) const;

	bool IsNull() const
	{
		if (Value.IsValid())
			return Value->Type == EJson::Null || Value->Type == EJson::None;
		else
			return false;
	}

};

struct FJsonValueArrayContainer
{
	TArray<TSharedPtr<FJsonValue>> Array;

	void Empty()
	{
		Array.Empty();
	}

	void AddNull()
	{
		Array.Add(MakeShared<FJsonValueNull>());
	}

	void AddString(const FString& Str)
	{
		Array.Add(MakeShared<FJsonValueString>(Str));
	}

	void AddNumberInt(int32 I)
	{
		Array.Add(MakeShared<FJsonValueNumber>(I));
	}

	void AddNumberDouble(double D)
	{
		Array.Add(MakeShared<FJsonValueNumber>(D));
	}

	void AddBoolean(bool B)
	{
		Array.Add(MakeShared<FJsonValueBoolean>(B));
	}

	void AddArray(const FJsonValueArrayContainer& ArrayValue)
	{
		Array.Add(MakeShared<FJsonValueArray>(ArrayValue.Array));
	}

	void AddObject(const FJsonObjectContainer& ObjectValue);

	void AddValue(const FJsonValueContainer& Value)
	{
		Array.Add(Value.Value);
	}

	int32 Num() const
	{
		return Array.Num();
	}

	FJsonValueContainer GetValueAt(int32 Index) const
	{
		if (!Array.IsValidIndex(Index))
		{
			FAngelscriptManager::Throw("Array index is out of bounds");
			return {};
		}

		return { Array[Index] };
	}

	// TODO : expand FJsonValueArrayContainer to support every value create/access method

};

struct FJsonObjectContainer
{
	TSharedPtr<FJsonObject> JsonObject;

	FJsonObjectContainer() = default;
	FJsonObjectContainer(TSharedPtr<FJsonObject> InJsonObject) : JsonObject(InJsonObject) {}

	// this one is not for binding!
	bool CheckValidObject() const
	{
		if (JsonObject.IsValid())
			return true;
		else
		{
			FAngelscriptManager::Throw("JsonObject is not valid");
			return false;
		}
	}

	void TypeErrorMessage(EJson FieldType, const FString& InType) const
	{
		auto Message = FString::Printf(TEXT("Json Value of type '%s' used as a '%s'."), *ValueTypeToString(FieldType), *InType);
		FAngelscriptManager::Throw(TCHAR_TO_ANSI(*Message));
	}

#if AS_ITERATOR_DEBUGGING
	bool CheckIteratorForNewField(const FString& FieldName) const
	{
		if (JsonObject.IsValid())
		{
			// adding a new field during iteration might reallocate/rehash the map and cause issues
			if (!JsonObject->HasField(FieldName))
				return CheckObjectIteratorDebug(&JsonObject->Values);
			return true;
		}

		return false;
	}
#endif

	bool IsValid() const
	{
		return JsonObject.IsValid();
	}

	bool HasField(const FString& FieldName) const
	{
		return CheckValidObject() && JsonObject->HasField(FieldName);
	}

	FString GetStringField(const FString& FieldName) const
	{
		if (CheckValidObject())
		{
			FString String;

			auto Value = JsonObject->GetField<EJson::None>(FieldName);
			if (!Value->TryGetString(String))
			{
				TypeErrorMessage(Value->Type, TEXT("String"));
			}

			return String;
		}
		else
		{
			return TEXT("");
		}
	}

	bool TryGetStringField(const FString& FieldName, FString& OutString) const
	{
		if (CheckValidObject())
		{
			auto Value = JsonObject->GetField<EJson::None>(FieldName);
			return Value->TryGetString(OutString);
		}
		else
		{
			return false;
		}
	}

	void SetStringField(const FString& FieldName, const FString& StringValue)
	{
		if (CheckValidObject())
		{
#if AS_ITERATOR_DEBUGGING
			if (!CheckIteratorForNewField(FieldName))
				return;
#endif
			JsonObject->SetStringField(FieldName, StringValue);
		}
	}

	double GetNumberField(const FString& FieldName) const
	{
		if (CheckValidObject())
		{
			double Number = 0.0;

			auto Value = JsonObject->GetField<EJson::None>(FieldName);
			if (!Value->TryGetNumber(Number))
			{
				TypeErrorMessage(Value->Type, TEXT("Number"));
			}

			return Number;
		}
		else
		{
			return 0.;
		}
	}

	bool TryGetNumberField(const FString& FieldName, double& OutNumber) const
	{
		if (CheckValidObject())
		{
			auto Value = JsonObject->GetField<EJson::None>(FieldName);
			return Value->TryGetNumber(OutNumber);
		}
		else
		{
			return false;
		}
	}

	void SetNumberField(const FString& FieldName, double Number)
	{
		if (CheckValidObject())
		{
#if AS_ITERATOR_DEBUGGING
			if (!CheckIteratorForNewField(FieldName))
				return;
#endif
			JsonObject->SetNumberField(FieldName, Number);
		}
	}

	bool GetBoolField(const FString& FieldName) const
	{
		if (CheckValidObject())
		{
			bool bBool = false;

			auto Value = JsonObject->GetField<EJson::None>(FieldName);
			if (!Value->TryGetBool(bBool))
			{
				TypeErrorMessage(Value->Type, TEXT("Bool"));
			}

			return bBool;
		}
		else
		{
			return false;
		}
	}

	bool TryGetBoolField(const FString& FieldName, bool& bOutBool) const
	{
		if (CheckValidObject())
		{
			auto Value = JsonObject->GetField<EJson::None>(FieldName);
			return Value->TryGetBool(bOutBool);
		}
		else
		{
			return false;
		}
	}

	void SetBoolField(const FString& FieldName, bool InValue)
	{
		if (CheckValidObject())
		{
#if AS_ITERATOR_DEBUGGING
			if (!CheckIteratorForNewField(FieldName))
				return;
#endif
			JsonObject->SetBoolField(FieldName, InValue);
		}
	}

	FJsonObjectContainer GetObjectField(const FString& FieldName) const
	{
		if (CheckValidObject())
		{
			auto Value = JsonObject->GetField<EJson::None>(FieldName);
			if (Value->Type == EJson::Object)
			{
				return FJsonObjectContainer(JsonObject->GetObjectField(FieldName));
			}

			TypeErrorMessage(Value->Type, TEXT("Object"));
		}

		return FJsonObjectContainer();
	}

	void SetObjectField(const FString& FieldName, const FJsonObjectContainer& InObject)
	{
		if (CheckValidObject())
		{
#if AS_ITERATOR_DEBUGGING
			if (!CheckIteratorForNewField(FieldName))
				return;
#endif
			JsonObject->SetObjectField(FieldName, InObject.JsonObject);
		}
	}

	FJsonObjectContainer CreateObjectField(const FString& FieldName)
	{
		FJsonObjectContainer Result;
		if (CheckValidObject())
		{
#if AS_ITERATOR_DEBUGGING
			if (!CheckIteratorForNewField(FieldName))
				return Result;
#endif
			Result.JsonObject = MakeShared<FJsonObject>();
			JsonObject->SetObjectField(FieldName, Result.JsonObject);
		}
		return Result;
	}

	FJsonValueArrayContainer GetArrayField(const FString& FieldName) const
	{
		if (CheckValidObject())
		{
			auto Value = JsonObject->GetField<EJson::None>(FieldName);
			if (Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>* Array;
				if (JsonObject->TryGetArrayField(FieldName, Array))
				{
					FJsonValueArrayContainer Result;
					Result.Array = *Array;
					return Result;
				}
			}
			else
			{
				TypeErrorMessage(Value->Type, TEXT("Array"));
			}
		}

		return FJsonValueArrayContainer();
	}

	void SetArrayField(const FString& FieldName, const FJsonValueArrayContainer& Array)
	{
		if (CheckValidObject())
		{
#if AS_ITERATOR_DEBUGGING
			if (!CheckObjectIteratorDebug(&JsonObject->Values))
				return;
#endif
			JsonObject->SetArrayField(FieldName, Array.Array);
		}
	}

	FJsonValueContainer GetField(const FString& FieldName)
	{
		if (CheckValidObject())
		{
			TSharedPtr<FJsonValue> Field = JsonObject->TryGetField(FieldName);
			return FJsonValueContainer(Field);
		}
		else
			return FJsonValueContainer();
	}

	void SetField(const FString& FieldName, const FJsonValueContainer& Value)
	{
		if (CheckValidObject())
		{
#if AS_ITERATOR_DEBUGGING
			if (!CheckIteratorForNewField(FieldName))
				return;
#endif
			JsonObject->SetField(FieldName, Value.Value);
		}
	}

	void RemoveField(const FString& FieldName)
	{
		if (CheckValidObject())
		{
#if AS_ITERATOR_DEBUGGING
			if (!CheckObjectIteratorDebug(&JsonObject->Values))
				return;
#endif
			JsonObject->RemoveField(FieldName);
		}
	}

	void RemoveAllFields()
	{
		if (CheckValidObject())
		{
#if AS_ITERATOR_DEBUGGING
			if (!CheckObjectIteratorDebug(&JsonObject->Values))
				return;
#endif
			JsonObject->Values.Empty();
		}
	}

	// TODO : expand FJsonObjectContainer to support every field type create/access method
};

void FJsonValueArrayContainer::AddObject(const FJsonObjectContainer& ObjectValue)
{
	Array.Add(MakeShared<FJsonValueObject>(ObjectValue.JsonObject));
}

struct FJsonObjectFieldIterator
{
	using MapType = decltype(FJsonObject::Values);
	MapType::TIterator Iterator;

	bool bCanProceed;
	MapType::TMapBase::ElementType CurrentValue;
	bool bValidValue;

#if AS_ITERATOR_DEBUGGING
	MapType* MapPtr;
#endif

	FJsonObjectFieldIterator(MapType& JsonValues)
		: Iterator(JsonValues.CreateIterator())
		, bCanProceed(Iterator)
		, bValidValue(false)
#if AS_ITERATOR_DEBUGGING
		, MapPtr(&JsonValues)
#endif
	{}

	FJsonObjectFieldIterator()
		: Iterator(MapType().CreateIterator())
		, bCanProceed(false)
		, bValidValue(false)
#if AS_ITERATOR_DEBUGGING
		, MapPtr(nullptr)
#endif
	{}

	bool CheckValidIterator() const
	{
		if (!bValidValue)
		{
			FAngelscriptManager::Throw("Iterator out of range");
			return false;
		}
		return true;
	}

	FString GetFieldName() const
	{
		if (!CheckValidIterator())
		{
			return TEXT("");
		}
		return FString(CurrentValue.Key.ToView());
	}

	FJsonValueContainer GetValue() const
	{
		if (!CheckValidIterator())
		{
			return FJsonValueContainer();
		}
		return FJsonValueContainer(CurrentValue.Value);
	}

	EJson GetType() const
	{
		if (CheckValidIterator() && CurrentValue.Value.IsValid())
		{
			return CurrentValue.Value->Type;
		}
		
		return EJson::None;
	}
};

bool FJsonValueContainer::TryGetArray(FJsonValueArrayContainer& OutArray) const
{
	if (Value.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ArrayValue;
		if (Value->TryGetArray(ArrayValue))
		{
			OutArray.Array = *ArrayValue;
			return true;
		}
	}
	return false;
}

bool FJsonValueContainer::TryGetObject(FJsonObjectContainer& Object) const
{
	if (Value.IsValid())
	{
		const TSharedPtr<FJsonObject>* ObjectValue;
		if (Value->TryGetObject(ObjectValue))
		{
			Object.JsonObject = *ObjectValue;
			return true;
		}
	}
	
	return false;
}

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_JsonClasses(FAngelscriptBinds::EOrder::Late, []
	{
		auto EJsonValueType = FAngelscriptBinds::Enum("EJsonType");
		EJsonValueType["None"] = EJson::None;
		EJsonValueType["Null"] = EJson::Null;
		EJsonValueType["String"] = EJson::String;
		EJsonValueType["Number"] = EJson::Number;
		EJsonValueType["Boolean"] = EJson::Boolean;
		EJsonValueType["Array"] = EJson::Array;
		EJsonValueType["Object"] = EJson::Object;

		auto JsonValue = FAngelscriptBinds::ValueClass<FJsonValueContainer>("FJsonValue", {});
		JsonValue.Constructor("void f()", [](FJsonValueContainer* Address)
			{
				new(Address) FJsonValueContainer();
			});
		JsonValue.Destructor("void f()", [](FJsonValueContainer& Obj)
			{
				Obj.~FJsonValueContainer();
			});
		JsonValue.Method("EJsonType GetType() const", METHOD_NOJIT(FJsonValueContainer, GetType));
		JsonValue.Method("bool TryGetNumber(float64& OutNumber) const", METHODPR_NOJIT(bool, FJsonValueContainer, TryGetNumber, (double&) const));
		JsonValue.Method("bool TryGetNumber(float32& OutNumber) const", METHODPR_NOJIT(bool, FJsonValueContainer, TryGetNumber, (float&) const));
		JsonValue.Method("bool TryGetNumber(int32& OutNumber) const", METHODPR_NOJIT(bool, FJsonValueContainer, TryGetNumber, (int32&) const));
		JsonValue.Method("bool TryGetNumber(int64& OutNumber) const", METHODPR_NOJIT(bool, FJsonValueContainer, TryGetNumber, (int64&) const));
		JsonValue.Method("bool TryGetString(FString& OutString) const", METHOD_NOJIT(FJsonValueContainer, TryGetString));
		JsonValue.Method("bool TryGetBool(bool& OutBool) const", METHOD_NOJIT(FJsonValueContainer, TryGetBool));
		JsonValue.Method("bool IsNull() const", METHOD_NOJIT(FJsonValueContainer, IsNull));

		auto JsonValueArray = FAngelscriptBinds::ValueClass<FJsonValueArrayContainer>("FJsonArray", {});
		JsonValueArray.Constructor("void f()", [](FJsonValueArrayContainer* Address)
			{
				new(Address) FJsonValueArrayContainer();
			});
		JsonValueArray.Destructor("void f()", [](FJsonValueArrayContainer& Obj)
			{
				Obj.~FJsonValueArrayContainer();
			});
		JsonValueArray.Method("void Empty()", METHOD_NOJIT(FJsonValueArrayContainer, Empty));
		JsonValueArray.Method("void AddString(const FString& Str)", METHOD_NOJIT(FJsonValueArrayContainer, AddString));
		JsonValueArray.Method("void AddNumber(int32 I)", METHOD_NOJIT(FJsonValueArrayContainer, AddNumberInt));
		JsonValueArray.Method("void AddNumber(float64 D)", METHOD_NOJIT(FJsonValueArrayContainer, AddNumberDouble));
		JsonValueArray.Method("void AddBoolean(bool B)", METHOD_NOJIT(FJsonValueArrayContainer, AddBoolean));
		JsonValueArray.Method("void AddArray(FJsonArray Array)", METHOD_NOJIT(FJsonValueArrayContainer, AddArray));
		JsonValueArray.Method("int32 Num() const", METHOD_NOJIT(FJsonValueArrayContainer, Num));
		JsonValueArray.Method("FJsonValue GetValueAt(int32 Index) const", METHOD_NOJIT(FJsonValueArrayContainer, GetValueAt));

		auto JsonObject = FAngelscriptBinds::ValueClass<FJsonObjectContainer>("FJsonObject", {});
		JsonObject.Constructor("void f(FJsonObject InObject)", [](FJsonObjectContainer* Address, const FJsonObjectContainer& InObject)
			{
				new(Address) FJsonObjectContainer();
				Address->JsonObject = InObject.JsonObject;
			});
		JsonObject.Constructor("void f()", [](FJsonObjectContainer* Address)
			{
				new(Address) FJsonObjectContainer();
				Address->JsonObject = MakeShared<FJsonObject>();
			});
		JsonObject.Destructor("void f()", [](FJsonObjectContainer& Obj)
			{
				Obj.~FJsonObjectContainer();
			});
		JsonObject.Method("bool IsValid() const", METHOD_NOJIT(FJsonObjectContainer, IsValid));
		JsonObject.Method("bool HasField(const FString& FieldName) const", METHOD_NOJIT(FJsonObjectContainer, HasField));
		JsonObject.Method("void RemoveField(const FString& FieldName)", METHOD_NOJIT(FJsonObjectContainer, RemoveField));
		JsonObject.Method("void RemoveAllFields()", METHOD_NOJIT(FJsonObjectContainer, RemoveAllFields));

		JsonObject.Method("FString GetStringField(const FString& FieldName) const", METHOD_NOJIT(FJsonObjectContainer, GetStringField));
		JsonObject.Method("float64 GetNumberField(const FString& FieldName) const", METHOD_NOJIT(FJsonObjectContainer, GetNumberField));
		JsonObject.Method("bool GetBoolField(const FString& FieldName) const", METHOD_NOJIT(FJsonObjectContainer, GetBoolField));
		JsonObject.Method("FJsonObject GetObjectField(const FString& FieldName) const", METHOD_NOJIT(FJsonObjectContainer, GetObjectField));
		JsonObject.Method("FJsonArray GetArrayField(const FString& FieldName) const", METHOD_NOJIT(FJsonObjectContainer, GetArrayField));

		JsonObject.Method("void SetStringField(const FString& FieldName, const FString& StringValue)", METHOD_NOJIT(FJsonObjectContainer, SetStringField));
		JsonObject.Method("void SetNumberField(const FString& FieldName, float64 Number)", METHOD_NOJIT(FJsonObjectContainer, SetNumberField));
		JsonObject.Method("void SetBoolField(const FString& FieldName, bool InValue)", METHOD_NOJIT(FJsonObjectContainer, SetBoolField));
		JsonObject.Method("void SetObjectField(const FString& FieldName, const FJsonObject& InObject)", METHOD_NOJIT(FJsonObjectContainer, SetObjectField));
		JsonObject.Method("void SetArrayField(const FString& FieldName, const FJsonArray& InArray)", METHOD_NOJIT(FJsonObjectContainer, SetArrayField));

		JsonObject.Method("bool TryGetStringField(const FString& FieldName, FString& OutString) const", METHOD_NOJIT(FJsonObjectContainer, TryGetStringField));
		JsonObject.Method("bool TryGetNumberField(const FString& FieldName, float64& OutNumber) const", METHOD_NOJIT(FJsonObjectContainer, TryGetNumberField));
		JsonObject.Method("bool TryGetBoolField(const FString& FieldName, bool& bOutBool) const", METHOD_NOJIT(FJsonObjectContainer, TryGetBoolField));
		JsonObject.Method("bool TryGetObjectField(const FString& FieldName, FJsonObject& OutObject) const",
			[](const FJsonObjectContainer& Object, const FString& FieldName, FJsonObjectContainer& OutObject)
			{
				const TSharedPtr<FJsonObject> *ResultPtr;
				if (!Object.JsonObject.IsValid() || !Object.JsonObject->TryGetObjectField(FieldName, ResultPtr) || !ResultPtr)
				{
					return false;
				}
				OutObject.JsonObject = *ResultPtr;
				return true;
			}
		);
		JsonObject.Method("FJsonObject CreateObjectField(const FString& FieldName)", METHOD_NOJIT(FJsonObjectContainer, CreateObjectField));

		JsonObject.Method("bool TryGetArrayField(const FString& FieldName, FJsonArray& OutArray) const",
			[](const FJsonObjectContainer& Object, const FString&FieldName, FJsonValueArrayContainer& OutArray)
			{
				const TArray<TSharedPtr<FJsonValue>>* Array;
				if (!Object.JsonObject.IsValid() || !Object.JsonObject->TryGetArrayField(FieldName, Array))
					return false;
				OutArray.Array = *Array;
				return true;
			});

		JsonObject.Method("bool LoadFromString(const FString& JsonStr)",
			[](FJsonObjectContainer* Object, const FString& JsonStr) -> bool
			{
				if (Object->CheckValidObject())
				{
#if AS_ITERATOR_DEBUGGING
					if (!CheckObjectIteratorDebug(&Object->JsonObject->Values))
						return false;
#endif
					auto JsonReader = TJsonReaderFactory<>::Create(JsonStr);
					return FJsonSerializer::Deserialize(JsonReader, Object->JsonObject);
				}
				return false;
			});
		JsonObject.Method("FString SaveToString(bool bPrettyPrint = true) const",
			[](FJsonObjectContainer* Object, bool bPrettyPrint) -> FString
			{
				if (Object->CheckValidObject())
				{
					FString Result;
					if (bPrettyPrint)
					{
						auto JsonWriter = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Result, 0);
						if (bool Success = FJsonSerializer::Serialize(Object->JsonObject.ToSharedRef(), JsonWriter, true))
							return Result;
					}
					else
					{
						auto JsonWriter = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result, 0);
						if (bool Success = FJsonSerializer::Serialize(Object->JsonObject.ToSharedRef(), JsonWriter, true))
							return Result;
					}
				}
				return TEXT("");
			});

		auto JsonObjectFieldIterator = FAngelscriptBinds::ValueClass<FJsonObjectFieldIterator>("FJsonObjectFieldIterator", {});

		JsonObjectFieldIterator.Destructor("void f()", [](FJsonObjectFieldIterator& Iterator)
			{
#if AS_ITERATOR_DEBUGGING
				if (Iterator.MapPtr != nullptr)
				{
					int RemovedCount = GFieldMapsBeingIterated.RemoveSingleSwap(Iterator.MapPtr);
					check(RemovedCount != 0);
				}
#endif
				Iterator.~FJsonObjectFieldIterator();
			});

		JsonObjectFieldIterator.Method("FString GetFieldName() const", METHOD_NOJIT(FJsonObjectFieldIterator, GetFieldName));
		JsonObjectFieldIterator.Method("EJsonType GetType() const", METHOD_NOJIT(FJsonObjectFieldIterator, GetType));
		JsonObjectFieldIterator.Method("FJsonValue GetValue() const", METHOD_NOJIT(FJsonObjectFieldIterator, GetValue));
		JsonObjectFieldIterator.Property("bool CanProceed", &FJsonObjectFieldIterator::bCanProceed);
		JsonObjectFieldIterator.Method("FJsonObjectFieldIterator& Proceed()", [](FJsonObjectFieldIterator& Iterator) -> FJsonObjectFieldIterator&
			{
				Iterator.bValidValue = (bool)Iterator.Iterator;
				if (!Iterator.bValidValue)
				{
					FAngelscriptManager::Throw("Iterator out of bounds.");
					return Iterator;
				}
				Iterator.CurrentValue = *Iterator.Iterator;
				++Iterator.Iterator;
				Iterator.bCanProceed = (bool)Iterator.Iterator;
				return Iterator;
			});
		JsonObjectFieldIterator.Method("FJsonObjectFieldIterator& Iterate()", [](FJsonObjectFieldIterator& Iterator) -> void*
			{
				Iterator.bValidValue = (bool)Iterator.Iterator;
				if (!Iterator.bValidValue)
					return nullptr;
				Iterator.CurrentValue = *Iterator.Iterator;
				++Iterator.Iterator;
				return &Iterator;
			});

		JsonObject.Method("FJsonObjectFieldIterator Iterator()", [](FJsonObjectContainer* Object) -> FJsonObjectFieldIterator
			{
				if (Object->CheckValidObject())
				{
#if AS_ITERATOR_DEBUGGING
					GFieldMapsBeingIterated.Add(&Object->JsonObject->Values);
#endif
					return FJsonObjectFieldIterator(Object->JsonObject->Values);
				}
				else
				{
					return FJsonObjectFieldIterator();
				}
			});

		// Binds that depend on arrays and objects for values
		JsonValue.Method("bool TryGetArray(FJsonArray& OutArray) const", METHOD_NOJIT(FJsonValueContainer, TryGetArray));
		JsonValue.Method("bool TryGetObject(FJsonObject& Object) const", METHOD_NOJIT(FJsonValueContainer, TryGetObject));
	
		JsonValueArray.Method("void AddObject(FJsonObject Object)", METHOD_NOJIT(FJsonValueArrayContainer, AddObject));

		{
			FAngelscriptBinds::FNamespace ns("Json");
			FAngelscriptBinds::BindGlobalFunction("FString ValueTypeToString(EJsonType T)", FUNC_NOJIT(ValueTypeToString));

			FAngelscriptBinds::BindGlobalFunction("FJsonObject ParseString(const FString& JsonStr)",
				[](const FString& JsonStr) -> FJsonObjectContainer
				{
					TSharedPtr<FJsonObject> ParsedObject = MakeShared<FJsonObject>();
					auto JsonReader = TJsonReaderFactory<>::Create(JsonStr);
					if (FJsonSerializer::Deserialize(JsonReader, ParsedObject))
						return { ParsedObject };
					else
						return { };
				});
		}
});
