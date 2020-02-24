#include "FlowGraphNode.h"
#include "FlowEditorSettings.h"
#include "Graph/FlowAssetGraph.h"
#include "Graph/FlowDebugger.h"
#include "Graph/FlowGraphCommands.h"
#include "Graph/FlowGraphSchema.h"
#include "Graph/Widgets/SFlowGraphNode.h"

#include "FlowAsset.h"
#include "Nodes/FlowNode.h"

#include "Developer/ToolMenus/Public/ToolMenus.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Font.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GraphEditorActions.h"
#include "ScopedTransaction.h"
#include "UnrealEd.h"

#define LOCTEXT_NAMESPACE "FlowGraphNode"

void FFlowBreakpoint::AddBreakpoint()
{
	if (!bHasBreakpoint)
	{
		bHasBreakpoint = true;
		bBreakpointEnabled = true;
	}
}

void FFlowBreakpoint::RemoveBreakpoint()
{
	if (bHasBreakpoint)
	{
		bHasBreakpoint = false;
		bBreakpointEnabled = false;
	}
}

bool FFlowBreakpoint::HasBreakpoint() const
{
	return bHasBreakpoint;
}

void FFlowBreakpoint::EnableBreakpoint()
{
	if (bHasBreakpoint && !bBreakpointEnabled)
	{
		bBreakpointEnabled = true;
	}
}

bool FFlowBreakpoint::CanEnableBreakpoint() const
{
	return bHasBreakpoint && !bBreakpointEnabled;
}

void FFlowBreakpoint::DisableBreakpoint()
{
	if (bHasBreakpoint && bBreakpointEnabled)
	{
		bBreakpointEnabled = false;
	}
}

bool FFlowBreakpoint::IsBreakpointEnabled() const
{
	return bHasBreakpoint && bBreakpointEnabled;
}

void FFlowBreakpoint::ToggleBreakpoint()
{
	if (bHasBreakpoint)
	{
		bHasBreakpoint = false;
		bBreakpointEnabled = false;
	}
	else
	{
		bHasBreakpoint = true;
		bBreakpointEnabled = true;
	}
}

UFlowGraphNode::UFlowGraphNode(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UFlowGraphNode::SetFlowNode(UFlowNode* InFlowNode)
{
	FlowNode = InFlowNode;
	InFlowNode->SetGraphNode(this);
}

UFlowNode* UFlowGraphNode::GetFlowNode() const
{
	if (FlowNode)
	{
		if (UFlowAsset* RuntimeIntance = FlowNode->GetFlowAsset()->GetInspectedInstance())
		{
			return RuntimeIntance->GetNodeInstance(FlowNode->GetGuid());
		}

		return FlowNode;
	}

	return nullptr;
}

void UFlowGraphNode::PostLoad()
{
	Super::PostLoad();

	// Fixup any node pointers that may be out of date
	if (FlowNode)
	{
		FlowNode->SetGraphNode(this);
	}

	// Fixup pins after changes in node's definition
	ReconstructNode();
}

void UFlowGraphNode::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);

	if (!bDuplicateForPIE)
	{
		CreateNewGuid();
	}
}

void UFlowGraphNode::PostEditImport()
{
	// Make sure this FlowNode is owned by the FlowAsset it's being pasted into.
	ResetFlowNodeOwner();
}

void UFlowGraphNode::PrepareForCopying()
{
	if (FlowNode)
	{
		// Temporarily take ownership of the FlowNode, so that it is not deleted when cutting
		FlowNode->Rename(nullptr, this, REN_DontCreateRedirectors);
	}
}

void UFlowGraphNode::PostCopyNode()
{
	// Make sure the FlowNode goes back to being owned by the FlowAsset after copying.
	ResetFlowNodeOwner();
}

void UFlowGraphNode::ResetFlowNodeOwner()
{
	if (FlowNode)
	{
		UFlowAsset* FlowAsset = CastChecked<UFlowAssetGraph>(GetGraph())->GetFlowAsset();

		if (FlowNode->GetOuter() != FlowAsset)
		{
			// Ensures FlowNode is owned by the FlowAsset
			FlowNode->Rename(nullptr, FlowAsset, REN_DontCreateRedirectors);
		}

		// Set up the back pointer for newly created flow nodes
		FlowNode->SetGraphNode(this);
	}
}

bool UFlowGraphNode::CanCreateUnderSpecifiedSchema(const UEdGraphSchema* Schema) const
{
	return Schema->IsA(UFlowGraphSchema::StaticClass());
}

void UFlowGraphNode::AutowireNewNode(UEdGraphPin* FromPin)
{
	if (FromPin != nullptr)
	{
		const UFlowGraphSchema* Schema = CastChecked<UFlowGraphSchema>(GetSchema());

		TSet<UEdGraphNode*> NodeList;

		// auto-connect from dragged pin to first compatible pin on the new node
		for (UEdGraphPin* Pin : Pins)
		{
			check(Pin);
			FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, Pin);
			if (ECanCreateConnectionResponse::CONNECT_RESPONSE_MAKE == Response.Response)
			{
				if (Schema->TryCreateConnection(FromPin, Pin))
				{
					NodeList.Add(FromPin->GetOwningNode());
					NodeList.Add(this);
				}
				break;
			}
			else if (ECanCreateConnectionResponse::CONNECT_RESPONSE_BREAK_OTHERS_A == Response.Response)
			{
				InsertNewNode(FromPin, Pin, NodeList);
				break;
			}
		}

		// Send all nodes that received a new pin connection a notification
		for (auto It = NodeList.CreateConstIterator(); It; ++It)
		{
			UEdGraphNode* Node = (*It);
			Node->NodeConnectionListChanged();
		}
	}
}

void UFlowGraphNode::InsertNewNode(UEdGraphPin* FromPin, UEdGraphPin* NewLinkPin, TSet<UEdGraphNode*>& OutNodeList)
{
	const UFlowGraphSchema* Schema = CastChecked<UFlowGraphSchema>(GetSchema());

	// The pin we are creating from already has a connection that needs to be broken. We want to "insert" the new node in between, so that the output of the new node is hooked up too
	UEdGraphPin* OldLinkedPin = FromPin->LinkedTo[0];
	check(OldLinkedPin);

	FromPin->BreakAllPinLinks();

	// Hook up the old linked pin to the first valid output pin on the new node
	for (int32 OutpinPinIdx = 0; OutpinPinIdx < Pins.Num(); OutpinPinIdx++)
	{
		UEdGraphPin* OutputExecPin = Pins[OutpinPinIdx];
		check(OutputExecPin);
		if (ECanCreateConnectionResponse::CONNECT_RESPONSE_MAKE == Schema->CanCreateConnection(OldLinkedPin, OutputExecPin).Response)
		{
			if (Schema->TryCreateConnection(OldLinkedPin, OutputExecPin))
			{
				OutNodeList.Add(OldLinkedPin->GetOwningNode());
				OutNodeList.Add(this);
			}
			break;
		}
	}

	if (Schema->TryCreateConnection(FromPin, NewLinkPin))
	{
		OutNodeList.Add(FromPin->GetOwningNode());
		OutNodeList.Add(this);
	}
}

void UFlowGraphNode::ReconstructNode()
{
	// Break any links to 'orphan' pins
	for (UEdGraphPin* Pin : Pins)
	{
		TArray<class UEdGraphPin*>& LinkedToRef = Pin->LinkedTo;
		for (int32 LinkIdx = 0; LinkIdx < LinkedToRef.Num(); LinkIdx++)
		{
			UEdGraphPin* OtherPin = LinkedToRef[LinkIdx];
			// If we are linked to a pin that its owner doesn't know about, break that link
			if (!OtherPin->GetOwningNode()->Pins.Contains(OtherPin))
			{
				Pin->LinkedTo.Remove(OtherPin);
			}
		}
	}

	// cache pins in arrays
	for (UEdGraphPin* Pin : Pins)
	{
		if (Pin->Direction == EGPD_Input)
		{
			InputPins.Add(Pin);
		}
		else
		{
			OutputPins.Add(Pin);
		}
	}

	// Store the old Input and Output pins
	TArray<UEdGraphPin*> OldInputPins(InputPins);
	TArray<UEdGraphPin*> OldOutputPins(OutputPins);

	// Move the existing pins to a saved array
	TArray<UEdGraphPin*> OldPins(Pins);
	Pins.Reset();
	InputPins.Reset();
	OutputPins.Reset();

	// Recreate the new pins
	AllocateDefaultPins();

	// Get new Input pins
	for (int32 PinIndex = 0; PinIndex < OldInputPins.Num(); PinIndex++)
	{
		if (PinIndex < InputPins.Num())
		{
			InputPins[PinIndex]->MovePersistentDataFromOldPin(*OldInputPins[PinIndex]);
		}
	}

	// Get new Output pins
	for (int32 PinIndex = 0; PinIndex < OldOutputPins.Num(); PinIndex++)
	{
		if (PinIndex < OutputPins.Num())
		{
			OutputPins[PinIndex]->MovePersistentDataFromOldPin(*OldOutputPins[PinIndex]);
		}
	}

	// Throw away the original pins
	for (UEdGraphPin* OldPin : OldPins)
	{
		OldPin->Modify();
		UEdGraphNode::DestroyPin(OldPin);
	}
}

void UFlowGraphNode::AllocateDefaultPins()
{
	check(Pins.Num() == 0);

	if (FlowNode)
	{
		for (const FName& InputName : FlowNode->InputNames)
		{
			CreateInputPin(InputName);
		}

		for (const FName& OutputName : FlowNode->OutputNames)
		{
			CreateOutputPin(OutputName);
		}
	}
}

void UFlowGraphNode::GetNodeContextMenuActions(class UToolMenu* Menu, class UGraphNodeContextMenuContext* Context) const
{
	const FGenericCommands& GenericCommands = FGenericCommands::Get();
	const FGraphEditorCommandsImpl& GraphCommands = FGraphEditorCommands::Get();
	const FFlowGraphCommands& FlowGraphCommands = FFlowGraphCommands::Get();

	if (Context->Pin)
	{
		{
			FToolMenuSection& Section = Menu->AddSection("FlowGraphSchemaPinActions", LOCTEXT("PinActionsMenuHeader", "Pin Actions"));
			if (Context->Pin->LinkedTo.Num() > 0)
			{
				Section.AddMenuEntry(GraphCommands.BreakPinLinks);
			}

			if (Context->Pin->Direction == EGPD_Input && CanUserRemoveInput(Context->Pin))
			{
				Section.AddMenuEntry(FlowGraphCommands.RemovePin);
			}
			else if (Context->Pin->Direction == EGPD_Output && CanUserRemoveOutput(Context->Pin))
			{
				Section.AddMenuEntry(FlowGraphCommands.RemovePin);
			}
		}

		{
			FToolMenuSection& Section = Menu->AddSection("FlowGraphNodePinBreakpoints", LOCTEXT("PinBreakpointsMenuHeader", "Pin Breakpoints"));
			Section.AddMenuEntry(FlowGraphCommands.AddPinBreakpoint);
			Section.AddMenuEntry(FlowGraphCommands.RemovePinBreakpoint);
			Section.AddMenuEntry(FlowGraphCommands.EnablePinBreakpoint);
			Section.AddMenuEntry(FlowGraphCommands.DisablePinBreakpoint);
			Section.AddMenuEntry(FlowGraphCommands.TogglePinBreakpoint);
		}
	}
	else if (Context->Node)
	{
		{
			FToolMenuSection& Section = Menu->AddSection("FlowGraphSchemaNodeActions", LOCTEXT("NodeActionsMenuHeader", "Node Actions"));
			Section.AddMenuEntry(GenericCommands.Delete);
			Section.AddMenuEntry(GenericCommands.Cut);
			Section.AddMenuEntry(GenericCommands.Copy);
			Section.AddMenuEntry(GenericCommands.Duplicate);

			Section.AddMenuEntry(GraphCommands.BreakNodeLinks);

			if (CanUserAddInput())
			{
				Section.AddMenuEntry(FlowGraphCommands.AddInput);
			}
			if (CanUserAddOutput())
			{
				Section.AddMenuEntry(FlowGraphCommands.AddOutput);
			}
		}

		{
			FToolMenuSection& Section = Menu->AddSection("FlowGraphNodeBreakpoints", LOCTEXT("NodeBreakpointsMenuHeader", "Node Breakpoints"));
			Section.AddMenuEntry(GraphCommands.AddBreakpoint);
			Section.AddMenuEntry(GraphCommands.RemoveBreakpoint);
			Section.AddMenuEntry(GraphCommands.EnableBreakpoint);
			Section.AddMenuEntry(GraphCommands.DisableBreakpoint);
			Section.AddMenuEntry(GraphCommands.ToggleBreakpoint);
		}

		{
			FToolMenuSection& Section = Menu->AddSection("FlowGraphNodeExtras", LOCTEXT("NodeExtrasMenuHeader", "Node Extras"));
			if (CanFocusViewport())
			{
				Section.AddMenuEntry(FlowGraphCommands.FocusViewport);
			}
		}
	}
}

TSharedPtr<SGraphNode> UFlowGraphNode::CreateVisualWidget()
{
	return SNew(SFlowGraphNode, this);
}

FText UFlowGraphNode::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (FlowNode)
	{
		return FlowNode->GetTitle();
	}

	return Super::GetNodeTitle(TitleType);
}

FLinearColor UFlowGraphNode::GetNodeTitleColor() const
{
	if (FlowNode)
	{
		if (const FLinearColor* Color = UFlowEditorSettings::Get()->NodeTitleColors.Find(FlowNode->NodeStyle))
		{
			return *Color;
		}
	}

	return Super::GetNodeTitleColor();
}

FSlateIcon UFlowGraphNode::GetIconAndTint(FLinearColor& OutColor) const
{
	return FSlateIcon();
}

FText UFlowGraphNode::GetTooltipText() const
{
	FText Tooltip;
	if (FlowNode)
	{
		Tooltip = FlowNode->GetClass()->GetToolTipText();
	}
	if (Tooltip.IsEmpty())
	{
		Tooltip = GetNodeTitle(ENodeTitleType::ListView);
	}
	return Tooltip;
}

FString UFlowGraphNode::GetNodeDescription() const
{
	return FlowNode ? FlowNode->GetNodeDescription() : FString();
}

UFlowNode* UFlowGraphNode::GetInspectedNodeInstance() const
{
	return FlowNode ? FlowNode->GetInspectedInstance() : nullptr;
}

EFlowActivationState UFlowGraphNode::GetActivationState() const
{
	if (FlowNode)
	{
		if (UFlowNode* NodeInstance = FlowNode->GetInspectedInstance())
		{
			return NodeInstance->GetActivationState();
		}
	}

	return EFlowActivationState::NeverActivated;
}

FString UFlowGraphNode::GetStatusString() const
{
	if (FlowNode)
	{
		if (UFlowNode* NodeInstance = FlowNode->GetInspectedInstance())
		{
			return NodeInstance->GetStatusString();
		}
	}

	return FString();
}

bool UFlowGraphNode::IsContentPreloaded() const
{
	if (FlowNode)
	{
		if (UFlowNode* NodeInstance = FlowNode->GetInspectedInstance())
		{
			return NodeInstance->bPreloaded;
		}
	}

	return false;
}

UObject* UFlowGraphNode::GetAssetToOpen() const
{
	return FlowNode ? FlowNode->GetAssetToOpen() : nullptr;
}

bool UFlowGraphNode::CanFocusViewport() const
{
	return FlowNode ? (GEditor->bIsSimulatingInEditor && FlowNode->GetAssetToOpen()) : false;
}

void UFlowGraphNode::CreateInputPin(const FName& PinName)
{
	UEdGraphPin* NewPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, PinName);
	if (NewPin->PinName.IsNone())
	{
		NewPin->PinName = CreateUniquePinName(TEXT("In"));
		NewPin->PinFriendlyName = FText::FromString(TEXT("In"));
	}

	InputPins.Add(NewPin);
}

void UFlowGraphNode::CreateOutputPin(const FName PinName)
{
	UEdGraphPin* NewPin = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, PinName);
	if (NewPin->PinName.IsNone())
	{
		NewPin->PinName = CreateUniquePinName(TEXT("Out"));
		NewPin->PinFriendlyName = FText::FromString(TEXT("Out"));
	}

	OutputPins.Add(NewPin);
}

bool UFlowGraphNode::CanUserAddInput() const
{
	return FlowNode && FlowNode->CanUserAddInput() && InputPins.Num() < 256;
}

bool UFlowGraphNode::CanUserAddOutput() const
{
	return FlowNode && FlowNode->CanUserAddOutput() && OutputPins.Num() < 256;
}

bool UFlowGraphNode::CanUserRemoveInput(const UEdGraphPin* Pin) const
{
	// 2 is only reasonable number of default pins for user-modifiable nodes
	return FlowNode && FlowNode->CanUserAddInput() && InputPins.Num() > 2;
}

bool UFlowGraphNode::CanUserRemoveOutput(const UEdGraphPin* Pin) const
{
	// 2 is only reasonable number of default pins for user-modifiable nodes
	return FlowNode && FlowNode->CanUserAddOutput() && OutputPins.Num() > 2;
}

void UFlowGraphNode::AddUserInput()
{
	const FScopedTransaction Transaction(LOCTEXT("FlowEditorAddInput", "Add Node Input"));
	Modify();

	// save pin name to asset
	const FName NewPinName = FName(*FString::FromInt(InputPins.Num()));
	FlowNode->InputNames.AddUnique(NewPinName);

	CreateInputPin(NewPinName);

	UFlowAsset* FlowAsset = CastChecked<UFlowAssetGraph>(GetGraph())->GetFlowAsset();
	FlowAsset->CompileNodeConnections();
	FlowAsset->MarkPackageDirty();

	// Refresh the current graph, so the pins can be updated
	GetGraph()->NotifyGraphChanged();
}

void UFlowGraphNode::AddUserOutput()
{
	const FScopedTransaction Transaction(LOCTEXT("FlowEditorAddOutput", "Add Node Input"));
	Modify();

	// save pin name to asset
	const FName NewPinName = FName(*FString::FromInt(OutputPins.Num()));
	FlowNode->OutputNames.AddUnique(NewPinName);

	CreateOutputPin(NewPinName);

	UFlowAsset* FlowAsset = CastChecked<UFlowAssetGraph>(GetGraph())->GetFlowAsset();
	FlowAsset->CompileNodeConnections();
	FlowAsset->MarkPackageDirty();

	// Refresh the current graph, so the pins can be updated
	GetGraph()->NotifyGraphChanged();
}

void UFlowGraphNode::RemoveUserPin(UEdGraphPin* Pin)
{
	const FScopedTransaction Transaction(LOCTEXT("FlowEditorRemovePin", "Remoe Node Pin"));
	Modify();

	if (Pin->Direction == EEdGraphPinDirection::EGPD_Input)
	{
		if (InputPins.Contains(Pin))
		{
			InputBreakpoints.Remove(InputPins.Find(Pin));
			InputPins.Remove(Pin);

			FlowNode->RemoveUserInput();

			Pin->MarkPendingKill();
			Pins.Remove(Pin);
		}
	}
	else
	{
		if (OutputPins.Contains(Pin))
		{
			OutputBreakpoints.Remove(OutputPins.Find(Pin));
			OutputPins.Remove(Pin);

			FlowNode->RemoveUserOutput();

			Pin->MarkPendingKill();
			Pins.Remove(Pin);
		}
	}

	// Recreate pins with updated names
	ReconstructNode();

	UFlowAsset* FlowAsset = CastChecked<UFlowAssetGraph>(GetGraph())->GetFlowAsset();
	FlowAsset->CompileNodeConnections();
	FlowAsset->MarkPackageDirty();

	GetGraph()->NotifyGraphChanged();
}

void UFlowGraphNode::OnInputTriggered(const int32 Index)
{
	if (InputPins.IsValidIndex(Index) && InputBreakpoints.Contains(Index))
	{
		InputBreakpoints[Index].bBreakpointHit = true;
		TryPausingSession(true);
	}

	TryPausingSession(false);
}

void UFlowGraphNode::OnOutputTriggered(const int32 Index)
{
	if (OutputPins.IsValidIndex(Index) && OutputBreakpoints.Contains(Index))
	{
		OutputBreakpoints[Index].bBreakpointHit = true;
		TryPausingSession(true);
	}

	TryPausingSession(false);
}

void UFlowGraphNode::TryPausingSession(bool bPauseSession)
{
	// Node breakpoints waits on any pin triggered
	if (NodeBreakpoint.IsBreakpointEnabled())
	{
		NodeBreakpoint.bBreakpointHit = true;
		bPauseSession = true;
	}

	if (bPauseSession)
	{
		FEditorDelegates::ResumePIE.AddUObject(this, &UFlowGraphNode::OnResumePIE);
		FEditorDelegates::EndPIE.AddUObject(this, &UFlowGraphNode::OnEndPIE);

		FFlowDebugger::PausePlaySession();
	}
}

void UFlowGraphNode::OnResumePIE(const bool bIsSimulating)
{
	ResetBreakpoints();
}

void UFlowGraphNode::OnEndPIE(const bool bIsSimulating)
{
	ResetBreakpoints();
}

void UFlowGraphNode::ResetBreakpoints()
{
	FEditorDelegates::ResumePIE.RemoveAll(this);
	FEditorDelegates::EndPIE.RemoveAll(this);

	NodeBreakpoint.bBreakpointHit = false;
	for (TPair<int32, FFlowBreakpoint>& PinBreakpoint : InputBreakpoints)
	{
		PinBreakpoint.Value.bBreakpointHit = false;
	}
	for (TPair<int32, FFlowBreakpoint>& PinBreakpoint : OutputBreakpoints)
	{
		PinBreakpoint.Value.bBreakpointHit = false;
	}
}

#undef LOCTEXT_NAMESPACE