// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

#include <new>
#include <type_traits>
#include <utility>

namespace CuvissStockASAutoCaller
{
	template<typename ParamType>
	FORCEINLINE decltype(auto) ReadParameter(void* Parameter)
	{
		using ValueType = typename std::remove_reference<ParamType>::type;

		if constexpr (std::is_reference<ParamType>::value)
			return static_cast<ParamType>(*static_cast<ValueType*>(Parameter));
		else if constexpr (std::is_pointer<ParamType>::value)
			return static_cast<ParamType>(Parameter);
		else
			return *static_cast<ValueType*>(Parameter);
	}

	template<typename ReturnType, typename CallableType>
	FORCEINLINE void InvokeAndStore(void* ReturnValue, CallableType&& Callable)
	{
		if constexpr (std::is_void<ReturnType>::value)
		{
			Callable();
		}
		else if constexpr (std::is_reference<ReturnType>::value)
		{
			auto&& Result = Callable();
			using ValueType = typename std::remove_reference<ReturnType>::type;
			*static_cast<ValueType**>(ReturnValue) = &Result;
		}
		else
		{
			new(ReturnValue) ReturnType(Callable());
		}
	}

	template<typename FunctionPointerType, FunctionPointerType FunctionPointer,
		typename ReturnType, typename... ParamTypes, size_t... Indices>
	FORCEINLINE void CallFunctionThunk(
		void** Parameters,
		void* ReturnValue,
		std::index_sequence<Indices...>)
	{
		InvokeAndStore<ReturnType>(ReturnValue, [&]() -> decltype(auto)
		{
			return FunctionPointer(ReadParameter<ParamTypes>(Parameters[Indices])...);
		});
	}

	template<typename FunctionPointerType, FunctionPointerType FunctionPointer,
		typename ReturnType, typename... ParamTypes>
	void RedirectFunctionCallerThunk(asFUNCTION_t, void** Parameters, void* ReturnValue)
	{
		CallFunctionThunk<FunctionPointerType, FunctionPointer, ReturnType, ParamTypes...>(
			Parameters,
			ReturnValue,
			std::index_sequence_for<ParamTypes...>{});
	}

	template<typename FunctionPointerType, FunctionPointerType FunctionPointer,
		typename ReturnType, typename ObjectType, typename... ParamTypes, size_t... Indices>
	FORCEINLINE void CallMethodThunk(
		void** Parameters,
		void* ReturnValue,
		std::index_sequence<Indices...>)
	{
		ObjectType* Object = static_cast<ObjectType*>(Parameters[0]);
		InvokeAndStore<ReturnType>(ReturnValue, [&]() -> decltype(auto)
		{
			return (Object->*FunctionPointer)(
				ReadParameter<ParamTypes>(Parameters[Indices + 1])...);
		});
	}

	template<typename FunctionPointerType, FunctionPointerType FunctionPointer,
		typename ReturnType, typename ObjectType, typename... ParamTypes>
	void RedirectMethodCallerThunk(asMETHOD_t, void** Parameters, void* ReturnValue)
	{
		CallMethodThunk<FunctionPointerType, FunctionPointer, ReturnType, ObjectType, ParamTypes...>(
			Parameters,
			ReturnValue,
			std::index_sequence_for<ParamTypes...>{});
	}

	template<typename ReturnType, typename... ParamTypes, size_t... Indices>
	FORCEINLINE void CallFunctionRaw(
		asFUNCTION_t Function,
		void** Parameters,
		void* ReturnValue,
		std::index_sequence<Indices...>)
	{
		using FunctionPointerType = ReturnType(*)(ParamTypes...);
		FunctionPointerType TypedFunction = reinterpret_cast<FunctionPointerType>(Function);
		InvokeAndStore<ReturnType>(ReturnValue, [&]() -> decltype(auto)
		{
			return TypedFunction(ReadParameter<ParamTypes>(Parameters[Indices])...);
		});
	}

	template<typename ReturnType, typename... ParamTypes>
	void RedirectFunctionCallerRaw(asFUNCTION_t Function, void** Parameters, void* ReturnValue)
	{
		CallFunctionRaw<ReturnType, ParamTypes...>(
			Function,
			Parameters,
			ReturnValue,
			std::index_sequence_for<ParamTypes...>{});
	}

	template<typename MethodPointerType>
	FORCEINLINE MethodPointerType RestoreMethodPointer(asMETHOD_t Function)
	{
		static_assert(
			sizeof(MethodPointerType) <= sizeof(asMETHOD_t),
			"AngelScript method pointer storage must preserve the native member pointer.");

		MethodPointerType TypedFunction;
		FMemory::Memcpy(&TypedFunction, &Function, sizeof(TypedFunction));
		return TypedFunction;
	}

	template<typename MethodPointerType, typename ReturnType, typename ObjectType,
		typename... ParamTypes, size_t... Indices>
	FORCEINLINE void CallMethodRaw(
		asMETHOD_t Function,
		void** Parameters,
		void* ReturnValue,
		std::index_sequence<Indices...>)
	{
		static_assert(
			std::is_member_function_pointer<MethodPointerType>::value,
			"Raw AngelScript method callers require an exact pointer-to-member type.");
		MethodPointerType TypedFunction = RestoreMethodPointer<MethodPointerType>(Function);
		ObjectType* Object = static_cast<ObjectType*>(Parameters[0]);
		InvokeAndStore<ReturnType>(ReturnValue, [&]() -> decltype(auto)
		{
			return (Object->*TypedFunction)(
				ReadParameter<ParamTypes>(Parameters[Indices + 1])...);
		});
	}

	template<typename MethodPointerType, typename ReturnType, typename ObjectType, typename... ParamTypes>
	void RedirectMethodCallerRaw(asMETHOD_t Function, void** Parameters, void* ReturnValue)
	{
		CallMethodRaw<MethodPointerType, ReturnType, ObjectType, ParamTypes...>(
			Function,
			Parameters,
			ReturnValue,
			std::index_sequence_for<ParamTypes...>{});
	}
}
