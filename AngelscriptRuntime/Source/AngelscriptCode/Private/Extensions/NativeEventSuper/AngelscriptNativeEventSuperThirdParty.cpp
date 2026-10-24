// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "Extensions/NativeEventSuper/AngelscriptNativeEventSuperThirdParty.h"
#ifndef AS_NO_COMPILER
#include "Extensions/NativeEventSuper/AngelscriptNativeEventSuperInternal.h"
#include "AngelscriptType.h"
#include "UObject/Class.h"
BEGIN_AS_NAMESPACE
namespace Cuviss::NativeEventSuper
{
	namespace
	{
		bool AddUniqueType(
			asCArray<asCObjectType*>& Types,
			asCObjectType* Type)
		{
			if (Type == nullptr)
				return false;
			for (asUINT Index = 0; Index < Types.GetLength(); ++Index)
			{
				if (Types[Index] == Type)
					return false;
			}
			Types.PushLast(Type);
			return true;
		}
		void AddShadowChain(
			asCArray<asCObjectType*>& Types,
			asITypeInfo* FirstShadow)
		{
			asCObjectType* Shadow = static_cast<asCObjectType*>(FirstShadow);
			while (Shadow != nullptr)
			{
				if (!AddUniqueType(Types, Shadow))
					break;
				Shadow = static_cast<asCObjectType*>(Shadow->shadowType);
			}
		}
		void AddRegisteredNativeSources(
			asCArray<asCObjectType*>& Sources,
			asCObjectType* Source)
		{
			if (Source == nullptr
				|| (Source->flags & asOBJ_SCRIPT_OBJECT) != 0
				|| Source->plainUserData == 0)
			{
				return;
			}
			UClass* NativeClass = reinterpret_cast<UClass*>(Source->plainUserData);
			TSharedPtr<FAngelscriptType> RuntimeType =
				FAngelscriptType::GetByClass(NativeClass);
			UClass* RegisteredClass = RuntimeType.IsValid()
				? RuntimeType->GetClass(FAngelscriptTypeUsage::DefaultUsage)
				: nullptr;
			if (!RuntimeType.IsValid() || RegisteredClass != NativeClass)
				return;
			asITypeInfo* CanonicalTypeInfo =
				RuntimeType->GetAngelscriptTypeInfo(
					FAngelscriptTypeUsage::DefaultUsage);
			AddUniqueType(Sources, static_cast<asCObjectType*>(CanonicalTypeInfo));
			TArray<UClass*> NativeInterfaces;
			for (const FImplementedInterface& Implemented : NativeClass->Interfaces)
			{
				if (Implemented.Class != nullptr)
					NativeInterfaces.Add(Implemented.Class);
			}
			TSet<UClass*> VisitedInterfaces;
			for (int32 Index = 0; Index < NativeInterfaces.Num(); ++Index)
			{
				UClass* NativeInterface = NativeInterfaces[Index];
				if (NativeInterface == nullptr
					|| VisitedInterfaces.Contains(NativeInterface)
					|| !NativeInterface->HasAnyClassFlags(CLASS_Interface))
				{
					continue;
				}
				VisitedInterfaces.Add(NativeInterface);
				TSharedPtr<FAngelscriptType> InterfaceType =
					FAngelscriptType::GetByClass(NativeInterface);
				UClass* RegisteredInterface = InterfaceType.IsValid()
					? InterfaceType->GetClass(
						FAngelscriptTypeUsage::DefaultUsage)
					: nullptr;
				if (!InterfaceType.IsValid()
					|| RegisteredInterface != NativeInterface)
				{
					continue;
				}
				asITypeInfo* InterfaceTypeInfo =
					InterfaceType->GetAngelscriptTypeInfo(
						FAngelscriptTypeUsage::DefaultUsage);
				asCObjectType* InterfaceObjectType =
					static_cast<asCObjectType*>(InterfaceTypeInfo);
				if (InterfaceObjectType != nullptr
                    && (InterfaceObjectType->IsInterface()
                        || (InterfaceObjectType->flags & asOBJ_IMPLICIT_HANDLE) != 0))
				{
					AddUniqueType(Sources, InterfaceObjectType);
				}
				for (const FImplementedInterface& Parent
					: NativeInterface->Interfaces)
				{
					if (Parent.Class != nullptr)
						NativeInterfaces.Add(Parent.Class);
				}
			}
		}
		asCArray<asCObjectType*> FindAliasSources(asCObjectType* Root)
		{
			asCArray<asCObjectType*> Sources;
			asCObjectType* Parent = Root != nullptr ? Root->derivedFrom : nullptr;
			if (Parent == nullptr && Root != nullptr)
				AddShadowChain(Sources, Root->shadowType);
			while (Parent != nullptr)
			{
				AddUniqueType(Sources, Parent);
				AddShadowChain(Sources, Parent->shadowType);
				Parent = Parent->derivedFrom;
			}
			for (asUINT Index = 0; Index < Sources.GetLength(); ++Index)
				AddRegisteredNativeSources(Sources, Sources[Index]);
			return Sources;
		}
		void AddUniqueAlias(
			asCArray<asCScriptFunction*>& Aliases,
			asCScriptFunction* Function)
		{
			if (!IsAngelscriptNativeEventSuperAlias(Function))
				return;
			for (asUINT Index = 0; Index < Aliases.GetLength(); ++Index)
			{
				if (Aliases[Index] == Function)
					return;
			}
			Aliases.PushLast(Function);
		}
		asCArray<asCScriptFunction*> FindAliases(
			const char* Name,
			asCObjectType* Root)
		{
			asCArray<asCScriptFunction*> Aliases;
			const asCString AliasName = asCString(Name) + "_Implementation";
			asCArray<asCObjectType*> Sources = FindAliasSources(Root);
			for (asUINT Index = 0; Index < Sources.GetLength(); ++Index)
			{
				Sources[Index]->FindMethod(
					AliasName.AddressOf(),
					[&Aliases](asCScriptFunction* Function)
					{
						AddUniqueAlias(Aliases, Function);
					});
			}
			return Aliases;
		}
		asCObjectType* FindNearestScriptImplementationType(
			const char* Name,
			asCObjectType* Root)
		{
			asCObjectType* Parent = Root != nullptr ? Root->derivedFrom : nullptr;
			if (Parent == nullptr)
				return nullptr;
			asCObjectType* NearestType = nullptr;
			const asCString ImplementationName =
				asCString(Name) + "_Implementation";
			Parent->FindMethod(
				ImplementationName.AddressOf(),
				[&NearestType](asCScriptFunction* Function)
				{
					if (NearestType == nullptr
						&& Function != nullptr
						&& Function->funcType == asFUNC_SCRIPT
						&& !IsAngelscriptNativeEventSuperAlias(Function))
					{
						NearestType = Function->objectType;
					}
				});
			return NearestType;
		}
		bool HaveCompatibleParameterTypes(
			const asCScriptFunction* ScriptCandidate,
			const asCScriptFunction* NativeAlias)
		{
			if (ScriptCandidate->parameterTypes.GetLength()
				!= NativeAlias->parameterTypes.GetLength())
			{
				return false;
			}
			for (asUINT Index = 0;
				Index < ScriptCandidate->parameterTypes.GetLength(); ++Index)
			{
				const asCDataType& ScriptParameter =
					ScriptCandidate->parameterTypes[Index];
				const asCDataType& AliasParameter =
					NativeAlias->parameterTypes[Index];
				if (ScriptParameter == AliasParameter)
					continue;
				if (!ScriptParameter.IsPrimitive()
					|| !AliasParameter.IsPrimitive()
					|| !ScriptParameter.IsEqualExceptConst(AliasParameter))
				{
					return false;
				}
			}
			return true;
		}
		bool IsCompatibleScriptCandidate(
			const asCScriptFunction* ScriptCandidate,
			const asCScriptFunction* NativeAlias)
		{
			return ScriptCandidate != nullptr
				&& NativeAlias != nullptr
				&& ScriptCandidate->funcType == asFUNC_SCRIPT
				&& IsAngelscriptNativeEventSuperAlias(NativeAlias)
				&& ScriptCandidate->returnType == NativeAlias->returnType
				&& ScriptCandidate->IsReadOnly() == NativeAlias->IsReadOnly()
				&& (ScriptCandidate->objectType != nullptr)
					== (NativeAlias->objectType != nullptr)
				&& ScriptCandidate->inOutFlags == NativeAlias->inOutFlags
				&& HaveCompatibleParameterTypes(ScriptCandidate, NativeAlias);
		}
		bool IsAmbiguousAlias(
			const asCArray<asCScriptFunction*>& Aliases,
			const asCScriptFunction* Alias)
		{
			for (asUINT Index = 0; Index < Aliases.GetLength(); ++Index)
			{
				asCScriptFunction* Other = Aliases[Index];
				if (Other != Alias
					&& Alias->IsSignatureExceptNameAndReturnTypeEqual(Other))
				{
					return true;
				}
			}
			return false;
		}
		void NormalizeScriptCandidates(
			asCBuilder& Builder,
			asCArray<int>& Methods,
			asCObjectType*& NearestScriptType)
		{
			for (asUINT Index = 0; Index < Methods.GetLength(); ++Index)
			{
				asCScriptFunction* Function =
					Builder.GetFunctionDescription(Methods[Index]);
				if (Function == nullptr
					|| Function->funcType != asFUNC_SCRIPT
					|| IsAngelscriptNativeEventSuperAlias(Function))
				{
					Methods.RemoveIndex(Index--);
					continue;
				}
				if (NearestScriptType == nullptr)
					NearestScriptType = Function->objectType;
				else if (Function->objectType != NearestScriptType)
					Methods.RemoveIndex(Index--);
			}
		}
		bool AcceptAliasForObject(
			const asCScriptFunction* Alias,
			bool bObjectIsConst,
			bool bConstDueToTemporary);
		void CollectOrdinaryCandidates(
			const char* Name,
			asCObjectType* Root,
			asCArray<int>& Methods,
			asCArray<asCScriptFunction*>& Aliases,
			asCObjectType*& NearestScriptType,
			bool bObjectIsConst,
			bool bConstDueToTemporary)
		{
			asCObjectType* Parent = Root != nullptr ? Root->derivedFrom : nullptr;
			if (Parent == nullptr)
				return;
			Parent->FindMethod(
				Name,
				[&](asCScriptFunction* Function)
				{
					if (IsAngelscriptNativeEventSuperAlias(Function))
					{
						AddUniqueAlias(Aliases, Function);
						return;
					}
					if (Function == nullptr || Function->funcType != asFUNC_SCRIPT)
						return;
					if (NearestScriptType == nullptr)
						NearestScriptType = Function->objectType;
					else if (Function->objectType != NearestScriptType)
						return;
					if (!AcceptAliasForObject(
						Function, bObjectIsConst, bConstDueToTemporary))
					{
						return;
					}
					Methods.PushLast(Function->id);
				});
		}
		bool HasCompatibleScriptCandidate(
			asCBuilder& Builder,
			const asCArray<int>& Methods,
			const asCScriptFunction* Alias)
		{
                                                                                                                  
			// A script implementation is the primary Super target. Native aliases
			// are only a fallback when no script implementation was found; their
			// declaration may retain native const/reference spelling that differs
			// from the normalized script override signature.
			if (Methods.GetLength() > 0)
				return true;
                                                                                                                
			for (asUINT Index = 0; Index < Methods.GetLength(); ++Index)
			{
				if (IsCompatibleScriptCandidate(
					Builder.GetFunctionDescription(Methods[Index]), Alias))
				{
					return true;
				}
			}
			return false;
		}
		bool AcceptAliasForObject(
			const asCScriptFunction* Alias,
			bool bObjectIsConst,
			bool bConstDueToTemporary)
		{
			if (Alias->IsReadOnly())
				return true;
			if (bObjectIsConst)
				return false;
			return !bConstDueToTemporary
				|| Alias->traits.GetTrait(asTRAIT_ACCEPT_TEMPORARY_OBJECT);
		}
		void ProcessBuilderCandidates(
			asCBuilder& Builder,
			const char* Name,
			asCObjectType* OriginalObjectType,
			asCArray<int>& Methods,
			bool bObjectIsConst,
			bool bConstDueToTemporary)
		{
			asCObjectType* NearestScriptType =
				FindNearestScriptImplementationType(Name, OriginalObjectType);
			NormalizeScriptCandidates(Builder, Methods, NearestScriptType);
			asCArray<asCScriptFunction*> Aliases =
				FindAliases(Name, OriginalObjectType);
			if (Methods.GetLength() == 0)
			{
				CollectOrdinaryCandidates(
					Name,
					OriginalObjectType,
					Methods,
					Aliases,
					NearestScriptType,
					bObjectIsConst,
					bConstDueToTemporary);
			}
			for (asUINT Index = 0; Index < Aliases.GetLength(); ++Index)
			{
				asCScriptFunction* Alias = Aliases[Index];
				if (IsAmbiguousAlias(Aliases, Alias)
					|| HasCompatibleScriptCandidate(Builder, Methods, Alias)
					|| !AcceptAliasForObject(
						Alias, bObjectIsConst, bConstDueToTemporary))
				{
					continue;
				}
				Methods.PushLast(Alias->id);
			}
		}
	}
	FBuilderPostProcessGuard::FBuilderPostProcessGuard(
		asCBuilder& InBuilder,
		const char* InName,
		asCObjectType* InOriginalObjectType,
		asCArray<int>& InMethods,
		bool bInObjectIsConst,
		bool bInSuperScope,
		bool bInConstDueToTemporary)
		: Builder(&InBuilder)
		, Name(InName)
		, OriginalObjectType(InOriginalObjectType)
		, Methods(&InMethods)
		, bObjectIsConst(bInObjectIsConst)
		, bSuperScope(bInSuperScope)
		, bConstDueToTemporary(bInConstDueToTemporary)
	{
	}
	FBuilderPostProcessGuard::FBuilderPostProcessGuard(
		FBuilderPostProcessGuard&& Other) noexcept
		: Builder(Other.Builder)
		, Name(Other.Name)
		, OriginalObjectType(Other.OriginalObjectType)
		, Methods(Other.Methods)
		, bObjectIsConst(Other.bObjectIsConst)
		, bSuperScope(Other.bSuperScope)
		, bConstDueToTemporary(Other.bConstDueToTemporary)
	{
		Other.Builder = nullptr;
		Other.Methods = nullptr;
	}
	FBuilderPostProcessGuard::~FBuilderPostProcessGuard()
	{
		if (Builder == nullptr || Methods == nullptr)
			return;
                                                                                                          
		// The native-event super alias is an internal _Implementation lookup
		// entry. It must not participate in ordinary method lookup or duplicate
		// checks; only the Super scope fallback may reintroduce it.
		for (asUINT Index = Methods->GetLength(); Index > 0; --Index)
		{
			const asUINT MethodIndex = Index - 1;
			asCScriptFunction* Function =
				Builder->GetFunctionDescription((*Methods)[MethodIndex]);
			if (IsAngelscriptNativeEventSuperAlias(Function))
				Methods->RemoveIndex(MethodIndex);
		}
                                                                                                        

		if (!bSuperScope)
			return;

		ProcessBuilderCandidates(
			*Builder,
			Name,
			OriginalObjectType,
			*Methods,
			bObjectIsConst,
			bConstDueToTemporary);
	}

	FBuilderPostProcessGuard PostProcessBuilder(
		asCBuilder& Builder,
		const char* Name,
		asCObjectType* OriginalObjectType,
		asCArray<int>& Methods,
		bool bObjectIsConst,
		bool bSuperScope,
		bool bConstDueToTemporary)
	{
		return FBuilderPostProcessGuard(
			Builder,
			Name,
			OriginalObjectType,
			Methods,
			bObjectIsConst,
			bSuperScope,
			bConstDueToTemporary);
	}

	void PostProcessCompiler(
		asCBuilder& Builder,
		bool bSuperScope,
		asCArray<asSOverloadCandidate>& MatchingFunctions)
	{
		if (!bSuperScope)
			return;
		for (asUINT AliasIndex = 0;
			AliasIndex < MatchingFunctions.GetLength(); ++AliasIndex)
		{
			asCScriptFunction* NativeAlias = Builder.GetFunctionDescription(
				MatchingFunctions[AliasIndex].funcId);
			if (!IsAngelscriptNativeEventSuperAlias(NativeAlias))
				continue;

			bool bHasCompatibleScriptCandidate = false;
			for (asUINT CandidateIndex = 0;
				CandidateIndex < MatchingFunctions.GetLength(); ++CandidateIndex)
			{
				asCScriptFunction* ScriptCandidate = Builder.GetFunctionDescription(
					MatchingFunctions[CandidateIndex].funcId);
				if (ScriptCandidate == nullptr
					|| ScriptCandidate->funcType != asFUNC_SCRIPT
					|| IsAngelscriptNativeEventSuperAlias(ScriptCandidate))
				{
					continue;
				}
				if (ScriptCandidate->IsSignatureExceptNameAndObjectTypeEqual(
					NativeAlias))
				{
					bHasCompatibleScriptCandidate = true;
					break;
				}
			}

			if (!bHasCompatibleScriptCandidate)
				continue;
			if (AliasIndex == MatchingFunctions.GetLength() - 1)
				MatchingFunctions.PopLast();
			else
				MatchingFunctions[AliasIndex] = MatchingFunctions.PopLast();
			--AliasIndex;
		}
	}
}

END_AS_NAMESPACE

#endif
