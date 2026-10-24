// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassReloadEventSignatureResolver.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "Engine/MemberReference.h"

UFunction* ResolveClassReloadEventSignature(const UK2Node_Event* EventNode)
{
	if (EventNode == nullptr)
	{
		return nullptr;
	}

	if (UFunction* Signature = EventNode->FindEventSignatureFunction())
	{
		return Signature;
	}

	const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(EventNode);
	if (CustomEvent == nullptr)
	{
		return nullptr;
	}

	const UEdGraphPin* DelegateOutput = CustomEvent->GetDelegatePin();
	if (DelegateOutput == nullptr)
	{
		return nullptr;
	}

	for (const UEdGraphPin* LinkedPin : DelegateOutput->LinkedTo)
	{
		if (LinkedPin == nullptr)
		{
			continue;
		}

		if (const UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(LinkedPin->GetOwningNode()))
		{
			if (UFunction* Signature = DelegateNode->GetDelegateSignature())
			{
				return Signature;
			}
		}

		if (LinkedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
		{
			if (UFunction* Signature =
				FMemberReference::ResolveSimpleMemberReference<UFunction>(
					LinkedPin->PinType.PinSubCategoryMemberReference))
			{
				return Signature;
			}
		}
	}

	return nullptr;
}
