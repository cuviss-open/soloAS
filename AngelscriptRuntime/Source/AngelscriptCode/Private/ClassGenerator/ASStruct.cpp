#include "ClassGenerator/ASStruct.h"
                                                                                                
#include "AngelscriptReloadSafety.h"
                                                                                              

                                                                                                                
#include "HAL/CriticalSection.h"
                                                                                                              
#include "Misc/SecureHash.h"
                                                                                                          
#include "Misc/ScopeLock.h"
                                                                                                        

                                                                                                            
#include "AngelscriptExecutionQuiescence.h"
                                                                                                          
#include "AngelscriptManager.h"
#include "AngelscriptInclude.h"

#include "StartAngelscriptHeaders.h"
#include "as_config.h"
#include "as_scriptengine.h"
#include "as_scriptobject.h"
#include "as_objecttype.h"
#include "as_context.h"
#include "EndAngelscriptHeaders.h"
                                                                                                              
struct FASStructOps;

struct FASStructInstanceState
{
	FASStructOps* Owner = nullptr;
	asCObjectType* ScriptType = nullptr;
	asIScriptFunction* EqualsFunction = nullptr;
	asIScriptFunction* ConstructFunction = nullptr;
	asIScriptFunction* HashFunction = nullptr;
};

namespace
{
	thread_local FASStructOps* GActiveASStructOps = nullptr;
	thread_local bool GPreparingASStructOps = false;
	FCriticalSection GASStructInstanceRegistryMutex;
	TMap<const void*, FASStructInstanceState> GASStructInstanceRegistry;

	class FASStructOpsScope
	{
	public:
		explicit FASStructOpsScope(FASStructOps* InOps)
			: Previous(GActiveASStructOps)
		{
			GActiveASStructOps = InOps;
		}

		~FASStructOpsScope()
		{
			GActiveASStructOps = Previous;
		}

	private:
		FASStructOps* Previous;
	};

	bool FindRegisteredASStructState(const void* Data, FASStructInstanceState& OutState)
	{
		if (Data == nullptr)
		{
			return false;
		}

		FScopeLock Lock(&GASStructInstanceRegistryMutex);
		if (FASStructInstanceState* Found = GASStructInstanceRegistry.Find(Data))
		{
			OutState = *Found;
			return true;
		}

		return false;
	}

	FASStructInstanceState MakeASStructInstanceState(FASStructOps* Ops);

	FASStructInstanceState ResolveASStructInstanceState(const void* Data)
	{
		FASStructInstanceState State;
		if (FindRegisteredASStructState(Data, State))
		{
			return State;
		}

		return MakeASStructInstanceState(GActiveASStructOps);
	}

	void RegisterASStructMemory(const FASStructInstanceState& State, void* Data, int32 ArrayDim);
	void UnregisterASStructMemory(FASStructOps* Ops, void* Data, int32 ArrayDim);
}
                                                                                                            

struct FASStructOps : UASStruct::ICppStructOps
{
	UASStruct* Struct;
	asCObjectType* ScriptType;

	asIScriptFunction* EqualsFunction;
	asIScriptFunction* ConstructFunction;
	asIScriptFunction* ToStrFunction;
	asIScriptFunction* HashFunction;

	struct FASFakeVTable : public UE::CoreUObject::Private::FStructOpsFakeVTable
	{
		void* Construct;
#if !(UE_BUILD_TEST || UE_BUILD_SHIPPING)
		void* ConstructForTests;
#endif
		void* Destruct;
		void* Copy;
		void* Identical;
		void* GetStructTypeHash;
	};

	FASFakeVTable FakeVTable;

	FASStructOps(UASStruct* InStruct, int32 InSize, int32 InAlignment)
		: UASStruct::ICppStructOps(InSize, InAlignment)
		, Struct(InStruct)
		, ScriptType((asCObjectType*)InStruct->ScriptType)
	{
		SetFromStruct(InStruct);
		FakeVPtr = &FakeVTable;

		FakeVTable.Flags = 
			UE::CoreUObject::Private::EStructOpsFakeVTableFlags::Construct
#if !(UE_BUILD_TEST || UE_BUILD_SHIPPING)
			| UE::CoreUObject::Private::EStructOpsFakeVTableFlags::ConstructForTests
#endif
			| UE::CoreUObject::Private::EStructOpsFakeVTableFlags::Destruct
			| UE::CoreUObject::Private::EStructOpsFakeVTableFlags::Copy
			| UE::CoreUObject::Private::EStructOpsFakeVTableFlags::Identical
			| UE::CoreUObject::Private::EStructOpsFakeVTableFlags::GetStructTypeHash
		;

                                                                                                                         
#if 0
                                                                                                                       
		FakeVTable.Construct = reinterpret_cast<void*>(&FASStructOps::Construct);
#if !(UE_BUILD_TEST || UE_BUILD_SHIPPING)
		FakeVTable.ConstructForTests = reinterpret_cast<void*>(&FASStructOps::Construct);
#endif
		FakeVTable.Destruct = reinterpret_cast<void*>(&FASStructOps::Destruct);
		FakeVTable.Copy = reinterpret_cast<void*>(&FASStructOps::Copy);
		FakeVTable.Identical = reinterpret_cast<void*>(&FASStructOps::Identical);
		FakeVTable.GetStructTypeHash = reinterpret_cast<void*>(&FASStructOps::GetStructTypeHash);
                                                                                                                     
#endif
		using FConstructCallback =
			typename UE::CoreUObject::Private::THandlingFunctionType<
				UE::CoreUObject::Private::EStructOpsFakeVTableFlags::Construct>::Type*;
		FakeVTable.Construct = reinterpret_cast<void*>(static_cast<FConstructCallback>(&FASStructOps::Construct));
#if !(UE_BUILD_TEST || UE_BUILD_SHIPPING)
		using FConstructForTestsCallback =
			typename UE::CoreUObject::Private::THandlingFunctionType<
				UE::CoreUObject::Private::EStructOpsFakeVTableFlags::ConstructForTests>::Type*;
		FakeVTable.ConstructForTests = reinterpret_cast<void*>(static_cast<FConstructForTestsCallback>(&FASStructOps::Construct));
#endif
		using FDestructCallback =
			typename UE::CoreUObject::Private::THandlingFunctionType<
				UE::CoreUObject::Private::EStructOpsFakeVTableFlags::Destruct>::Type*;
		using FCopyCallback = typename UE::CoreUObject::Private::THandlingFunctionType<UE::CoreUObject::Private::EStructOpsFakeVTableFlags::Copy>::Type*;
		using FIdenticalCallback =
			typename UE::CoreUObject::Private::THandlingFunctionType<
				UE::CoreUObject::Private::EStructOpsFakeVTableFlags::Identical>::Type*;
		using FGetStructTypeHashCallback =
			typename UE::CoreUObject::Private::THandlingFunctionType<
				UE::CoreUObject::Private::EStructOpsFakeVTableFlags::GetStructTypeHash>::Type*;
		FakeVTable.Destruct = reinterpret_cast<void*>(static_cast<FDestructCallback>(&FASStructOps::Destruct));
		FakeVTable.Copy = reinterpret_cast<void*>(static_cast<FCopyCallback>(&FASStructOps::Copy));
		FakeVTable.Identical = reinterpret_cast<void*>(static_cast<FIdenticalCallback>(&FASStructOps::Identical));
		FakeVTable.GetStructTypeHash = reinterpret_cast<void*>(static_cast<FGetStructTypeHashCallback>(&FASStructOps::GetStructTypeHash));
                                                                                                                   

		FMemory::Memzero(FakeVTable.Capabilities);
		FakeVTable.Capabilities.HasDestructor = true;
		FakeVTable.Capabilities.HasCopy = true;
		FakeVTable.Capabilities.HasIdentical = (EqualsFunction != nullptr);
		FakeVTable.Capabilities.HasGetTypeHash = (HashFunction != nullptr);
		FakeVTable.Capabilities.ComputedPropertyFlags |= (HashFunction != nullptr) ? CPF_HasGetValueTypeHash : CPF_None;

                                                                                                                  
#if 0
                                                                                                                
		if ((ScriptType->flags & asOBJ_POD) != 0)
		{
			FakeVTable.Capabilities.IsPlainOldData = true;
			FakeVTable.Capabilities.ComputedPropertyFlags |= CPF_IsPlainOldData;
		}
                                                                                                                  
#endif
		// Even POD script values need AS type identity and registry bookkeeping.
		// UE must run the construct and destroy callbacks for every script struct.
                                                                                                                
	}

	void SetFromStruct(UASStruct* InStruct)
	{
		check(InStruct == Struct);
		ScriptType = (asCObjectType*)InStruct->ScriptType;

		if (ScriptType != nullptr)
		{
			auto& Manager = FAngelscriptManager::Get();
			if (ScriptType->beh.construct != 0)
				ConstructFunction = Manager.Engine->GetFunctionById(ScriptType->beh.construct);
			else
				ConstructFunction = nullptr;

			if (ScriptType->GetFirstMethod("opEquals") != nullptr)
			{
				FString StructName = ANSI_TO_TCHAR(ScriptType->GetName());
				FString EqualsDecl = FString::Printf(TEXT("bool opEquals(const %s& Other) const"), *StructName);
				EqualsFunction = ScriptType->GetMethodByDecl(TCHAR_TO_ANSI(*EqualsDecl));
			}
			else
			{
				EqualsFunction = nullptr;
			}

			if (ScriptType->GetFirstMethod("ToString") != nullptr)
			{
				ToStrFunction = ScriptType->GetMethodByDecl("FString ToString() const");
			}
			else
			{
				ToStrFunction = nullptr;
			}

			if (ScriptType->GetFirstMethod("Hash") != nullptr)
			{
				const FString HashDecl = TEXT("uint32 Hash() const");
				HashFunction = ScriptType->GetMethodByDecl(TCHAR_TO_ANSI(*HashDecl));
			}
			else
			{
				HashFunction = nullptr;
			}
		}
		else
		{
			EqualsFunction = nullptr;
			ConstructFunction = nullptr;
			ToStrFunction = nullptr;
			HashFunction = nullptr;
		}
	}

                                                                                                                     
#if 0
	// The locked upstream callback body remains outside this hook range.
                                                                                                                   
	static void Construct(FASStructOps* Ops, void *Dest)
	{
		if (Ops->ScriptType == nullptr)
		{
			FMemory::Memzero(Dest, Ops->GetSize());
			return;
		}

		new(Dest) asCScriptObject(Ops->ScriptType);

		if (Ops->ConstructFunction != nullptr)
		{
			// Call the angelscript constructor of the scriptobject
			FAngelscriptContext Context;
			Context->Prepare(Ops->ConstructFunction);
			Context->SetObject((asIScriptObject*)Dest);
			Context->Execute();
		}
		else
		{
			FMemory::Memzero(Dest, Ops->GetSize());
		}
	}

	static void Destruct(FASStructOps* Ops, void *Dest)
	{
		if (Ops->ScriptType == nullptr)
		{
			FMemory::Memzero(Dest, Ops->GetSize());
			return;
		}

		auto* ScriptObject = (asCScriptObject*)(Dest);
		ScriptObject->CallDestructor(Ops->ScriptType);
	}

	static bool Copy(FASStructOps* Ops, void* Dest, void const* Src, int32 ArrayDim)
	{
		if (Ops->ScriptType == nullptr)
			return true;

		auto* DestObject = (asCScriptObject*)(Dest);
		auto* SourceObject = (asCScriptObject*)(Src);
		DestObject->PerformCopy(SourceObject, Ops->ScriptType);
		return true;
	}

	static bool Identical(FASStructOps* Ops, const void* A, const void* B, uint32 PortFlags, bool& bOutResult)
	{
		if (Ops->ScriptType == nullptr)
			return false;
		if (Ops->EqualsFunction == nullptr)
			return false;

		// Call the angelscript compare function
		FAngelscriptContext Context;
		Context->Prepare(Ops->EqualsFunction);
		Context->SetObject((asIScriptObject*)A);
		Context->SetArgAddress(0, (void*)B);
		Context->Execute();
		bOutResult = (Context->GetReturnByte() != 0);
		return true;
	}

	static uint32 GetStructTypeHash(FASStructOps* Ops, const void* Src)
	{
		if (Ops->HashFunction == nullptr)
			return 0;

		FAngelscriptContext Context;
		Context->Prepare(Ops->HashFunction);
		Context->SetObject(const_cast<void*>(Src));
		Context->Execute();
		return Context->GetReturnDWord();
	}
                                                                                                                               
#endif
	static void Construct(void* Dest)
	{
		FASStructInstanceState ExistingState;
		const bool bHasExistingState = FindRegisteredASStructState(Dest, ExistingState);
		FASStructOps* Ops = GActiveASStructOps;
		if (Ops == nullptr && bHasExistingState)
		{
			// UScriptStruct::ClearScriptStruct invokes Destruct and Construct
			// back-to-back without entering UASStruct::InitializeStruct. Keep the
			// instance identity available across that pair.
			Ops = ExistingState.Owner;
		}
		if (Ops == nullptr)
		{
			return;
		}

		const FASStructInstanceState State = GActiveASStructOps != nullptr
			? MakeASStructInstanceState(Ops)
			: ExistingState;
		if (State.ScriptType == nullptr)
		{
			FMemory::Memzero(Dest, Ops->GetSize());
			return;
		}

		new(Dest) asCScriptObject(State.ScriptType);
		RegisterASStructMemory(State, Dest, 1);

		if (State.ConstructFunction != nullptr)
		{
			FAngelscriptContext Context;
			Context->Prepare(State.ConstructFunction);
			Context->SetObject((asIScriptObject*)Dest);
			const int32 ExecutionResult = Context->Execute();
			AngelscriptReloadSafety::CheckScriptExecution(ExecutionResult, TEXT("struct_construct"));
		}
		else
		{
			FMemory::Memzero(Dest, Ops->GetSize());
		}
	}

	static void Construct(void* StructOps, void* Dest)
	{
		FASStructOpsScope Scope(static_cast<FASStructOps*>(StructOps));
		Construct(Dest);
	}
                                                                                                                             
                                                                                                                              
	static void Destruct(void* Dest)
	{
		if (IsAngelscriptExecutionShutdownStarted())
		{
			return;
		}
		FASStructInstanceState State = ResolveASStructInstanceState(Dest);
		if (State.Owner == nullptr || State.ScriptType == nullptr)
		{
			UnregisterASStructMemory(State.Owner, Dest, 1);
			return;
		}

		auto* ScriptObject = (asCScriptObject*)Dest;
		ScriptObject->CallDestructor(State.ScriptType);
		// VM destruction retires the value's address. ClearScriptStruct invokes
		// Construct immediately without an ops scope, so retain its type until
		// that callback; UASStruct::DestroyStruct removes it after Super returns.
		if (!GPreparingASStructOps)
			RegisterASStructMemory(State, Dest, 1);
	}

	static void Destruct(void* StructOps, void* Dest)
	{
		FASStructOpsScope Scope(static_cast<FASStructOps*>(StructOps));
		Destruct(Dest);
	}
                                                                                                                            
                                                                                                                          
	static bool Copy(void* Dest, void const* Src, int32 ArrayDim)
	{
		FASStructInstanceState SourceState;
		FASStructInstanceState DestState;
		const bool bHasSourceState = FindRegisteredASStructState(Src, SourceState);
		const bool bHasDestState = FindRegisteredASStructState(Dest, DestState);
		if (bHasSourceState && bHasDestState && SourceState.Owner != DestState.Owner)
		{
			return false;
		}

		FASStructInstanceState State = bHasSourceState
			? SourceState
			: (bHasDestState ? DestState : ResolveASStructInstanceState(Src != nullptr ? Src : Dest));
		if (State.Owner == nullptr || State.ScriptType == nullptr)
		{
			return false;
		}

		const int32 Stride = State.Owner->GetSize();
		for (int32 ArrayIndex = 0; ArrayIndex < ArrayDim; ++ArrayIndex)
		{
			auto* DestObject = (asCScriptObject*)((uint8*)Dest + ArrayIndex * Stride);
			auto* SourceObject = (asCScriptObject*)((const uint8*)Src + ArrayIndex * Stride);
			DestObject->PerformCopy(SourceObject, State.ScriptType);
			RegisterASStructMemory(State, DestObject, 1);
		}
		return true;
	}

	static bool Copy(void* StructOps, void* Dest, void const* Src, int32 ArrayDim)
	{
		FASStructOpsScope Scope(static_cast<FASStructOps*>(StructOps));
		return Copy(Dest, Src, ArrayDim);
	}
                                                                                                                        
                                                                                                                               
	static bool Identical(const void* A, const void* B, uint32 PortFlags, bool& bOutResult)
	{
		FASStructInstanceState AState;
		FASStructInstanceState BState;
		const bool bHasAState = FindRegisteredASStructState(A, AState);
		const bool bHasBState = FindRegisteredASStructState(B, BState);
		if (bHasAState && bHasBState && AState.Owner != BState.Owner)
		{
			return false;
		}

		FASStructInstanceState State = bHasAState
			? AState
			: (bHasBState ? BState : ResolveASStructInstanceState(A != nullptr ? A : B));
		if (State.Owner == nullptr || State.ScriptType == nullptr || State.EqualsFunction == nullptr)
		{
			return false;
		}

		FAngelscriptContext Context;
		Context->Prepare(State.EqualsFunction);
		Context->SetObject((asIScriptObject*)A);
		Context->SetArgAddress(0, (void*)B);
		const int32 ExecutionResult = Context->Execute();
		AngelscriptReloadSafety::CheckScriptExecution(ExecutionResult, TEXT("struct_compare"));
		bOutResult = (Context->GetReturnByte() != 0);
		return true;
	}

	static bool Identical(void* StructOps, const void* A, const void* B, uint32 PortFlags, bool& bOutResult)
	{
		FASStructOpsScope Scope(static_cast<FASStructOps*>(StructOps));
		return Identical(A, B, PortFlags, bOutResult);
	}
                                                                                                                             
                                                                                                                          
	static uint32 GetStructTypeHash(const void* Src)
	{
		FASStructInstanceState State = ResolveASStructInstanceState(Src);
		if (State.Owner == nullptr || State.HashFunction == nullptr)
		{
			return 0;
		}

		FAngelscriptContext Context;
		Context->Prepare(State.HashFunction);
		Context->SetObject(const_cast<void*>(Src));
		const int32 ExecutionResult = Context->Execute();
		AngelscriptReloadSafety::CheckScriptExecution(ExecutionResult, TEXT("struct_hash"));
		return Context->GetReturnDWord();
	}

	static uint32 GetStructTypeHash(void* StructOps, const void* Src)
	{
		FASStructOpsScope Scope(static_cast<FASStructOps*>(StructOps));
		return GetStructTypeHash(Src);
	}
                                                                                                                        
};
                                                                                                             
namespace
{
	FASStructInstanceState MakeASStructInstanceState(FASStructOps* Ops)
	{
		FASStructInstanceState State;
		State.Owner = Ops;
		if (Ops != nullptr)
		{
			State.ScriptType = Ops->ScriptType;
			State.EqualsFunction = Ops->EqualsFunction;
			State.ConstructFunction = Ops->ConstructFunction;
			State.HashFunction = Ops->HashFunction;
		}
		return State;
	}

	void RegisterASStructMemory(const FASStructInstanceState& State, void* Data, int32 ArrayDim)
	{
		if (State.Owner == nullptr || Data == nullptr || ArrayDim <= 0)
		{
			return;
		}

		FScopeLock Lock(&GASStructInstanceRegistryMutex);
		uint8* Element = static_cast<uint8*>(Data);
		for (int32 ArrayIndex = 0; ArrayIndex < ArrayDim; ++ArrayIndex)
		{
			GASStructInstanceRegistry.Add(Element + ArrayIndex * State.Owner->GetSize(), State);
		}
	}

	void UnregisterASStructMemory(FASStructOps* Ops, void* Data, int32 ArrayDim)
	{
		if (Ops == nullptr || Data == nullptr || ArrayDim <= 0)
		{
			return;
		}

		FScopeLock Lock(&GASStructInstanceRegistryMutex);
		uint8* Element = static_cast<uint8*>(Data);
		for (int32 ArrayIndex = 0; ArrayIndex < ArrayDim; ++ArrayIndex)
		{
			const void* Address = Element + ArrayIndex * Ops->GetSize();
			if (FASStructInstanceState* Found = GASStructInstanceRegistry.Find(Address))
			{
				if (Found->Owner == Ops)
				{
					GASStructInstanceRegistry.Remove(Address);
				}
			}
		}
	}
}

ANGELSCRIPTCODE_API void ForgetAngelscriptStructScriptType(const void* Data, const asITypeInfo* /*ScriptType*/)
{
	FScopeLock Lock(&GASStructInstanceRegistryMutex);
	// The address is the lifetime key; hot reload can retire it through a replacement type.
	GASStructInstanceRegistry.Remove(Data);
}

ANGELSCRIPTCODE_API asITypeInfo* GetAngelscriptStructScriptType(const void* Data)
{
	FASStructInstanceState State;
	return FindRegisteredASStructState(Data, State) ? State.ScriptType : nullptr;
}
                                                                                                           

                                                                                                           
int32 UASStruct::GetLiveInstanceCount() const
{
	UASStruct* NewestStruct = const_cast<UASStruct*>(this)->ResolveNewestStruct();
	if (NewestStruct == nullptr)
	{
		return 0;
	}

	FScopeLock Lock(&GASStructInstanceRegistryMutex);
	int32 Count = 0;
	for (const TPair<const void*, FASStructInstanceState>& Pair : GASStructInstanceRegistry)
	{
		FASStructOps* Owner = Pair.Value.Owner;
		if (Owner != nullptr && Owner->Struct != nullptr
			&& Owner->Struct->ResolveNewestStruct() == NewestStruct)
		{
			++Count;
		}
	}
	return Count;
}
                                                                                                         

UASStruct::UASStruct(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

UASStruct::ICppStructOps* UASStruct::CreateCppStructOps()
{
	return new FASStructOps(this, GetPropertiesSize(), GetMinAlignment());
}
                                                                                                             
void UASStruct::InitializeStruct(void* Dest, int32 ArrayDim) const
{
	FASStructOps* Ops = static_cast<FASStructOps*>(GetCppStructOps());
	FASStructOpsScope Scope(Ops);
	Super::InitializeStruct(Dest, ArrayDim);
	RegisterASStructMemory(MakeASStructInstanceState(Ops), Dest, ArrayDim);
}

void UASStruct::DestroyStruct(void* Dest, int32 ArrayDim) const
{
	FASStructOps* Ops = static_cast<FASStructOps*>(GetCppStructOps());
	FASStructOpsScope Scope(Ops);
	Super::DestroyStruct(Dest, ArrayDim);
	UnregisterASStructMemory(Ops, Dest, ArrayDim);
}

uint32 UASStruct::GetStructTypeHash(const void* Src) const
{
	FASStructOps* Ops = static_cast<FASStructOps*>(GetCppStructOps());
	if (Ops == nullptr || Ops->HashFunction == nullptr)
	{
		return 0;
	}

	FAngelscriptContext Context;
	Context->Prepare(Ops->HashFunction);
	Context->SetObject(const_cast<void*>(Src));
	Context->Execute();
	return Context->GetReturnDWord();
}
                                                                                                           

                                                                                                        
#if 0
                                                                                                      
void UASStruct::PrepareCppStructOps()
{
	if (CppStructOps == nullptr)
		SetCppStructOps(CreateCppStructOps());
	Super::PrepareCppStructOps();
}
                                                                                                       
#endif
void UASStruct::SetAngelscriptCppStructOps(ICppStructOps* InCppStructOps)
{
	checkf(InCppStructOps != nullptr, TEXT("UASStruct requires non-null AngelScript CppStructOps."));
	checkf(CppStructOps == nullptr, TEXT("UASStruct CppStructOps may only be assigned once."));
	CppStructOps = InCppStructOps;
}

void UASStruct::PrepareCppStructOps()
{
	if (CppStructOps == nullptr)
		SetAngelscriptCppStructOps(CreateCppStructOps());
	FASStructOpsScope Scope(static_cast<FASStructOps*>(CppStructOps));
	const bool PreviousPreparing = GPreparingASStructOps;
	GPreparingASStructOps = true;
	Super::PrepareCppStructOps();
	GPreparingASStructOps = PreviousPreparing;
}
                                                                                                     

void UASStruct::UpdateScriptType()
{
	FASStructOps* Ops = ((FASStructOps*)GetCppStructOps());
	Ops->SetFromStruct(this);

	// Update struct flags that may have changed in the new script type
	if (Ops->EqualsFunction != nullptr)
		StructFlags = EStructFlags(StructFlags | STRUCT_IdenticalNative);
	else
		StructFlags = EStructFlags(StructFlags & ~STRUCT_IdenticalNative);
}

asIScriptFunction* UASStruct::GetToStringFunction() const
{
	if (ICppStructOps* StructOps = GetCppStructOps())
	{
		return ((FASStructOps*)StructOps)->ToStrFunction;
	}

	return nullptr;
}

void UASStruct::SetGuid(FName FromName)
{
	FString HashString = TEXT("Script:");
	HashString += FromName.ToString();

	ensure(HashString.Len());

	const uint32 BufferLength = HashString.Len() * sizeof(HashString[0]);
	uint32 HashBuffer[5];
	FSHA1::HashBuffer(*HashString, BufferLength, reinterpret_cast<uint8*>(HashBuffer));
	Guid = FGuid(HashBuffer[1], HashBuffer[2], HashBuffer[3], HashBuffer[4]);
}
