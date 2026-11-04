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
	// The ops whose callback UE invoked; null in the shared callbacks that name no ops.
	thread_local FASStructOps* GInvokedASStructOps = nullptr;
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

	class FASInvokedStructOpsScope
	{
	public:
		explicit FASInvokedStructOpsScope(FASStructOps* InOps)
			: Previous(GInvokedASStructOps)
		{
			GInvokedASStructOps = InOps;
		}

		~FASInvokedStructOpsScope()
		{
			GInvokedASStructOps = Previous;
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
	void RemoveOtherASStructRegistration(FASStructOps* Ops, const void* Data);

	// A registration describes the value at its address only to the ops that registered it. Script value types
	// lay out from offset 0, so a value and its first member share an address, and a value moved bitwise leaves
	// its registration behind for whatever arrives at that address later. Like the fork, any other ops treats
	// the address as its own type; a lifetime callback of another ops also drops the registration.
	bool FindInvokedASStructState(FASStructOps* Invoked, const void* Data, FASStructInstanceState& OutState, bool bDropOther)
	{
		if (!FindRegisteredASStructState(Data, OutState))
		{
			return false;
		}
		if (Invoked == nullptr || OutState.Owner == Invoked)
		{
			return true;
		}

		if (bDropOther)
		{
			RemoveOtherASStructRegistration(Invoked, Data);
		}
		OutState = FASStructInstanceState();
		return false;
	}

	FASStructInstanceState ResolveASStructInstanceState(const void* Data, bool bDropOther)
	{
		FASStructOps* const Invoked = GInvokedASStructOps;
		FASStructInstanceState State;
		if (FindInvokedASStructState(Invoked, Data, State, bDropOther))
		{
			return State;
		}

		return MakeASStructInstanceState(Invoked != nullptr ? Invoked : GActiveASStructOps);
	}

	void RegisterASStructMemory(const FASStructInstanceState& State, void* Data, int32 ArrayDim);
	void UnregisterASStructMemory(FASStructOps* Ops, void* Data, int32 ArrayDim);
}
                                                                                                            

                                                                                      
#include "Compat/SoloASStructOpsCompat.h"
struct FASStructOps : SoloASBridge::Compat::FScriptStructOps
                                                                                    
{
	UASStruct* Struct;
	asCObjectType* ScriptType;

	asIScriptFunction* EqualsFunction;
	asIScriptFunction* ConstructFunction;
	asIScriptFunction* ToStrFunction;
	asIScriptFunction* HashFunction;

                                                                                       
// SOLOAS-COMPAT-BEGIN(<5.7) UE 5.6 stores capabilities on the virtual operations object [test: SoloAS.ScriptStructOperations.CarrierAndLifecycle]
#if SOLOAS_UE_BEFORE(5, 7)
    struct { FCapabilities Capabilities; } FakeVTable;
    FCapabilities GetCapabilities() const override { return FakeVTable.Capabilities; }
    void Construct(void* Dest) override { FASInvokedStructOpsScope Scope(this); ConstructValue(Dest); }
    void ConstructForTests(void* Dest) override { FASInvokedStructOpsScope Scope(this); ConstructValue(Dest); }
    void Destruct(void* Dest) override { FASInvokedStructOpsScope Scope(this); DestructValue(Dest); }
    bool Copy(void* Dest, const void* Src, int32 Count) override { FASInvokedStructOpsScope Scope(this); return CopyValue(Dest, Src, Count); }
    bool Identical(const void* A, const void* B, uint32 Flags, bool& Result) override
    {
        FASInvokedStructOpsScope Scope(this);
        return IdenticalValue(A, B, Flags, Result);
    }
    uint32 GetStructTypeHash(const void* Src) override { FASInvokedStructOpsScope Scope(this); return GetStructTypeHashValue(Src); }
#else
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

#endif
// SOLOAS-COMPAT-END
                                                                                     

	FASStructOps(UASStruct* InStruct, int32 InSize, int32 InAlignment)
                                                                                                                
		: SoloASBridge::Compat::FScriptStructOps(InSize, InAlignment)
                                                                                                              
		, Struct(InStruct)
		, ScriptType((asCObjectType*)InStruct->ScriptType)
	{
		SetFromStruct(InStruct);
// SOLOAS-COMPAT-BEGIN(<5.7) Only UE 5.7+ has the fake-vtable constructor ABI [test: SoloAS.ScriptStructOperations.CarrierAndLifecycle]
#if SOLOAS_UE_AT_LEAST(5, 7)
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
		// Each ops routes the installed callbacks through thunks that name it; only an exhausted pool keeps the shared ones.
		if (const FInvokedCallbackSet* Invoked = ClaimInvokedCallbackSet(this))
		{
			auto Route = [](void*& Callback, void* Thunk) { if (Callback != nullptr) { Callback = Thunk; } };
			Route(FakeVTable.Construct, reinterpret_cast<void*>(Invoked->Construct));
#if !(UE_BUILD_TEST || UE_BUILD_SHIPPING)
			Route(FakeVTable.ConstructForTests, reinterpret_cast<void*>(Invoked->Construct));
#endif
			Route(FakeVTable.Destruct, reinterpret_cast<void*>(Invoked->Destruct));
			Route(FakeVTable.Copy, reinterpret_cast<void*>(Invoked->Copy));
			Route(FakeVTable.Identical, reinterpret_cast<void*>(Invoked->Identical));
			Route(FakeVTable.GetStructTypeHash, reinterpret_cast<void*>(Invoked->GetStructTypeHash));
		}
                                                                                                                   

#endif
// SOLOAS-COMPAT-END

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

                                                                                                    
// SOLOAS-COMPAT-BEGIN(<5.7) Legacy virtual methods cannot share the stateless SDK callback signatures [test: SoloAS.ScriptStructOperations.CarrierAndLifecycle]
#if SOLOAS_UE_AT_LEAST(5, 7)
    // Shared callbacks name no ops: a value's registration alone identifies its type.
    static void Construct(void* Dest) { FASInvokedStructOpsScope Scope(nullptr); ConstructValue(Dest); }
    static void Destruct(void* Dest) { FASInvokedStructOpsScope Scope(nullptr); DestructValue(Dest); }
    static bool Copy(void* Dest, const void* Src, int32 Count) { FASInvokedStructOpsScope Scope(nullptr); return CopyValue(Dest, Src, Count); }
    static bool Identical(const void* A, const void* B, uint32 Flags, bool& Result)
    {
        FASInvokedStructOpsScope Scope(nullptr);
        return IdenticalValue(A, B, Flags, Result);
    }
    static uint32 GetStructTypeHash(const void* Src) { FASInvokedStructOpsScope Scope(nullptr); return GetStructTypeHashValue(Src); }

    // The fork's callbacks receive their ops; the stock SDK's do not. Each ops therefore binds one slot of
    // callbacks, and the slot names the ops for the callback.
    static constexpr int32 InvokedCallbackSlotCount = 4096;
    using FInvokedConstructCallback = typename UE::CoreUObject::Private::THandlingFunctionType<
        UE::CoreUObject::Private::EStructOpsFakeVTableFlags::Construct>::Type*;
    using FInvokedDestructCallback = typename UE::CoreUObject::Private::THandlingFunctionType<
        UE::CoreUObject::Private::EStructOpsFakeVTableFlags::Destruct>::Type*;
    using FInvokedCopyCallback = typename UE::CoreUObject::Private::THandlingFunctionType<
        UE::CoreUObject::Private::EStructOpsFakeVTableFlags::Copy>::Type*;
    using FInvokedIdenticalCallback = typename UE::CoreUObject::Private::THandlingFunctionType<
        UE::CoreUObject::Private::EStructOpsFakeVTableFlags::Identical>::Type*;
    using FInvokedHashCallback = typename UE::CoreUObject::Private::THandlingFunctionType<
        UE::CoreUObject::Private::EStructOpsFakeVTableFlags::GetStructTypeHash>::Type*;

    struct FInvokedCallbackSet
    {
        FInvokedConstructCallback Construct;
        FInvokedDestructCallback Destruct;
        FInvokedCopyCallback Copy;
        FInvokedIdenticalCallback Identical;
        FInvokedHashCallback GetStructTypeHash;
    };

    inline static FASStructOps* InvokedCallbackOwners[InvokedCallbackSlotCount] = {};
    inline static int32 NextInvokedCallbackSlot = 0;

    static FORCENOINLINE void ConstructInSlot(int32 Slot, void* Dest)
    {
        FASInvokedStructOpsScope Scope(InvokedCallbackOwners[Slot]);
        ConstructValue(Dest);
    }

    static FORCENOINLINE void DestructInSlot(int32 Slot, void* Dest)
    {
        FASInvokedStructOpsScope Scope(InvokedCallbackOwners[Slot]);
        DestructValue(Dest);
    }

    static FORCENOINLINE bool CopyInSlot(int32 Slot, void* Dest, const void* Src, int32 Count)
    {
        FASInvokedStructOpsScope Scope(InvokedCallbackOwners[Slot]);
        return CopyValue(Dest, Src, Count);
    }

    static FORCENOINLINE bool IdenticalInSlot(int32 Slot, const void* A, const void* B, uint32 Flags, bool& Result)
    {
        FASInvokedStructOpsScope Scope(InvokedCallbackOwners[Slot]);
        return IdenticalValue(A, B, Flags, Result);
    }

    static FORCENOINLINE uint32 GetStructTypeHashInSlot(int32 Slot, const void* Src)
    {
        FASInvokedStructOpsScope Scope(InvokedCallbackOwners[Slot]);
        return GetStructTypeHashValue(Src);
    }

    template <int32 Slot>
    struct TInvokedCallbacks
    {
        static void Construct(void* Dest) { ConstructInSlot(Slot, Dest); }
        static void Destruct(void* Dest) { DestructInSlot(Slot, Dest); }
        static bool Copy(void* Dest, const void* Src, int32 Count) { return CopyInSlot(Slot, Dest, Src, Count); }
        static bool Identical(const void* A, const void* B, uint32 Flags, bool& Result) { return IdenticalInSlot(Slot, A, B, Flags, Result); }
        static uint32 GetStructTypeHash(const void* Src) { return GetStructTypeHashInSlot(Slot, Src); }
    };

    template <int32... Slots>
    static const FInvokedCallbackSet* MakeInvokedCallbackSets(TIntegerSequence<int32, Slots...>)
    {
        static const FInvokedCallbackSet Sets[] = {
            { &TInvokedCallbacks<Slots>::Construct, &TInvokedCallbacks<Slots>::Destruct, &TInvokedCallbacks<Slots>::Copy,
                &TInvokedCallbacks<Slots>::Identical, &TInvokedCallbacks<Slots>::GetStructTypeHash }... };
        return Sets;
    }

    static const FInvokedCallbackSet* ClaimInvokedCallbackSet(FASStructOps* Ops)
    {
        static const FInvokedCallbackSet* const Sets = MakeInvokedCallbackSets(TMakeIntegerSequence<int32, InvokedCallbackSlotCount>());
        const int32 Slot = FPlatformAtomics::InterlockedIncrement(&NextInvokedCallbackSlot) - 1;
        if (Slot >= InvokedCallbackSlotCount)
        {
            if (Slot == InvokedCallbackSlotCount)
            {
                UE_LOG(Angelscript, Warning, TEXT("AS struct operation callback slots are exhausted; later struct ops identify values by registration only."));
            }
            return nullptr;
        }
        InvokedCallbackOwners[Slot] = Ops;
        return &Sets[Slot];
    }
#endif
// SOLOAS-COMPAT-END
                                                                                                  

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
	static void ConstructValue(void* Dest)
	{
		FASStructOps* const Invoked = GInvokedASStructOps;
		// An enclosing scope of another ops (a script constructor inside its InitializeStruct) is not this callback's.
		FASStructOps* const Active = Invoked == nullptr || GActiveASStructOps == Invoked ? GActiveASStructOps : nullptr;
		FASStructInstanceState ExistingState;
		const bool bHasExistingState = FindInvokedASStructState(Invoked, Dest, ExistingState, true);
		FASStructOps* Ops = Active;
		if (Ops == nullptr && bHasExistingState)
		{
			// UScriptStruct::ClearScriptStruct invokes Destruct and Construct
			// back-to-back without entering UASStruct::InitializeStruct. Keep the
			// instance identity available across that pair.
			Ops = ExistingState.Owner;
		}
		if (Ops == nullptr && Invoked != nullptr && Invoked->ScriptType != nullptr)
		{
			// As in the fork, a value no registration describes is constructed by the invoked ops.
			Ops = Invoked;
		}
		if (Ops == nullptr)
		{
			return;
		}

		const FASStructInstanceState State = Active != nullptr || !bHasExistingState
			? MakeASStructInstanceState(Ops)
			: ExistingState;
		if (State.ScriptType == nullptr || Ops->ScriptType == nullptr)
		{
			// Like the fork, a struct without a current type constructs nothing; a registration this ops kept for
			// the address (a retired type held across ClearScriptStruct's destruct) no longer describes it.
			UnregisterASStructMemory(Ops, Dest, 1);
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
		ConstructValue(Dest);
	}
                                                                                                                             
                                                                                                                              
	static void DestructValue(void* Dest)
	{
		if (IsAngelscriptExecutionShutdownStarted())
		{
			return;
		}
		FASStructInstanceState State = ResolveASStructInstanceState(Dest, true);
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
		DestructValue(Dest);
	}
                                                                                                                            
                                                                                                                          
	static bool CopyValue(void* Dest, void const* Src, int32 ArrayDim)
	{
		FASStructOps* const Invoked = GInvokedASStructOps;
		FASStructInstanceState SourceState;
		FASStructInstanceState DestState;
		// A copy ends no lifetime: another ops' registration at either address stays where it is.
		const bool bHasSourceState = FindInvokedASStructState(Invoked, Src, SourceState, false);
		const bool bDestRegistered = FindRegisteredASStructState(Dest, DestState);
		const bool bHasDestState = bDestRegistered && (Invoked == nullptr || DestState.Owner == Invoked);
		if (bHasSourceState && bHasDestState && SourceState.Owner != DestState.Owner)
		{
			return false;
		}

		FASStructInstanceState State = bHasSourceState
			? SourceState
			: (bHasDestState ? DestState : ResolveASStructInstanceState(Src != nullptr ? Src : Dest, false));
		if (State.Owner == nullptr || State.ScriptType == nullptr)
		{
			return false;
		}

		// The destination keeps a registration of this ops, or takes one when it now holds a type the ops no longer
		// constructs; a member copied into the storage of another ops' value leaves that value's registration.
		const bool bRegisterDest = Invoked == nullptr || bHasDestState || (!bDestRegistered && State.ScriptType != Invoked->ScriptType);
		const int32 Stride = State.Owner->GetSize();
		for (int32 ArrayIndex = 0; ArrayIndex < ArrayDim; ++ArrayIndex)
		{
			auto* DestObject = (asCScriptObject*)((uint8*)Dest + ArrayIndex * Stride);
			auto* SourceObject = (asCScriptObject*)((const uint8*)Src + ArrayIndex * Stride);
			DestObject->PerformCopy(SourceObject, State.ScriptType);
			if (bRegisterDest)
			{
				RegisterASStructMemory(State, DestObject, 1);
			}
		}
		return true;
	}

	static bool Copy(void* StructOps, void* Dest, void const* Src, int32 ArrayDim)
	{
		FASStructOpsScope Scope(static_cast<FASStructOps*>(StructOps));
		return CopyValue(Dest, Src, ArrayDim);
	}
                                                                                                                        
                                                                                                                               
	static bool IdenticalValue(const void* A, const void* B, uint32 PortFlags, bool& bOutResult)
	{
		FASStructOps* const Invoked = GInvokedASStructOps;
		FASStructInstanceState AState;
		FASStructInstanceState BState;
		const bool bHasAState = FindInvokedASStructState(Invoked, A, AState, false);
		const bool bHasBState = FindInvokedASStructState(Invoked, B, BState, false);
		if (bHasAState && bHasBState && AState.Owner != BState.Owner)
		{
			return false;
		}

		FASStructInstanceState State = bHasAState
			? AState
			: (bHasBState ? BState : ResolveASStructInstanceState(A != nullptr ? A : B, false));
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
		return IdenticalValue(A, B, PortFlags, bOutResult);
	}
                                                                                                                             
                                                                                                                          
	static uint32 GetStructTypeHashValue(const void* Src)
	{
		FASStructInstanceState State = ResolveASStructInstanceState(Src, false);
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
		return GetStructTypeHashValue(Src);
	}
                                                                                                                        
};
                                                                                                             
namespace
{
	// Registered values per captured struct ops and script type. A value keeps calling the type
	// it was constructed with, so its module stays leased until the last such value is gone.
	TMap<TPair<FASStructOps*, asCObjectType*>, int32> GASStructLiveTypes;

	// The registry mutex is held by the callers below.
	void CountASStructLiveType(const FASStructInstanceState& State, int32 Delta)
	{
		if (State.Owner == nullptr || State.ScriptType == nullptr)
		{
			return;
		}

		const TPair<FASStructOps*, asCObjectType*> Key(State.Owner, State.ScriptType);
		int32& Count = GASStructLiveTypes.FindOrAdd(Key);
		Count += Delta;
		check(Count >= 0);
		if (Count == 0)
		{
			GASStructLiveTypes.Remove(Key);
		}
	}

	void SetASStructRegistryEntry(const void* Address, const FASStructInstanceState& State)
	{
		if (const FASStructInstanceState* Existing = GASStructInstanceRegistry.Find(Address))
		{
			CountASStructLiveType(*Existing, -1);
		}
		GASStructInstanceRegistry.Add(Address, State);
		CountASStructLiveType(State, 1);
	}

	void RemoveASStructRegistryEntry(const void* Address)
	{
		FASStructInstanceState Removed;
		if (GASStructInstanceRegistry.RemoveAndCopyValue(Address, Removed))
		{
			CountASStructLiveType(Removed, -1);
		}
	}

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
			SetASStructRegistryEntry(Element + ArrayIndex * State.Owner->GetSize(), State);
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
					RemoveASStructRegistryEntry(Address);
				}
			}
		}
	}

	void RemoveOtherASStructRegistration(FASStructOps* Ops, const void* Data)
	{
		FScopeLock Lock(&GASStructInstanceRegistryMutex);
		if (const FASStructInstanceState* Found = GASStructInstanceRegistry.Find(Data))
		{
			if (Found->Owner != Ops)
			{
				RemoveASStructRegistryEntry(Data);
			}
		}
	}
}

ANGELSCRIPTCODE_API void ForgetAngelscriptStructScriptType(const void* Data, const asITypeInfo* /*ScriptType*/)
{
	FScopeLock Lock(&GASStructInstanceRegistryMutex);
	// The address is the lifetime key; hot reload can retire it through a replacement type.
	RemoveASStructRegistryEntry(Data);
}

ANGELSCRIPTCODE_API asITypeInfo* GetAngelscriptStructScriptType(const void* Data)
{
	FASStructInstanceState State;
	return FindRegisteredASStructState(Data, State) ? State.ScriptType : nullptr;
}

namespace AngelscriptRetiredModules
{
// Called on the game thread under the execution writer whenever old modules may be discarded.
void AppendStructInstanceModules(TArray<asCModule*>& Modules)
{
	FScopeLock Lock(&GASStructInstanceRegistryMutex);
	// A soft reload gives the same struct a new script type of the same layout. Like the fork's
	// struct ops, its existing values then run the current type, never the discarded one.
	TSet<FASStructOps*> Moved;
	for (const auto& Pair : GASStructLiveTypes)
	{
		if (Pair.Key.Key->ScriptType != nullptr && Pair.Key.Key->ScriptType != Pair.Key.Value)
		{
			Moved.Add(Pair.Key.Key);
		}
	}
	if (!Moved.IsEmpty())
	{
		for (auto& Entry : GASStructInstanceRegistry)
		{
			FASStructInstanceState& State = Entry.Value;
			if (State.ScriptType == nullptr || !Moved.Contains(State.Owner) || State.ScriptType == State.Owner->ScriptType)
			{
				continue;
			}
			CountASStructLiveType(State, -1);
			State = MakeASStructInstanceState(State.Owner);
			CountASStructLiveType(State, 1);
		}
	}
	// A struct retired or removed by a full reload has no current type. Its remaining values
	// still destruct through the type they were constructed with, so that module stays.
	for (const auto& Pair : GASStructLiveTypes)
	{
		if (Pair.Key.Value->module != nullptr)
		{
			Modules.Add(Pair.Key.Value->module);
		}
	}
}
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
