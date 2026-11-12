// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "ApiPolicy/ModKitApiAllowlist.h"

#include "FeatureMods/ModKitCompilationManager.h"
#include "FeatureMods/ModKitModRegistry.h"
#include "FeatureMods/ModKitSignature.h"
#include "AngelscriptManager.h"
#include "AngelscriptCoreExtensionPoints.h"
#include "AngelscriptOptionalProviders.h"
#include "AngelscriptType.h"
#include "as_module.h"
#include "as_context.h"
#include "as_objecttype.h"
#include "as_scriptfunction.h"
#include "as_scriptengine.h"
#include "as_bytecode.h"
#include "Misc/Base64.h"
#include "Misc/SecureHash.h"
#include "Core/ModKitSettings.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/FileManager.h"
#include "GameFramework/Actor.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TArray<TSharedRef<FAngelscriptModuleDesc>>* BlueprintAuditModules = nullptr;

	void CollectModuleApi(const FAngelscriptModuleDesc* CuvissModule, FModKitApiAllowlist::FModApiUsage& CuvissApiUsage)
	{
		if (!CuvissModule || !CuvissModule->ScriptModule) return;
		auto* CuvissScriptModule = (asCModule*)CuvissModule->ScriptModule;
		auto* ScriptEngine = FAngelscriptManager::Get().GetScriptEngine();
		auto CuvissAddUnique = [](TArray<FString>& OutDeclarations, FString Declaration)
		{
			if (!OutDeclarations.Contains(Declaration))
			{
				OutDeclarations.Add(MoveTemp(Declaration));
			}
		};

		// Records a declaration string for system functions only; script,
		// imported, interface, funcdef and delegate functions are skipped.
		auto CuvissRecordSystemFunction = [&CuvissAddUnique, &CuvissApiUsage](asIScriptFunction* Function, asITypeInfo* ImplicitType = nullptr)
		{
			if (Function == nullptr || Function->GetFuncType() != asFUNC_SYSTEM)
			{
				return;
			}
			if (ImplicitType != nullptr)
				for (asUINT Index = 0; Index < ImplicitType->GetBehaviourCount(); ++Index)
				{
					asEBehaviours Behaviour;
					asIScriptFunction* Lifetime = ImplicitType->GetBehaviourByIndex(Index, &Behaviour);
					if ((Behaviour == asBEHAVE_CONSTRUCT || Behaviour == asBEHAVE_DESTRUCT)
						&& Lifetime != nullptr && Lifetime->GetId() == Function->GetId()) return;
				}
			CuvissAddUnique(CuvissApiUsage.SystemCallDeclarations, UTF8_TO_TCHAR(Function->GetDeclaration(true, true)));
		};

		enum class EContext : uint8 { Method, Behaviour, GlobalFunction, GlobalInitializer };
		auto CuvissCollectFunction = [&](asIScriptFunction* Function, EContext Context, asITypeInfo* ImplicitType = nullptr)
		{
			TArray<asIScriptFunction*> Pending;
			TSet<asIScriptFunction*> Visited;
			if (Function) Pending.Add(Function);
			while (!Pending.IsEmpty())
			{
				auto* Current = Pending.Pop();
				if (!Current || Visited.Contains(Current)) continue;
				Visited.Add(Current);
				if (Current->GetFuncType() == asFUNC_SYSTEM)
				{
					CuvissRecordSystemFunction(Current, Context == EContext::GlobalInitializer ? ImplicitType : nullptr);
					continue;
				}
				asUINT Length = 0;
				asDWORD* ByteCode = Current->GetByteCode(&Length);
				if (!ByteCode || Length == 0)
				{
					CuvissApiUsage.CollectionError = TEXT("MODKIT-API-ALLOWLIST: candidate bytecode unavailable");
					continue;
				}
				for (asUINT Offset = 0; Offset < Length;)
				{
					asDWORD* Instruction = ByteCode + Offset;
					const asBYTE Op = static_cast<asBYTE>(*Instruction);
					if (Op >= asBC_MAXBYTECODE)
					{
						CuvissApiUsage.CollectionError = TEXT("MODKIT-API-ALLOWLIST: invalid bytecode opcode");
						break;
					}
					const int Size = asBCTypeSize[asBCInfo[Op].type];
					if (Size <= 0 || static_cast<asUINT>(Size) > Length - Offset)
					{
						CuvissApiUsage.CollectionError = TEXT("MODKIT-API-ALLOWLIST: invalid bytecode boundary");
						break;
					}
					if (Op == asBC_CALLSYS || Op == asBC_Thiscall1 || Op == asBC_FuncPtr)
						Pending.Add(reinterpret_cast<asIScriptFunction*>(asBC_PTRARG(Instruction)));
					else if (Op == asBC_CALL || Op == asBC_ALLOC)
					{
						const int Id = Op == asBC_ALLOC ? asBC_INTARG(Instruction + AS_PTR_SIZE) : asBC_INTARG(Instruction);
						if (Id != 0 || Op != asBC_ALLOC)
						{
							auto* Target = ScriptEngine->GetFunctionById(Id);
							if (Target) Pending.Add(Target);
							else CuvissApiUsage.CollectionError = TEXT("MODKIT-API-ALLOWLIST: unresolved call target");
						}
					}
					else if (Op == asBC_FREE)
					{
						auto* Type = reinterpret_cast<asCObjectType*>(asBC_PTRARG(Instruction));
						if (Type && Type->beh.destruct) Pending.Add(ScriptEngine->GetFunctionById(Type->beh.destruct));
					}
					else if (Op == asBC_CALLBND)
					{
						auto* Engine = static_cast<asCScriptEngine*>(ScriptEngine);
						const asUINT Id = asBC_INTARG(Instruction) & ~FUNC_IMPORTED;
						if (Id >= Engine->importedFunctions.GetLength() || !Engine->importedFunctions[Id]
							|| Engine->importedFunctions[Id]->boundFunctionId < 0)
							CuvissApiUsage.CollectionError = TEXT("MODKIT-API-ALLOWLIST: unbound imported call");
						else Pending.Add(Engine->GetFunctionById(Engine->importedFunctions[Id]->boundFunctionId));
					}
					else if (Op == asBC_CALLINTF || Op == asBC_CallPtr)
					{
						// Dynamic dispatch needs a proven target set; a missing proof is
						// an explicit unsupported signature, never an empty API surface.
						CuvissApiUsage.CollectionError = TEXT("MODKIT-API-ALLOWLIST: dynamic call target set unavailable");
					}
					Offset += Size;
				}
			}
		};

		// Base-class facts come from the manager's class descriptors (the
		// module type enumeration does not surface base types pre-swap-in),
		// while method bytecode comes from the module type enumeration
		// (ClassDesc::ScriptType is not yet attached at this stage). The
		// two sources are complementary; both are mechanical extraction.
		for (const TSharedRef<FAngelscriptClassDesc>& CuvissClass : CuvissModule->Classes)
		{
			if (CuvissClass->bIsStaticsClass || CuvissClass->bIsStruct)
			{
				continue;
			}

			// Inherited system base classes (script bases are left to the
			// direction rules audited by the leaf-direction hook).
			if (CuvissClass->bSuperIsCodeClass && !CuvissClass->SuperClass.IsEmpty())
			{
				CuvissAddUnique(CuvissApiUsage.BaseTypeDeclarations, CuvissClass->SuperClass);
			}

			// UFUNCTION(BlueprintOverride) methods use an Unreal _Implementation
			// thunk rather than AngelScript's override trait. CodeSuperClass points
			// to the native ancestor even when this class inherits through scripts.
			if (CuvissClass->CodeSuperClass != nullptr)
			{
				FString CuvissNativeSuperName;
				asITypeInfo* CuvissNativeSuperType = nullptr;
				bool bCuvissNativeSuperResolved = false;
				for (const TSharedRef<FAngelscriptFunctionDesc>& CuvissMethodDesc : CuvissClass->Methods)
				{
					if (!CuvissMethodDesc->bBlueprintOverride)
					{
						continue;
					}

					TArray<FString> CuvissOverrideNames;
					if (!CuvissMethodDesc->OriginalFunctionName.IsEmpty())
					{
						CuvissOverrideNames.Add(CuvissMethodDesc->OriginalFunctionName);
					}
					if (!CuvissMethodDesc->FunctionName.IsEmpty() && !CuvissOverrideNames.Contains(CuvissMethodDesc->FunctionName))
					{
						CuvissOverrideNames.Add(CuvissMethodDesc->FunctionName);
					}
					if (!CuvissMethodDesc->ScriptFunctionName.IsEmpty() && !CuvissOverrideNames.Contains(CuvissMethodDesc->ScriptFunctionName))
					{
						CuvissOverrideNames.Add(CuvissMethodDesc->ScriptFunctionName);
					}

					// Script-only BlueprintEvents are checked by script inheritance
					// rules. Record only overrides that resolve to native events.
					bool bCuvissNativeEvent = false;
					for (const FString& CuvissOverrideName : CuvissOverrideNames)
					{
						if (const UFunction* Event = AngelscriptCoreExtensionPoints::FindBlueprintEventByScriptName(CuvissClass->CodeSuperClass, CuvissOverrideName);
						Event && Event->HasAnyFunctionFlags(FUNC_BlueprintEvent)
						&& Event->GetOwnerClass()->HasAnyClassFlags(CLASS_Native))
						{
							bCuvissNativeEvent = true;
							break;
						}
					}
					if (!bCuvissNativeEvent)
					{
						continue;
					}
					if (!bCuvissNativeSuperResolved)
					{
						CuvissNativeSuperName = FAngelscriptType::GetBoundClassName(CuvissClass->CodeSuperClass);
						CuvissNativeSuperType = ScriptEngine->GetTypeInfoByDecl(TCHAR_TO_UTF8(*CuvissNativeSuperName));
						bCuvissNativeSuperResolved = true;
					}

					bool bCuvissOverrideRecorded = false;
					if (CuvissNativeSuperType != nullptr)
					{
						for (const FString& CuvissOverrideName : CuvissOverrideNames)
						{
							asIScriptFunction* CuvissNativeParentMethod = CuvissNativeSuperType->GetMethodByName(TCHAR_TO_UTF8(*CuvissOverrideName));
							if (CuvissNativeParentMethod != nullptr && CuvissNativeParentMethod->GetFuncType() == asFUNC_SYSTEM)
							{
								CuvissAddUnique(CuvissApiUsage.OverrideDeclarations, UTF8_TO_TCHAR(CuvissNativeParentMethod->GetDeclaration(true, true)));
								bCuvissOverrideRecorded = true;
								break;
							}
						}
					}

					if (!bCuvissOverrideRecorded)
					{
						CuvissAddUnique(CuvissApiUsage.OverrideDeclarations,
							FString::Printf(TEXT("%s::%s"), *CuvissNativeSuperName,
								CuvissOverrideNames.Num() != 0 ? *CuvissOverrideNames[0] : TEXT("<unresolved>")));
					}
				}
			}
		}

		const asUINT CuvissTypeCount = CuvissScriptModule->GetObjectTypeCount();
		for (asUINT CuvissTypeIndex = 0; CuvissTypeIndex < CuvissTypeCount; ++CuvissTypeIndex)
		{
			asITypeInfo* CuvissType = CuvissScriptModule->GetObjectTypeByIndex(CuvissTypeIndex);
			if (CuvissType == nullptr)
			{
				continue;
			}

			// Methods and behaviours carry the module's executable code.
			const asUINT CuvissMethodCount = CuvissType->GetMethodCount();
			for (asUINT CuvissMethodIndex = 0; CuvissMethodIndex < CuvissMethodCount; ++CuvissMethodIndex)
			{
				asIScriptFunction* CuvissMethod = CuvissType->GetMethodByIndex(CuvissMethodIndex);
				if (CuvissMethod == nullptr)
				{
					continue;
				}
				CuvissCollectFunction(CuvissMethod, EContext::Method);

				// Native virtual overrides: the parent function is the
				// virtualFunctionTable slot at the function's vfTableIdx.
				asCScriptFunction* CuvissMethodImpl = (asCScriptFunction*)CuvissMethod;
				if (CuvissMethodImpl->IsOverride())
				{
					asCObjectType* CuvissObjectType = CuvissMethodImpl->objectType;
					if (CuvissObjectType != nullptr && CuvissObjectType->derivedFrom != nullptr
						&& CuvissMethodImpl->vfTableIdx >= 0
						&& (asUINT)CuvissMethodImpl->vfTableIdx < CuvissObjectType->derivedFrom->virtualFunctionTable.GetLength())
					{
						asCScriptFunction* CuvissParentFunction = CuvissObjectType->derivedFrom->virtualFunctionTable[CuvissMethodImpl->vfTableIdx];
						if (CuvissParentFunction != nullptr && CuvissParentFunction->funcType == asFUNC_SYSTEM)
						{
							CuvissAddUnique(CuvissApiUsage.OverrideDeclarations, UTF8_TO_TCHAR(CuvissParentFunction->GetDeclaration(true, true)));
						}
					}
				}
			}

			const asUINT CuvissBehaviourCount = CuvissType->GetBehaviourCount();
			for (asUINT CuvissBehaviourIndex = 0; CuvissBehaviourIndex < CuvissBehaviourCount; ++CuvissBehaviourIndex)
			{
				asIScriptFunction* CuvissBehaviour = CuvissType->GetBehaviourByIndex(CuvissBehaviourIndex, nullptr);
				if (CuvissBehaviour == nullptr)
				{
					continue;
				}
				CuvissCollectFunction(CuvissBehaviour, EContext::Behaviour);
			}
		}

		const asUINT CuvissGlobalFunctionCount = CuvissScriptModule->GetFunctionCount();
		for (asUINT CuvissGlobalIndex = 0; CuvissGlobalIndex < CuvissGlobalFunctionCount; ++CuvissGlobalIndex)
		{
			CuvissCollectFunction(CuvissScriptModule->GetFunctionByIndex(CuvissGlobalIndex), EContext::GlobalFunction);
		}

		// Initializers are not exported module functions. They may call native
		// APIs directly even when every ordinary function has an empty API surface.
		for (asUINT CuvissGlobalIndex = 0; CuvissGlobalIndex < CuvissScriptModule->scriptGlobalsList.GetLength(); ++CuvissGlobalIndex)
		{
			auto* CuvissProperty = CuvissScriptModule->scriptGlobalsList[CuvissGlobalIndex];
			asITypeInfo* CuvissImplicitType = nullptr;
			// Exempt only construction/destruction of the preprocessor-generated
			// TStructType metadata token. User initializers stay audited.
			for (const auto& CuvissClass : CuvissModule->Classes)
			{
				if (CuvissProperty->nameSpace != nullptr && CuvissProperty->module == CuvissScriptModule
					&& CuvissClass->bIsStruct && !CuvissClass->StaticClassGlobalVariableName.IsEmpty()
					&& CuvissClass->StaticClassGlobalVariableName == UTF8_TO_TCHAR(CuvissProperty->name.AddressOf())
					&& CuvissProperty->type.GetTypeInfo() != nullptr
					&& FString(UTF8_TO_TCHAR(CuvissProperty->type.GetTypeInfo()->GetName())) == TEXT("TStructType")
					&& CuvissClass->Namespace.Get(FString()) == UTF8_TO_TCHAR(CuvissProperty->nameSpace->name.AddressOf()))
				{
					CuvissImplicitType = CuvissProperty->type.GetTypeInfo();
					break;
				}
			}
			CuvissCollectFunction(CuvissProperty->initFunc, EContext::GlobalInitializer, CuvissImplicitType);
		}
	}

	bool AppliesAllowlist(EModKitModTrust Trust, const UModKitSettings& Settings)
	{
		return Trust == EModKitModTrust::Player;
	}

	TSharedPtr<FAngelscriptModuleDesc> AuditedCandidate(const FString& ModuleName)
	{
		if (BlueprintAuditModules)
			for (const auto& Module : *BlueprintAuditModules)
				if (Module->ModuleName == ModuleName) return Module;
		return nullptr;
	}

	bool IsTrustedFeatureMod(const FString& ModuleName, const UModKitSettings& Settings)
	{
		const auto Candidate = AuditedCandidate(ModuleName);
		FModKitSourceOwner Owner;
		FString Error;
		return Candidate.IsValid()
			&& FModKitCompilationManager::GetSourceOwner(Candidate.ToSharedRef(), Owner, Error)
			&& Owner.Layer == EModKitSourceLayer::Leaf
			&& FModKitModRegistry::IsSourceOwnerCurrent(Owner) && !AppliesAllowlist(Owner.Trust, Settings);
	}

	bool TryGetBlueprintSourceIdentity(UClass* Class, FString& OutPath, bool bPropertyOwner = false)
	{
		if (!IsValid(Class) || !Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint)) return false;
		if (!bPropertyOwner)
		{
			const auto Registered = FAngelscriptType::GetByClass(Class);
			if (!Registered.IsValid() || Registered->GetClass(FAngelscriptTypeUsage()) != Class) return false;
		}
		const FString LivePath = Class->GetPathName();
		if (!LivePath.StartsWith(TEXT("/")) || !LivePath.EndsWith(TEXT("_C"))) return false;
		OutPath = LivePath;
		return true;
	}

	bool MatchesBlueprintSourceIdentity(UClass* Class, const FString& ExpectedPath)
	{
		FString ActualPath;
		return TryGetBlueprintSourceIdentity(Class, ActualPath) && ActualPath == ExpectedPath;
	}

	bool SignedBlueprintViews(const FString& ModuleName, TMap<FString, FString>& Views)
	{
		if (!BlueprintAuditModules) return true;
		for (const auto& Module : *BlueprintAuditModules)
		{
			if (Module->ModuleName != ModuleName) continue;
			FModKitSourceOwner Owner;
			FString Error;
			if (!FModKitCompilationManager::GetSourceOwner(Module, Owner, Error)) return false;
			FString Json = Owner.VerifiedConfig;
			TSharedPtr<FJsonObject> Config;
			const TSharedPtr<FJsonObject>* Bindings = nullptr;
			const bool bSignedBindings = !Json.IsEmpty()
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Config) && Config
				&& Config->TryGetObjectField(TEXT("BlueprintBindings"), Bindings);
			bool bFoundCarrier = false;
			for (const auto& Class : Module->Classes)
			{
				if (Class->ClassName != TEXT("U__SoloASInterfaceSchema_") + FMD5::HashAnsiString(*ModuleName)) continue;
				if (bFoundCarrier) return false;
				bFoundCarrier = true;
				const FString* Encoded = Class->Meta.Find(TEXT("SoloASInterfaceSchema"));
				TSharedPtr<FJsonObject> Schema;
				const TArray<TSharedPtr<FJsonValue>>* Imports = nullptr;
				if (!bSignedBindings || !Encoded || !FBase64::Decode(*Encoded, Json)
					|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Schema) || !Schema
					|| !Schema->TryGetArrayField(TEXT("imports"), Imports)) return false;
				for (const auto& Import : *Imports)
				{
					if (!Import.IsValid() || Import->Type != EJson::Object) return false;
					const auto Entry = Import->AsObject();
					FString Name, ClassPath, View, Asset;
					if (!Entry || !Entry->TryGetStringField(TEXT("name"), Name)
						|| !Entry->TryGetStringField(TEXT("path"), ClassPath)
						|| !Entry->TryGetStringField(TEXT("view"), View)
						|| !View.StartsWith(TEXT("__SoloASBlueprint_"))
						|| !(*Bindings)->TryGetStringField(Name, Asset)) return false;
					if (!Asset.Contains(TEXT("."))) Asset += TEXT(".") + FPaths::GetCleanFilename(Asset);
					if (Asset.EndsWith(TEXT("_C"))) Asset.LeftChopInline(2);
					// Carrier metadata is AS-declarable. Only the native type registry
					// populated by Extensions can attest which class a view represents.
					const auto Registered = FAngelscriptType::GetByAngelscriptTypeName(View);
					UClass* RegisteredClass = Registered.IsValid() ? Registered->GetClass(FAngelscriptTypeUsage()) : nullptr;
					if (!IsValid(RegisteredClass) || !RegisteredClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint)
						|| ClassPath != Asset + TEXT("_C")
						|| !MatchesBlueprintSourceIdentity(RegisteredClass, ClassPath)
						|| (Views.Contains(View) && Views[View] != Asset)) return false;
					Views.Add(View, Asset);
				}
			}
		}
		return true;
	}

	bool CollectProperties(const FString& ModuleName, TMap<FString, FString>& Views,
		TMap<FString, FString>& Declarations)
	{
		asCModule* Prepared = nullptr;
		if (BlueprintAuditModules)
			for (const auto& Module : *BlueprintAuditModules)
				if (Module->ModuleName == ModuleName) Prepared = Module->ScriptModule;
		auto& Manager = FAngelscriptManager::Get();
		auto* Engine = Manager.GetScriptEngine();
		if (!Prepared)
		{
			// Editor/direct recompiles have no ModKit scope. The compiler's reciprocal
			// transaction link excludes candidates retained after a rejected reload.
			const auto Active = Manager.GetModule(ModuleName);
			asCModule* ActiveScript = Active ? Active->ScriptModule : nullptr;
			if (ActiveScript)
			{
				Prepared = ActiveScript->ReloadNewModule;
				if (Prepared && (Prepared->ReloadOldModule != ActiveScript
					|| UTF8_TO_TCHAR(Prepared->baseModuleName.AddressOf()) != ModuleName)) return false;
			}
			else
			{
				// Initial append has no published generation to supply a link.
				for (asUINT Index = 0; Index < Engine->GetModuleCount(); ++Index)
				{
					auto* Module = static_cast<asCModule*>(Engine->GetModuleByIndex(Index));
					if (UTF8_TO_TCHAR(Module->baseModuleName.AddressOf()) != ModuleName) continue;
					if (Prepared) return false;
					Prepared = Module;
				}
			}
		}
		if (!Prepared) return !Manager.bIsCompilingModules;

		const auto CollectFunction = [&](asIScriptFunction* Function)
		{
			if (!Function || Function->GetFuncType() != asFUNC_SCRIPT) return true;
			asUINT Length = 0;
			const asDWORD* Code = Function->GetByteCode(&Length);
			for (asUINT Position = 0; Position < Length; )
			{
				const asDWORD* Instruction = Code + Position;
				const auto Op = static_cast<asEBCInstr>(*Instruction & 0xff);
				if (Op >= asBC_MAXBYTECODE) return false;
				const int Size = asBCTypeSize[asBCInfo[Op].type];
				if (Size <= 0 || static_cast<asUINT>(Size) > Length - Position) return false;
				int TypeId = 0, Offset = 0;
				switch (Op)
				{
				case asBC_ADDSi:
				case asBC_LoadThisR:
				case asBC_PshThisMemObj:
				case asBC_PshThisMemRef:
					TypeId = asBC_DWORDARG(Instruction);
					Offset = asBC_SWORDARG0(Instruction);
					break;
				case asBC_LoadRObjR:
				case asBC_LoadVObjR:
				case asBC_PshVMemObj:
				case asBC_PshVMemRef:
				case asBC_PSFVMemRef:
					TypeId = Instruction[2];
					Offset = asBC_SWORDARG1(Instruction);
					break;
				default:
					Position += Size;
					continue;
				}
				asITypeInfo* Type = Engine->GetTypeInfoById(TypeId);
				if (!Type || !(Type->GetFlags() & (asOBJ_REF | asOBJ_VALUE))) return false;
				TArray<asCObjectType*, TInlineAllocator<8>> Ancestors;
				// Match the VM's ownership walk: script bases first, then native shadows.
				// The bytecode type is the receiver, which may be a script subclass.
				for (auto* Current = static_cast<asCObjectType*>(Type); Current;
					Current = Current->derivedFrom ? Current->derivedFrom : static_cast<asCObjectType*>(Current->shadowType))
				{
					if (Ancestors.Contains(Current)) return false;
					Ancestors.Add(Current);
					const FString Owner = UTF8_TO_TCHAR(Current->GetName());
					if (!Owner.StartsWith(TEXT("__SoloASBlueprint_"))) continue;
					const auto Registered = FAngelscriptType::GetByAngelscriptTypeName(Owner);
					UClass* RegisteredClass = Registered.IsValid() ? Registered->GetClass(FAngelscriptTypeUsage()) : nullptr;
					if (!IsValid(RegisteredClass) || !RegisteredClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint)
						|| Registered->GetAngelscriptTypeInfo(FAngelscriptTypeUsage::DefaultUsage) != Current) return false;
					// A parent view can have been imported by another module. Its native
					// registration, not this module's import list, attests the identity.
					FString Asset;
					if (!TryGetBlueprintSourceIdentity(RegisteredClass, Asset)) return false;
					if (!Asset.EndsWith(TEXT("_C"))) return false;
					Asset.LeftChopInline(2);
					if (const FString* SignedAsset = Views.Find(Owner); SignedAsset && *SignedAsset != Asset) return false;
					Views.Add(Owner, MoveTemp(Asset));
				}
				{
					// Native fields remain native API uses even after an implicit upcast
					// erases the Blueprint receiver type without emitting a system call.
					FString Declaration, CanonicalDeclaration;
					asCObjectType* DeclaringType = nullptr;
					for (auto* Current : Ancestors)
					{
						for (asUINT Index = 0; Index < Current->GetPropertyCount(); ++Index)
						{
							// Script tables also contain copies of their script bases' properties.
							if (Current->IsPropertyInherited(Index)) continue;
							const char* Name = nullptr;
							int PropertyOffset = 0, PropertyType = 0, CompositeOffset = 0;
							bool bIndirect = false;
							if (Current->GetProperty(Index, &Name, &PropertyType, nullptr, nullptr, &PropertyOffset,
								nullptr, nullptr, &CompositeOffset, &bIndirect) < 0) return false;
							if (PropertyOffset != Offset) continue;
							if (DeclaringType || !Name || CompositeOffset || bIndirect) return false;
							DeclaringType = Current;
							if (Current->GetFlags() & asOBJ_SCRIPT_OBJECT) continue;
							// A view can bind an unregistered ancestor's fields into its local
							// VM table. Only reflection identifies the actual declaring owner.
							const auto Receiver = FAngelscriptTypeUsage::FromTypeId(Current->GetTypeId());
							UStruct* Reflected = Receiver.GetClass();
							if (!Reflected) Reflected = Receiver.GetUnrealStruct();
							if (!IsValid(Reflected)) return false;
							FProperty* BoundProperty = nullptr;
							for (TFieldIterator<FProperty> It(Reflected); It; ++It)
							{
								if (It->GetOffset_ForUFunction() != PropertyOffset) continue;
								// Do not pick a name or the first field at an aliased offset.
								if (BoundProperty) return false;
								BoundProperty = *It;
							}
							if (!BoundProperty) return false;
							const auto PropertyUsage = FAngelscriptTypeUsage::FromProperty(BoundProperty);
							if (!PropertyUsage.IsValid() || !PropertyUsage.EqualsUnqualified(
								FAngelscriptTypeUsage::FromTypeId(PropertyType))) return false;
							UClass* OwnerClass = BoundProperty->GetOwnerClass();
							UStruct* Owner = OwnerClass ? OwnerClass : BoundProperty->GetOwnerStruct();
							if (!IsValid(Owner)) return false;
							FString StableOwner = Owner->GetPathName();
							FString RawOwner = UTF8_TO_TCHAR(Current->GetName());
							if (OwnerClass && OwnerClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
							{
								if (!TryGetBlueprintSourceIdentity(OwnerClass, StableOwner, true)) return false;
								if (!StableOwner.EndsWith(TEXT("_C"))) return false;
								StableOwner.LeftChopInline(2);
								const auto OwnerType = FAngelscriptType::GetByClass(OwnerClass);
								RawOwner = OwnerType.IsValid() ? OwnerType->GetAngelscriptTypeName()
									: TEXT("__SoloASBlueprint_") + FMD5::HashAnsiString(*OwnerClass->GetPathName());
							}
							else if (OwnerClass) RawOwner = FAngelscriptType::GetBoundClassName(OwnerClass);
							const FString Prefix = FString(UTF8_TO_TCHAR(Engine->GetTypeDeclaration(PropertyType, true))) + TEXT(" ");
							const FString Member = TEXT("::") + BoundProperty->GetName();
							Declaration = Prefix + RawOwner + Member;
							CanonicalDeclaration = Prefix + StableOwner + Member;
						}
					}
					if (!DeclaringType) return false;
					// A mod's own script fields do not acquire native API permissions.
					if (!(DeclaringType->GetFlags() & asOBJ_SCRIPT_OBJECT))
					{
						if (const auto* Existing = Declarations.Find(Declaration); Existing && *Existing != CanonicalDeclaration) return false;
						Declarations.Add(MoveTemp(Declaration), MoveTemp(CanonicalDeclaration));
					}
				}
				Position += Size;
			}
			return true;
		};
		// Includes methods, constructors, factories and lambdas, not just exported globals.
		for (asUINT Index = 0; Index < Prepared->scriptFunctions.GetLength(); ++Index)
			if (!CollectFunction(Prepared->scriptFunctions[Index])) return false;
		// Include initializer properties in the same audit before globals execute.
		for (asUINT Index = 0; Index < Prepared->scriptGlobalsList.GetLength(); ++Index)
			if (!CollectFunction(Prepared->scriptGlobalsList[Index]->initFunc)) return false;
		return true;
	}

	enum class EApiMetadataState : uint8 { Unknown, Classified, Conflict };
	struct FApiGroup
	{
		FString Tier;
		FString Module;
		TArray<FString> Tags;
	};

	EApiMetadataState QueryApiGroup(const FString& Declaration, FApiGroup& Group)
	{
		using namespace AngelscriptCoreExtensionPoints;
		Group = {};
		FApiMetadata Metadata;
		const auto Result = QueryApiMetadata(FAngelscriptManager::Get().GetScriptEngine(), Declaration, Metadata);
		if (Result == EApiMetadataResult::Conflict) return EApiMetadataState::Conflict;
		if (Result != EApiMetadataResult::Classified) return EApiMetadataState::Unknown;
		Group.Tier = MoveTemp(Metadata.Tier);
		Group.Module = MoveTemp(Metadata.Module);
		Group.Tags = MoveTemp(Metadata.Tags);
		return EApiMetadataState::Classified;
	}

	bool ContainsIgnoreCase(const TArray<FString>& Values, const FString& Value)
	{
		return Values.ContainsByPredicate([&Value](const FString& Candidate)
			{ return Candidate.Equals(Value, ESearchCase::IgnoreCase); });
	}
}

#if WITH_DEV_AUTOMATION_TESTS
#include "UObject/StrongObjectPtr.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformFileManager.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "AngelscriptSettings.h"
#include "Preprocessor/AngelscriptPreprocessor.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitApiTierAllowlistTest,
    "AngelscriptModKit.Allowlist.TiersAndDeny", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitApiTierAllowlistTest::RunTest(const FString&)
{
    using namespace AngelscriptCoreExtensionPoints;
    using EFace = FModKitApiAllowlist::EApiFace;
    auto* Settings = NewObject<UModKitSettings>();
    const TArray<FString> DefaultDeniedPatterns = Settings->DeniedApiPatterns;
    const FString Key = TEXT("void __ModKitTier::Call()");
    FApiMetadata Metadata {TEXT("core"), TEXT("Example"), {}};
    auto Handle = RegisterApiMetadataProvider(FAngelscriptManager::Get().GetScriptEngine(), [&](FStringView Declaration, FApiMetadata& Out) {
        if (Declaration != FStringView(Key)) return EApiMetadataResult::Unknown;
        Out = Metadata; return EApiMetadataResult::Classified;
    });
    ON_SCOPE_EXIT { if (Handle.IsValid()) UnregisterApiMetadataProvider(Handle); };
    if (!TestTrue(TEXT("Provider registered"), Handle.IsValid())) return false;
    TestTrue(TEXT("Core is allowed"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
    TestTrue(TEXT("Unknown declaration is allowed"), FModKitApiAllowlist::IsAllowed(Key + TEXT(" const"), EFace::SystemCall, *Settings));
    const FString WildKey = TEXT("FString::FString(int)");
    auto Wild = RegisterApiMetadataProvider(FAngelscriptManager::Get().GetScriptEngine(), [WildKey](FStringView Declaration, FApiMetadata& Out) {
        if (Declaration != FStringView(WildKey)) return EApiMetadataResult::Unknown;
        Out = {TEXT("extended"), TEXT("Core"), {}}; return EApiMetadataResult::Classified;
    });
    TestTrue(TEXT("Wildcard test provider registered"), Wild.IsValid());
    TestTrue(TEXT("Extended declaration is allowed"), FModKitApiAllowlist::IsAllowed(WildKey, EFace::SystemCall, *Settings));
    if (Wild.IsValid()) UnregisterApiMetadataProvider(Wild);
    Settings->DeniedApiPatterns = {Key};
    TestFalse(TEXT("Deny overrides core"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
    Settings->DeniedApiPatterns.Reset(); Metadata.Tier = TEXT("internal"); Settings->AllowedApiPatterns = {Key};
    TestFalse(TEXT("Internal overrides explicit allow"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
    Settings->DeniedApiPatterns = DefaultDeniedPatterns;
    TestFalse(TEXT("Default console deny"), FModKitApiAllowlist::IsAllowed(TEXT("void System::ExecuteConsoleCommand(FString)"), EFace::SystemCall, *Settings));
    TestTrue(TEXT("Native parent is open"), FModKitApiAllowlist::IsAllowed(TEXT("AActor"), EFace::BaseType, *Settings));
    TestTrue(TEXT("Native override is open"), FModKitApiAllowlist::IsAllowed(TEXT("void AActor::BeginPlay()"), EFace::Override, *Settings));
    Settings->DeniedBaseClassPatterns = {TEXT("AActor")};
    Settings->DeniedOverridePatterns = {TEXT("*AActor::BeginPlay*")};
    TestFalse(TEXT("Project base deny"), FModKitApiAllowlist::IsAllowed(TEXT("AActor"), EFace::BaseType, *Settings));
    TestFalse(TEXT("Project override deny"), FModKitApiAllowlist::IsAllowed(TEXT("void AActor::BeginPlay()"), EFace::Override, *Settings));
    Settings->bApplyAllowlistToOfficialMods = true;
    TestTrue(TEXT("Player policy applies"), AppliesAllowlist(EModKitModTrust::Player, *Settings));
    TestFalse(TEXT("Official policy is exempt"), AppliesAllowlist(EModKitModTrust::Official, *Settings));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitApiPolicyRefreshTest,
    "AngelscriptModKit.Allowlist.PolicyContract.Registry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModKitApiPolicyRefreshTest::RunTest(const FString&)
{
    using namespace AngelscriptCoreExtensionPoints;
    auto* Settings = const_cast<UModKitSettings*>(UModKitSettings::Get());
    TGuardValue<TArray<FString>> Tiers(Settings->AllowedApiTiers, {TEXT("core")});
    TGuardValue<TArray<FString>> Calls(Settings->AllowedApiPatterns, {});
    TGuardValue<TArray<FString>> Denied(Settings->DeniedApiPatterns, {});
    const FString Key = TEXT("void __ModKitPolicyRefresh::Call()");
    FApiMetadata Metadata {TEXT("core"), TEXT("Test"), {}};
    auto Handle = RegisterApiMetadataProvider(FAngelscriptManager::Get().GetScriptEngine(), [&](FStringView Declaration, FApiMetadata& Out) {
        if (Declaration != FStringView(Key)) return EApiMetadataResult::Unknown;
        Out = Metadata; return EApiMetadataResult::Classified;
    });
    ON_SCOPE_EXIT { if (Handle.IsValid()) UnregisterApiMetadataProvider(Handle); };
    if (!TestTrue(TEXT("Provider registered"), Handle.IsValid())) return false;
    FModKitApiAllowlist::FModApiUsage Usage; Usage.SystemCallDeclarations = {Key};
    FString Error; const FString ModuleName = TEXT("FeatureMods.__ModKitPolicyRefresh.Main");
    TestFalse(TEXT("Core passes audit"), FModKitApiAllowlist::CheckApiAllowlist(ModuleName, Usage, Error));
    Metadata.Tier = TEXT("internal");
    TestTrue(TEXT("Updated classification revokes audit"), FModKitApiAllowlist::CheckApiAllowlist(ModuleName, Usage, Error));
    Metadata.Tier = TEXT("core");
    TestFalse(TEXT("Restored classification passes next audit"), FModKitApiAllowlist::CheckApiAllowlist(ModuleName, Usage, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitApiReloadPolicyTest,
	"AngelscriptModKit.Allowlist.ReloadPolicy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModKitApiReloadPolicyTest::RunTest(const FString& Parameters)
{
	auto* Settings = const_cast<UModKitSettings*>(UModKitSettings::Get());
	const TArray<FString> DefaultDeniedPatterns = Settings->DeniedApiPatterns;
	TGuardValue<TArray<FString>> Tiers(Settings->AllowedApiTiers, {TEXT("core")});
	TGuardValue<TArray<FString>> Tags(Settings->AllowedApiTags, {});
	TGuardValue<TArray<FString>> Modules(Settings->AllowedApiModules, {});
	TGuardValue<bool> OfficialOnly(Settings->bOnlyOfficialExternalMods, false);
	TGuardValue<bool> Confirm(Settings->bConfirmPlayerModsOnFirstLoad, false);
	TGuardValue<TArray<FString>> Calls(Settings->AllowedApiPatterns, {});
	TGuardValue<TArray<FString>> Overrides(Settings->AllowedOverridePatterns, {});
	TGuardValue<TArray<FString>> Denied(Settings->DeniedApiPatterns, {});
	const FString Name = TEXT("ApiPolicyReload") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString TestClassSuffix = Name.Right(32);
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("FeatureMods") / Name);
	const FString ApprovalPath = FPaths::ProjectSavedDir() / TEXT("FeatureModApprovals.ini");
	FString OriginalApprovals;
	const bool bHadApprovalFile = FFileHelper::LoadFileToString(OriginalApprovals, *ApprovalPath);
	const FString Source = Root / TEXT("Script/Policy.as");
	const FString RelativeSource = TEXT("FeatureMods") / Name / TEXT("Script/Policy.as");
	const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("AngelscriptModKit"));
	if (!TestTrue(TEXT("ModKit template is installed"), Plugin.IsValid())) return false;
	const FString Template = Plugin->GetBaseDir() / TEXT("Templates/ModName");
	FString Descriptor, Config;
	if (!TestTrue(TEXT("Read existing FeatureMod descriptor template"), FFileHelper::LoadFileToString(Descriptor, *(Template / TEXT("ModName.uplugin"))))
		|| !TestTrue(TEXT("Read existing FeatureMod config template"), FFileHelper::LoadFileToString(Config, *(Template / TEXT("FeatureMod.json"))))) return false;
	IFileManager::Get().MakeDirectory(*(Root / TEXT("Script")), true);
	ON_SCOPE_EXIT
	{
		if (bHadApprovalFile) FFileHelper::SaveStringToFile(OriginalApprovals, *ApprovalPath);
		else IFileManager::Get().Delete(*ApprovalPath);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	};
	if (!TestTrue(TEXT("Write player descriptor"), FFileHelper::SaveStringToFile(Descriptor.Replace(TEXT("ModName"), *Name), *(Root / (Name + TEXT(".uplugin")))))
		|| !TestTrue(TEXT("Write player config"), FFileHelper::SaveStringToFile(Config, *(Root / TEXT("FeatureMod.json"))))) return false;
	if (!TestTrue(TEXT("Discover a real unsigned player FeatureMod"), FModKitModRegistry::RescanOrAdd(Name))) return false;
	TestTrue(TEXT("Unsigned source has player trust"), FModKitModRegistry::GetModTrust(Name) == EModKitModTrust::Player);
	if (!TestTrue(TEXT("Approve player source for API policy audit"), FModKitModRegistry::ApprovePlayerMod(Name))) return false;
	auto& Manager = FAngelscriptManager::Get();
	const FString ModuleName = TEXT("FeatureMods.") + Name + TEXT(".Script.Policy");
	const FString Key = TEXT("void Error(const FString&inout)");
	using namespace AngelscriptCoreExtensionPoints;
	FApiMetadata Metadata {TEXT("core"), TEXT("Core"), {}};
	auto Producer = RegisterApiMetadataProvider(Manager.GetScriptEngine(), [&](FStringView Declaration, FApiMetadata& Out) {
		if (Declaration != FStringView(Key)) return EApiMetadataResult::Unknown;
		Out = Metadata; return EApiMetadataResult::Classified;
	});
	if (!TestTrue(TEXT("Register reload metadata"), Producer.IsValid())) return false;
	ON_SCOPE_EXIT { UnregisterApiMetadataProvider(Producer); };
	const auto Compile = [&](int32 Value, bool bDirectConstructor = false, const FString& OverrideSource = FString())
	{
		Manager.ResetDiagnostics();
		const FString Code = !OverrideSource.IsEmpty() ? OverrideSource : bDirectConstructor
			? FString::Printf(TEXT("const FVector PolicyVector = FVector(1.0f, 2.0f, 3.0f); int ReadPolicy() { return %d; }"), Value)
			: FString::Printf(TEXT("int InitializePolicy() { Error(\"mka-allowed-global-initializer\"); return %d; } ")
				TEXT("const int PolicyValue = InitializePolicy(); int ReadPolicy() { return PolicyValue; }"), Value);
		if (!FFileHelper::SaveStringToFile(Code, *Source)) return ECompileResult::Error;
		FAngelscriptPreprocessor Preprocessor;
		Preprocessor.AddFile(RelativeSource, Source);
		if (!Preprocessor.Preprocess()) return ECompileResult::Error;
		const auto Candidates = Preprocessor.GetModulesToCompile();
		FModKitApiAllowlist::FBlueprintAuditScope Scope(Candidates);
		TArray<TSharedRef<FAngelscriptModuleDesc>> Compiled;
		const auto Result = Manager.CompileModules(ECompileType::FullReload, Candidates, Compiled);
		TestTrue(TEXT("Audit scope preserves the actual candidate"), AuditedCandidate(Candidates[0]->ModuleName) == Candidates[0]);
		return Result;
	};
	const auto Read = [&]()
	{
		const auto Active = Manager.GetModule(ModuleName);
		if (!Active || !Active->ScriptModule) { AddError(TEXT("Expected active player module")); return -1; }
		asIScriptFunction* Function = nullptr;
		for (asUINT Index = 0; Index < Active->ScriptModule->GetFunctionCount(); ++Index)
		{
			auto* Candidate = Active->ScriptModule->GetFunctionByIndex(Index);
			if (FCStringAnsi::Strcmp(Candidate->GetName(), "ReadPolicy") == 0) { Function = Candidate; break; }
		}
		if (!Function) { AddError(TEXT("Expected player entry point")); return -1; }
		FAngelscriptContext Context;
		Context->Prepare(Function);
		TestEqual(TEXT("Player entry executes"), Context->Execute(), int(asEXECUTION_FINISHED));
		const int32 Value = int32(Context->GetReturnDWord());
		return Value;
	};
	// Only the two approved generations may execute the native Error call.
	// A third marker from the rejected generation fails the exact occurrence count.
	AddExpectedError(TEXT("mka-allowed-global-initializer"), EAutomationExpectedErrorFlags::Contains, 2);
	if (!TestEqual(TEXT("Allowed player source initializes and compiles"), Compile(7), ECompileResult::FullyHandled)) return false;
	TestEqual(TEXT("Allowed generation executes"), Read(), 7);
	const auto Previous = Manager.GetModule(ModuleName);
	Metadata.Tier = TEXT("internal");
	AddExpectedError(TEXT("MODKIT-API-ALLOWLIST"), EAutomationExpectedErrorFlags::Contains, 0);
	struct FDefaultDenyCase { const TCHAR* Description; const TCHAR* Source; const TCHAR* Declaration; };
	const FDefaultDenyCase DefaultDenyCases[] = {
		{TEXT("Default QuitGame deny rejects player mod"), TEXT("void AttemptQuit() { System::QuitGame(nullptr, EQuitPreference::Quit, false); } int ReadPolicy() { return 44; }"), TEXT("System::QuitGame")},
		{TEXT("Default save deletion deny rejects player mod"), TEXT("void AttemptDelete() { Gameplay::DeleteGameInSlot(\"Slot\", 0); } int ReadPolicy() { return 44; }"), TEXT("Gameplay::DeleteGameInSlot")},
		{TEXT("Default editor API deny rejects player mod"), TEXT("#if EDITOR\nvoid ReadEditorVersion() { Editor::GetCurrentEngineVersion(); }\n#endif\nint ReadPolicy() { return 44; }"), TEXT("Editor::GetCurrentEngineVersion")},
	};
	AddExpectedError(TEXT("Hot reload failed due to script compile errors"), EAutomationExpectedErrorFlags::Contains, 10 + UE_ARRAY_COUNT(DefaultDenyCases));
	TestEqual(TEXT("Reload re-audits refreshed provider metadata"), Compile(8), ECompileResult::Error);
	TestTrue(TEXT("Rejected reload retains the previous module"), Manager.GetModule(ModuleName) == Previous);
	TestEqual(TEXT("Rejected candidate never replaces the callable generation"), Read(), 7);
	Metadata.Tier = TEXT("core");
	TestEqual(TEXT("Restored permission permits a fresh reload"), Compile(9), ECompileResult::FullyHandled);
	TestEqual(TEXT("Accepted replacement executes"), Read(), 9);
	Settings->DeniedApiPatterns = {TEXT("*FVector::FVector*")};
	TestEqual(TEXT("Direct initializer native constructor is audited"), Compile(11, true), ECompileResult::Error);
	const auto HasViolation = [&](const FString& Declaration)
	{
		for (const auto& File : Manager.Diagnostics)
			for (const auto& Diagnostic : File.Value.Diagnostics)
				if (Diagnostic.Message.Contains(TEXT("MODKIT-API-ALLOWLIST"))
					&& Diagnostic.Message.Contains(Declaration)) return true;
		return false;
	};
	TestTrue(TEXT("Constructor rejection came from initializer API extraction"), HasViolation(TEXT("FVector::FVector")));
	TestEqual(TEXT("Direct initializer rejection preserves callable generation"), Read(), 9);
	Settings->DeniedApiPatterns = {TEXT("APawn")};
	TestEqual(TEXT("Player native base is audited before globals"), Compile(0, false,
		FString::Printf(TEXT("class APolicyDeniedPawn_%s : APawn {} int ReadPolicy() { return 13; }"), *TestClassSuffix)),
		ECompileResult::Error);
	TestTrue(TEXT("Native base declaration is extracted"), HasViolation(TEXT("'APawn'")));
	Settings->DeniedApiPatterns = {TEXT("*AActor::BeginPlay*")};
	TestEqual(TEXT("Player native override is audited before globals"), Compile(0, false,
		FString::Printf(TEXT("class APolicyDeniedActor_%s : AActor { UFUNCTION(BlueprintOverride) void BeginPlay() {} } ")
			TEXT("int ReadPolicy() { return 14; }"), *TestClassSuffix)), ECompileResult::Error);
	TestTrue(TEXT("Native override declaration is extracted"), HasViolation(TEXT("AActor::BeginPlay")));
	TestEqual(TEXT("Rejected native parent and override keep previous generation"), Read(), 9);
	TestNotNull(TEXT("Non-event native function exists"), AActor::StaticClass()->FindFunctionByName(TEXT("K2_GetActorLocation")));
	TestNull(TEXT("Non-BlueprintEvent function is not resolved as an override"),
		AngelscriptCoreExtensionPoints::FindBlueprintEventByScriptName(AActor::StaticClass(), TEXT("GetActorLocation")));
	Settings->DeniedApiPatterns = DefaultDeniedPatterns;
	TestEqual(TEXT("Default console command deny rejects player mod"), Compile(0, false,
		TEXT("int ReadPolicy() { System::ExecuteConsoleCommand(\"quit\"); return 0; }")), ECompileResult::Error);
	TestTrue(TEXT("Console command is reported"), HasViolation(TEXT("System::ExecuteConsoleCommand")));
	Settings->DeniedApiPatterns = {TEXT("*FPaths::FileExists*")};
	TestEqual(TEXT("Custom filesystem deny applies"), Compile(0, false,
		TEXT("int ReadPolicy() { return FPaths::FileExists(\"\") ? 1 : 0; }")), ECompileResult::Error);
	TestTrue(TEXT("Filesystem declaration is rejected by fallback policy"), HasViolation(TEXT("FPaths::FileExists")));
	TestEqual(TEXT("Rejected filesystem access retains previous generation"), Read(), 9);
	TestEqual(TEXT("Own script struct and enum metadata is not an external API call"), Compile(0, false,
		TEXT("struct FPolicyOwnedStruct { int Value = 37; } enum EPolicyOwnedEnum { OwnedValue = 37 } ")
		TEXT("int ReadPolicy() { FPolicyOwnedStruct Value; return Value.Value; }")), ECompileResult::FullyHandled);
	TestEqual(TEXT("Own script type executes with the closed API policy"), Read(), 37);
	Settings->DeniedApiPatterns = {TEXT("*FVector::FVector*")};
	TestEqual(TEXT("Explicit native construction remains audited beside an own script type"), Compile(0, false,
		TEXT("struct FPolicyOwnedStruct { int Value = 37; } const FVector ExplicitNative = FVector(1.0f, 2.0f, 3.0f); ")
		TEXT("int ReadPolicy() { return 38; }")), ECompileResult::Error);
	TestTrue(TEXT("Explicit native constructor remains rejected"), HasViolation(TEXT("FVector::FVector")));
	TestEqual(TEXT("Rejected native constructor preserves the own-type generation"), Read(), 37);
	TestEqual(TEXT("Explicit native construction in an ordinary function is audited"), Compile(0, false,
		TEXT("int ReadPolicy() { FVector Value(1.0f, 2.0f, 3.0f); return 40; }")), ECompileResult::Error);
	TestTrue(TEXT("Function body records the native constructor"), HasViolation(TEXT("FVector::FVector")));
	TestEqual(TEXT("Implicit native member construction is audited"), Compile(0, false,
		TEXT("struct FPolicyNativeMember { FVector Value; } int ReadPolicy() { FPolicyNativeMember Value; return 41; }")), ECompileResult::Error);
	TestTrue(TEXT("Struct behaviour records implicit native member constructor"), HasViolation(TEXT("FVector::FVector")));
	TestEqual(TEXT("A same-named StaticType global in another namespace is audited"), Compile(0, false,
		TEXT("struct FPolicyOwnedStruct { int Value; } namespace Other { const FVector __StaticType_FPolicyOwnedStruct = FVector(1.0f, 2.0f, 3.0f); } int ReadPolicy() { return 42; }")), ECompileResult::Error);
	TestTrue(TEXT("User StaticType name does not grant a lifetime exemption"), HasViolation(TEXT("FVector::FVector")));
	Settings->DeniedApiPatterns.Reset();
	Settings->AllowedApiPatterns = {
		TEXT("UClass TSubclassOf::opImplConv() const"),
		TEXT("void UObject::opCast(?&inout) const"),
		TEXT("AActor SpawnActor(const TSubclassOf<AActor>&inout, const FVector&inout = FVector :: ZeroVector, const FRotator&inout = FRotator :: ZeroRotator, const FName&inout = NAME_None, bool = false, ULevel = nullptr)"),
		TEXT("TSubclassOf::TSubclassOf(UClass)"),
		TEXT("UClass TSubclassOf::Get() const"),
		TEXT("TSubclassOf::TSubclassOf()")
	};
	Settings->AllowedOverridePatterns = {TEXT("void AActor::BeginPlay()")};
	TestEqual(TEXT("Allowed native event override compiles"), Compile(0, false,
		FString::Printf(TEXT("class APolicyAllowedActor_%s : AActor { UFUNCTION(BlueprintOverride) void BeginPlay() {} } ")
			TEXT("int ReadPolicy() { return 43; }"), *TestClassSuffix)), ECompileResult::FullyHandled);
	TestEqual(TEXT("Allowed native event generation executes"), Read(), 43);
	Settings->DeniedApiPatterns = DefaultDeniedPatterns;
	TestTrue(TEXT("Plugin defaults deny console commands"), DefaultDeniedPatterns.Contains(TEXT("*System::ExecuteConsoleCommand*")));
	TestTrue(TEXT("Plugin defaults deny file access"), DefaultDeniedPatterns.Contains(TEXT("*FFileHelper::LoadFileToString*"))
		&& DefaultDeniedPatterns.Contains(TEXT("*FFileHelper::SaveStringToFile*")));
	TestTrue(TEXT("Plugin defaults deny process and URL launch"), DefaultDeniedPatterns.Contains(TEXT("*FPlatformProcess::LaunchURL*"))
		&& DefaultDeniedPatterns.Contains(TEXT("*System::LaunchURL*")));
	TestTrue(TEXT("Plugin defaults deny object lookup and editor property mutation"), DefaultDeniedPatterns.Contains(TEXT("*FindObject(*"))
		&& DefaultDeniedPatterns.Contains(TEXT("*UObject::ResetEditorProperty*")));
	TestTrue(TEXT("Plugin defaults deny QuitGame"), DefaultDeniedPatterns.Contains(TEXT("*System::QuitGame*")));
	TestTrue(TEXT("Plugin defaults deny save deletion"), DefaultDeniedPatterns.Contains(TEXT("*Gameplay::DeleteGameInSlot*")));
	TestTrue(TEXT("Plugin defaults deny editor APIs"), DefaultDeniedPatterns.Contains(TEXT("* Editor::*"))
		&& DefaultDeniedPatterns.Contains(TEXT("* UEditorAssetSubsystem::*")));
	TestTrue(TEXT("Default policy leaves SaveGameToSlot available"), FModKitApiAllowlist::IsAllowed(
		TEXT("bool Gameplay::SaveGameToSlot(USaveGame, FString, int)"), FModKitApiAllowlist::EApiFace::SystemCall, *Settings));
	TestTrue(TEXT("Default policy leaves LoadGameFromSlot available"), FModKitApiAllowlist::IsAllowed(
		TEXT("USaveGame Gameplay::LoadGameFromSlot(FString, int)"), FModKitApiAllowlist::EApiFace::SystemCall, *Settings));
	TestEqual(TEXT("Player mod with class registration and BeginPlay override loads with default policy"), Compile(0, false,
		FString::Printf(TEXT("class APolicyOnlyActor_%s : AActor { UFUNCTION(BlueprintOverride) void BeginPlay() {} } ")
			TEXT("int ReadPolicy() { return 43; }"), *TestClassSuffix)), ECompileResult::FullyHandled);
	TestEqual(TEXT("Class registration sample executes after loading"), Read(), 43);
	for (const FDefaultDenyCase& Case : DefaultDenyCases)
	{
		TestEqual(Case.Description, Compile(0, false, Case.Source), ECompileResult::Error);
		TestTrue(Case.Declaration, HasViolation(Case.Declaration));
	}
	TestEqual(TEXT("Default denials preserve the active player generation"), Read(), 43);
	Settings->DeniedApiPatterns.Remove(TEXT("*System::QuitGame*"));
	TestEqual(TEXT("Project removal of the default deny permits QuitGame call compilation"), Compile(0, false,
		TEXT("void AttemptQuit() { System::QuitGame(nullptr, EQuitPreference::Quit, false); } int ReadPolicy() { return 44; }")), ECompileResult::FullyHandled);
	TestEqual(TEXT("Project-allowed generation loads"), Read(), 44);
	// The production H4 provider and its off path are exercised by
	// FeatureMods.ModuleAdmission; no secondary legacy audit registry is installed.

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitApiMetadataProviderTest,
    "AngelscriptModKit.Allowlist.ApiMetadata", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitApiMetadataProviderTest::RunTest(const FString&)
{
    using namespace AngelscriptCoreExtensionPoints;
    auto* Engine = FAngelscriptManager::Get().GetScriptEngine();
    UModKitSettings* Settings = NewObject<UModKitSettings>();
    using EFace = FModKitApiAllowlist::EApiFace;
    const FString Key = TEXT("void __ModKitMetadata::Call()");
    FApiMetadata Metadata {TEXT("core"), TEXT("ModKitTest"), {TEXT("Test")}};
    FDelegateHandle A, B;
    ON_SCOPE_EXIT
    {
        if (B.IsValid()) UnregisterApiMetadataProvider(B);
        if (A.IsValid()) UnregisterApiMetadataProvider(A);
    };
    const auto Query = [&](FStringView Declaration, FApiMetadata& Out)
    {
        if (Declaration != FStringView(Key)) return EApiMetadataResult::Unknown;
        Out = Metadata;
        return EApiMetadataResult::Classified;
    };
    TestTrue(TEXT("Missing metadata is allowed"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
    A = RegisterApiMetadataProvider(Engine, Query);
    if (!TestTrue(TEXT("Register the native provider"), A.IsValid())) return false;
    TestTrue(TEXT("Live core classification permits the exact declaration"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
    Settings->DeniedApiPatterns = {Key};
    TestFalse(TEXT("Explicit deny precedes metadata"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
    Settings->DeniedApiPatterns.Reset();
    Settings->AllowedApiTiers.Reset();
    TestTrue(TEXT("Tier does not restrict API"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
    Settings->AllowedApiTags = {TEXT("Test")};
    TestTrue(TEXT("Provider tags are consumed"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
    Settings->AllowedApiTags.Reset();
    Settings->AllowedApiModules = {TEXT("ModKitTest")};
    TestTrue(TEXT("Module does not restrict API"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
    B = RegisterApiMetadataProvider(Engine, [&](FStringView Declaration, FApiMetadata& Out)
    {
        if (Declaration != FStringView(Key)) return EApiMetadataResult::Unknown;
        Out = Metadata;
        Out.Tier = TEXT("internal");
        return EApiMetadataResult::Classified;
    });
    if (!TestTrue(TEXT("Register a conflicting provider"), B.IsValid())) return false;
    Settings->AllowedApiPatterns = {Key};
    TestFalse(TEXT("Conflict overrides even exact permission"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
    const auto RemovedB = UnregisterApiMetadataProvider(B);
    TestTrue(TEXT("Remove conflicting provider"), RemovedB == ERemoveResult::Removed);
    if (RemovedB == ERemoveResult::Removed) B.Reset();
    TestTrue(TEXT("Remaining classification is restored"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
    const auto RemovedA = UnregisterApiMetadataProvider(A);
    TestTrue(TEXT("Remove original provider"), RemovedA == ERemoveResult::Removed);
    if (RemovedA == ERemoveResult::Removed) A.Reset();
    Settings->AllowedApiPatterns = {TEXT("*")};
    TestTrue(TEXT("Provider removal restores default openness"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitApiUnknownDenyTest,
	"AngelscriptModKit.Allowlist.UnknownDenyFirst", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitApiUnknownDenyTest::RunTest(const FString&)
{
	UModKitSettings* Settings = NewObject<UModKitSettings>();
	const FString Key = TEXT("void __ModKitUnknown::Call()");
	using EFace = FModKitApiAllowlist::EApiFace;
	TestTrue(TEXT("Unknown classification is allowed"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
	Settings->AllowedApiPatterns = {TEXT("*")};
	TestTrue(TEXT("Deprecated wildcard has no effect"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
	Settings->AllowedApiPatterns = {Key};
	TestTrue(TEXT("Exact declaration can be permitted"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
	Settings->DeniedApiPatterns = {Key};
	TestFalse(TEXT("Explicit deny overrides exact permission"), FModKitApiAllowlist::IsAllowed(Key, EFace::SystemCall, *Settings));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitBlueprintPropertyOwnerIdentityTest,
	"AngelscriptModKit.Allowlist.BlueprintPropertyOwnerIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModKitBlueprintPropertyOwnerIdentityTest::RunTest(const FString&)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* OriginalPackage = CreatePackage(*(TEXT("/Game/ModKitIdentity_") + Suffix));
	auto* Class = NewObject<UBlueprintGeneratedClass>(OriginalPackage, TEXT("Owner_C"), RF_Public);
	Class->ClassFlags |= CLASS_CompiledFromBlueprint;
	const FName OriginalName = Class->GetFName();
	const FString OriginalPath = Class->GetPathName();
	auto* Settings = const_cast<UModKitSettings*>(UModKitSettings::Get());
	TGuardValue<TArray<FString>> Allowed(Settings->AllowedApiPatterns, {TEXT("int ") + OriginalPath.LeftChop(2) + TEXT("::Value")});
	TGuardValue<TArray<FString>> Denied(Settings->DeniedApiPatterns, {});
	auto AuditOwner = [&]()
	{
		FString Path, Error;
		if (!TryGetBlueprintSourceIdentity(Class, Path, true) || !Path.EndsWith(TEXT("_C"))) return true;
		FModKitApiAllowlist::FModApiUsage Usage;
		Usage.SystemCallDeclarations = {TEXT("int ") + Path.LeftChop(2) + TEXT("::Value")};
		return FModKitApiAllowlist::CheckApiAllowlist(TEXT("ModKitPropertyOwnerIdentity"), Usage, Error);
	};
	TestFalse(TEXT("Original property owner permission is allowed"), AuditOwner());
	for (const TCHAR* Label : {TEXT("Staged"), TEXT("Retired")})
	{
		UPackage* TemporaryPackage = CreatePackage(*(TEXT("/Temp/ModKitIdentity_") + Suffix + Label));
		if (!TestTrue(TEXT("Move the property owner"), Class->Rename(*OriginalName.ToString(), TemporaryPackage,
			REN_DontCreateRedirectors | REN_NonTransactional))) return false;
		const FString MovedPath = Class->GetPathName();
		Settings->DeniedApiPatterns = {TEXT("int ") + MovedPath.LeftChop(2) + TEXT("::Value")};
		TestTrue(TEXT("Project deny matches moved property owner"), AuditOwner());
		if (!TestTrue(TEXT("Restore the property owner"), Class->Rename(*OriginalName.ToString(), OriginalPackage,
			REN_DontCreateRedirectors | REN_NonTransactional))) return false;
		TestFalse(TEXT("Deny for moved owner does not match restored owner"), AuditOwner());
		Settings->DeniedApiPatterns.Reset();
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitBlueprintIdentityUnknownTest,
	"AngelscriptModKit.Allowlist.UnknownBlueprintIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitBlueprintIdentityUnknownTest::RunTest(const FString&)
{
	FString Path;
	TestFalse(TEXT("An unknown Blueprint source cannot be attested"), TryGetBlueprintSourceIdentity(UObject::StaticClass(), Path));
	const FString PackageName = TEXT("/Temp/ModKitIdentity_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* Package = CreatePackage(*PackageName);
	auto* Fake = NewObject<UBlueprintGeneratedClass>(Package, TEXT("Claimed_C"), RF_Public);
	Fake->ClassFlags |= CLASS_CompiledFromBlueprint;
	const FString ClaimedPath = Fake->GetPathName();
	TestFalse(TEXT("A self-reported name cannot register a Blueprint"), MatchesBlueprintSourceIdentity(Fake, ClaimedPath));
	TestTrue(TEXT("Rename the unregistered class"), Fake->Rename(TEXT("Renamed_C"), Package, REN_DontCreateRedirectors | REN_NonTransactional));
	TestFalse(TEXT("Renaming cannot preserve a claimed identity"), MatchesBlueprintSourceIdentity(Fake, ClaimedPath));
	auto* Replacement = NewObject<UBlueprintGeneratedClass>(Package, TEXT("Claimed_C"), RF_Public);
	Replacement->ClassFlags |= CLASS_CompiledFromBlueprint;
	TestFalse(TEXT("A replacement object cannot borrow the claimed identity"), MatchesBlueprintSourceIdentity(Replacement, ClaimedPath));
	return true;
}

#endif

FModKitApiAllowlist::FBlueprintAuditScope::FBlueprintAuditScope(const TArray<TSharedRef<FAngelscriptModuleDesc>>& Modules)
	: Previous(BlueprintAuditModules)
{
	BlueprintAuditModules = &Modules;
}

FModKitApiAllowlist::FBlueprintAuditScope::~FBlueprintAuditScope()
{
	BlueprintAuditModules = Previous;
}

FString FModKitApiAllowlist::StableBlueprintDeclaration(const FString& Declaration, const TMap<FString, FString>& Views)
{
	// Replace just the owner token. Keep return type, member, arguments and constness exact.
	const int32 Begin = Declaration.Find(TEXT("__SoloASBlueprint_"));
	if (Begin == INDEX_NONE || (Begin > 0 && Declaration[Begin - 1] != TEXT(' '))) return Declaration;
	const int32 End = Declaration.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Begin);
	if (End == INDEX_NONE) return Declaration;
	const FString* Asset = Views.Find(Declaration.Mid(Begin, End - Begin));
	return Asset ? Declaration.Left(Begin) + *Asset + Declaration.Mid(End) : Declaration;
}

bool FModKitApiAllowlist::MatchDeclaration(const FString& Declaration, const FString& Pattern)
{
	// Wildcards: a leading and/or trailing '*' turns the pattern into a
	// contains/suffix/prefix match respectively; without any '*' the match is
	// exact. The leading-'*' form matches declarations regardless of their
	// return-type prefix (GetDeclaration includes it, e.g.
	// "UClass TSubclassOf::opImplConv() const").
	const bool bLeadWild = Pattern.StartsWith(TEXT("*"));
	const bool bTailWild = Pattern.EndsWith(TEXT("*"));
	if (bLeadWild && bTailWild && Pattern.Len() > 2)
	{
		return Declaration.Contains(Pattern.Mid(1, Pattern.Len() - 2));
	}
	if (bTailWild)
	{
		return Declaration.StartsWith(Pattern.LeftChop(1));
	}
	if (bLeadWild)
	{
		return Declaration.EndsWith(Pattern.Mid(1));
	}
	return Declaration == Pattern;
}

bool FModKitApiAllowlist::IsAllowed(const FString& Declaration, EApiFace Face, const UModKitSettings& Settings, FString* OutReason)
{
	if (OutReason) OutReason->Reset();
	const TArray<FString>* Patterns = Face == EApiFace::SystemCall ? &Settings.DeniedApiPatterns
		: Face == EApiFace::BaseType ? &Settings.DeniedBaseClassPatterns : &Settings.DeniedOverridePatterns;
	for (const FString& Pattern : Settings.DeniedApiPatterns)
		if (MatchDeclaration(Declaration, Pattern)) return false;
	for (const FString& Pattern : *Patterns)
		if (MatchDeclaration(Declaration, Pattern)) return false;
	FApiGroup Group;
	const EApiMetadataState State = QueryApiGroup(Declaration, Group);
	if (State == EApiMetadataState::Conflict) return false;
	if (State == EApiMetadataState::Classified && Group.Tier.Equals(TEXT("internal"), ESearchCase::IgnoreCase)) return false;
	if (OutReason) *OutReason = TEXT("default-open");
	return true;
}

bool UModKitSettings::IsCapabilityDenied(const FString& Declaration) const
{
	using EFace = FModKitApiAllowlist::EApiFace;
	return !FModKitApiAllowlist::IsAllowed(Declaration, EFace::SystemCall, *this)
		|| !FModKitApiAllowlist::IsAllowed(Declaration, EFace::BaseType, *this)
		|| !FModKitApiAllowlist::IsAllowed(Declaration, EFace::Override, *this);
}

bool FModKitApiAllowlist::CheckApiAllowlist(const FString& ModuleName, const FModKitApiAllowlist::FModApiUsage& InputUsage, FString& OutError)
{
	if (InputUsage.Module && !FModKitCompilationManager::RequiresModAdmission(*InputUsage.Module)) return false;
	auto Usage = InputUsage;
	CollectModuleApi(Usage.Module, Usage);
	// Heartbeat: proves the audit ran per module (guards against a silently
	// empty registry producing a false-green chain).
	UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_API_AUDIT module=%s calls=%d bases=%d overrides=%d"),
		*ModuleName, Usage.SystemCallDeclarations.Num(), Usage.BaseTypeDeclarations.Num(), Usage.OverrideDeclarations.Num());

	const UModKitSettings* Settings = UModKitSettings::Get();
	// The game body and provenance-checked built-in/official FeatureMods are trusted.
	const auto Candidate = AuditedCandidate(ModuleName);
	FModKitSourceOwner Owner;
	if (Usage.Module && (!Candidate.IsValid() || Candidate.Get() != Usage.Module
		|| !FModKitCompilationManager::GetSourceOwner(Candidate.ToSharedRef(), Owner, OutError))) return true;
	if ((Candidate.IsValid() && Owner.Layer != EModKitSourceLayer::Leaf) || IsTrustedFeatureMod(ModuleName, *Settings))
	{
		return false;
	}

	if (!Usage.CollectionError.IsEmpty()) { OutError = Usage.CollectionError; return true; }
	// Collect every unmatched declaration in one pass so a single run reveals
	// the complete offending surface (first-error-only reporting made
	// boilerplate discovery take one run per API).
	TArray<FString> Violations;
	TMap<FString, FString> Views;
	if (!SignedBlueprintViews(ModuleName, Views))
	{
		OutError = FString::Printf(TEXT("MODKIT-API-ALLOWLIST: module %s has an invalid Blueprint identity carrier"), *ModuleName);
		return true;
	}
	const auto CollectNonAllowlisted = [&Violations, Settings, &Views](const TArray<FString>& Declarations, FModKitApiAllowlist::EApiFace Face)
	{
		for (const FString& Declaration : Declarations)
		{
			const FString Stable = Face == EApiFace::SystemCall ? StableBlueprintDeclaration(Declaration, Views) : Declaration;
			// Explicit denies apply to both identities before any stable permission is considered.
			const bool Denied = Settings->DeniedApiPatterns.ContainsByPredicate([&](const FString& Pattern)
				{ return MatchDeclaration(Declaration, Pattern) || MatchDeclaration(Stable, Pattern); });
			if (Denied || !FModKitApiAllowlist::IsAllowed(Declaration, Face, *Settings)
				|| !FModKitApiAllowlist::IsAllowed(Stable, Face, *Settings))
			{
				Violations.Add(Declaration);
			}
		}
	};
	TMap<FString, FString> Properties;
	if (!CollectProperties(ModuleName, Views, Properties))
	{
		OutError = FString::Printf(TEXT("MODKIT-API-ALLOWLIST: module %s has an unresolved property access"), *ModuleName);
		return true;
	}
	for (const auto& Property : Properties)
	{
		const bool Denied = Settings->DeniedApiPatterns.ContainsByPredicate([&](const FString& Pattern)
			{ return MatchDeclaration(Property.Key, Pattern) || MatchDeclaration(Property.Value, Pattern); });
		if (Denied || !IsAllowed(Property.Key, EApiFace::SystemCall, *Settings)
			|| !IsAllowed(Property.Value, EApiFace::SystemCall, *Settings))
			Violations.Add(Property.Key + TEXT(" [stable: ") + Property.Value + TEXT("]"));
	}
	CollectNonAllowlisted(Usage.SystemCallDeclarations, FModKitApiAllowlist::EApiFace::SystemCall);
	CollectNonAllowlisted(Usage.BaseTypeDeclarations, FModKitApiAllowlist::EApiFace::BaseType);
	CollectNonAllowlisted(Usage.OverrideDeclarations, FModKitApiAllowlist::EApiFace::Override);

	if (Violations.Num() == 0)
	{
		return false;
	}
	OutError = FString::Printf(
		TEXT("MODKIT-API-ALLOWLIST: module %s referenced non-allowlisted API '%s'"),
		*ModuleName, *FString::Join(Violations, TEXT("' | '")));
	return true;
}
