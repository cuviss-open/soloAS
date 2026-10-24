// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "as_builder.h"
#include "as_compiler.h"

#ifndef AS_NO_COMPILER

BEGIN_AS_NAMESPACE

namespace Cuviss::NativeEventSuper
{
	/**
	 * Defers native-event super candidate policy until the stock builder has
	 * completed, including when the stock path returns before collecting a
	 * method for a native shadow type.
	 */
	class FBuilderPostProcessGuard final
	{
	public:
		FBuilderPostProcessGuard(FBuilderPostProcessGuard&& Other) noexcept;
		~FBuilderPostProcessGuard();

		FBuilderPostProcessGuard(const FBuilderPostProcessGuard&) = delete;
		FBuilderPostProcessGuard& operator=(
			const FBuilderPostProcessGuard&) = delete;
		FBuilderPostProcessGuard& operator=(
			FBuilderPostProcessGuard&&) = delete;

	private:
		friend FBuilderPostProcessGuard PostProcessBuilder(
			asCBuilder&,
			const char*,
			asCObjectType*,
			asCArray<int>&,
			bool,
			bool,
			bool);

		FBuilderPostProcessGuard(
			asCBuilder& InBuilder,
			const char* InName,
			asCObjectType* InOriginalObjectType,
			asCArray<int>& InMethods,
			bool bInObjectIsConst,
			bool bInSuperScope,
			bool bInConstDueToTemporary);

		asCBuilder* Builder = nullptr;
		const char* Name = nullptr;
		asCObjectType* OriginalObjectType = nullptr;
		asCArray<int>* Methods = nullptr;
		bool bObjectIsConst = false;
		bool bSuperScope = false;
		bool bConstDueToTemporary = false;
	};

	FBuilderPostProcessGuard PostProcessBuilder(
		asCBuilder& Builder,
		const char* Name,
		asCObjectType* OriginalObjectType,
		asCArray<int>& Methods,
		bool bObjectIsConst,
		bool bSuperScope,
		bool bConstDueToTemporary);

	/** Removes native aliases superseded by a matching script candidate. */
	void PostProcessCompiler(
		asCBuilder& Builder,
		bool bSuperScope,
		asCArray<asSOverloadCandidate>& MatchingFunctions);
}

END_AS_NAMESPACE

#endif
