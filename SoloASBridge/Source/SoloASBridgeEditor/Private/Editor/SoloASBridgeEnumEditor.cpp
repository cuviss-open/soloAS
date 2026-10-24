#include "Editor/SoloASBridgeEnumEditor.h"

#include "CoreGlobals.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "K2Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "NodeDependingOnEnumInterface.h"
#include "UObject/UObjectIterator.h"

namespace
{
bool HasEnumChanged(
    const UUserDefinedEnum& Enum,
    const TArray<TPair<FName, int64>>& OldNames
)
{
    const int32 EnumeratorCount = FMath::Max(0, Enum.NumEnums() - 1);
    if (EnumeratorCount != OldNames.Num())
    {
        return true;
    }

    for (int32 Index = 0; Index < EnumeratorCount; ++Index)
    {
        if (Enum.GetNameByIndex(Index) != OldNames[Index].Key
            || Enum.GetValueByIndex(Index) != OldNames[Index].Value)
        {
            return true;
        }
    }
    return false;
}

bool IsRefreshableNode(const UK2Node* Node)
{
    return IsValid(Node)
        && Cast<UEdGraph>(Node->GetOuter()) != nullptr
        && !Node->HasAnyFlags(RF_Transient)
        && Node->HasValidBlueprint();
}

void AddBlueprint(TArray<UBlueprint*>& Blueprints, UBlueprint* Blueprint)
{
    Blueprints.AddUnique(Blueprint);
}

void RefreshDependentNodes(
    UUserDefinedEnum& Enum,
    TArray<UBlueprint*>& Blueprints
)
{
    for (TObjectIterator<UK2Node> It(RF_Transient); It; ++It)
    {
        UK2Node* Node = *It;
        if (!IsRefreshableNode(Node))
        {
            continue;
        }

        INodeDependingOnEnumInterface* EnumDependent =
            Cast<INodeDependingOnEnumInterface>(Node);
        if (EnumDependent == nullptr || EnumDependent->GetEnum() != &Enum)
        {
            continue;
        }

        if (EnumDependent->ShouldBeReconstructedAfterEnumChanged())
        {
            Node->ReconstructNode();
        }
        AddBlueprint(Blueprints, Node->GetBlueprint());
    }
}

void RepairInvalidInputDefaults(
    UUserDefinedEnum& Enum,
    TArray<UBlueprint*>& Blueprints
)
{
    for (TObjectIterator<UEdGraphNode> It(RF_Transient); It; ++It)
    {
        UK2Node* Node = Cast<UK2Node>(*It);
        if (!IsRefreshableNode(Node))
        {
            continue;
        }

        const TArray<UEdGraphPin*> Pins = Node->Pins;
        for (UEdGraphPin* Pin : Pins)
        {
            if (Pin == nullptr
                || !Node->Pins.Contains(Pin)
                || Pin->Direction != EGPD_Input
                || Pin->PinType.PinSubCategory == UEdGraphSchema_K2::PSC_Bitmask
                || Pin->PinType.PinSubCategoryObject.Get() != &Enum
                || Enum.GetIndexByNameString(Pin->DefaultValue) != INDEX_NONE)
            {
                continue;
            }

            Pin->Modify();
            UBlueprint* Blueprint = Node->GetBlueprint();
            if (Blueprint->BlueprintType == BPTYPE_Interface
                && Enum.NumEnums() > 1)
            {
                Pin->DefaultValue = Enum.GetNameStringByIndex(0);
            }
            else
            {
                Pin->DefaultValue = TEXT("(INVALID)");
            }
            Node->PinDefaultValueChanged(Pin);
            AddBlueprint(Blueprints, Blueprint);
        }
    }
}
}

void SoloASBridge::RefreshRuntimeEnumEditorDependents(
    UUserDefinedEnum& Enum,
    const TArray<TPair<FName, int64>>& OldNames
)
{
    check(IsInGameThread());
    if (!HasEnumChanged(Enum, OldNames))
    {
        return;
    }

    TArray<UBlueprint*> Blueprints;
    RefreshDependentNodes(Enum, Blueprints);
    RepairInvalidInputDefaults(Enum, Blueprints);
    Blueprints.Sort([](const UBlueprint& Left, const UBlueprint& Right)
    {
        return Left.GetPathName() < Right.GetPathName();
    });

    for (UBlueprint* Blueprint : Blueprints)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        Blueprint->BroadcastChanged();
    }

    FEnumEditorUtils::FEnumEditorManager::Get().PostChange(
        &Enum,
        FEnumEditorUtils::EEnumEditorChangeInfo::Changed
    );
}
