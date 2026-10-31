#include "K2Node_CourierRequest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "CourierHttp.h"
#include "CourierLibrary.h"
#include "CourierRequestAsset.h"
#include "CourierSendRequest.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "KismetCompiler.h"

#define LOCTEXT_NAMESPACE "Courier"

namespace CourierNode
{
	const FName RequestPin(TEXT("Request"));
	const FName VariablesPin(TEXT("Variables"));
	const TCHAR* VariablePrefix = TEXT("Var_");

	void SetupLoaded(UEdGraphNode* NewNode, bool bIsTemplateNode, TWeakObjectPtr<UCourierRequestAsset> Asset)
	{
		if (UK2Node_CourierRequest* Node = Cast<UK2Node_CourierRequest>(NewNode))
		{
			Node->RequestAsset = Asset.Get();
		}
	}

	void SetupUnloaded(UEdGraphNode* NewNode, bool bIsTemplateNode, FAssetData AssetData)
	{
		UK2Node_CourierRequest* Node = Cast<UK2Node_CourierRequest>(NewNode);
		if (Node && !bIsTemplateNode)
		{
			Node->RequestAsset = Cast<UCourierRequestAsset>(AssetData.GetAsset());
		}
	}

	FText MenuName(const FName& AssetName)
	{
		return FText::Format(LOCTEXT("NodeMenuName", "Courier: {0}"), FText::FromName(AssetName));
	}
}

UK2Node_CourierRequest::UK2Node_CourierRequest()
{
	ProxyFactoryFunctionName = GET_FUNCTION_NAME_CHECKED(UCourierLibrary, SendCourierRequestAsset);
	ProxyFactoryClass = UCourierLibrary::StaticClass();
	ProxyClass = UCourierSendRequest::StaticClass();
	ProxyActivateFunctionName = GET_FUNCTION_NAME_CHECKED(UBlueprintAsyncActionBase, Activate);
}

FName UK2Node_CourierRequest::VariablePinName(const FString& Variable)
{
	return FName(*(FString(CourierNode::VariablePrefix) + Variable));
}

bool UK2Node_CourierRequest::IsVariablePin(const UEdGraphPin* Pin)
{
	return Pin && Pin->Direction == EGPD_Input && Pin->PinName.ToString().StartsWith(CourierNode::VariablePrefix);
}

FString UK2Node_CourierRequest::VariableFromPin(const UEdGraphPin* Pin)
{
	return Pin->PinName.ToString().RightChop(FCString::Strlen(CourierNode::VariablePrefix));
}

void UK2Node_CourierRequest::AllocateDefaultPins()
{
	Super::AllocateDefaultPins();

	if (UEdGraphPin* Request = FindPin(CourierNode::RequestPin))
	{
		Request->DefaultObject = RequestAsset;
		Request->PinToolTip = LOCTEXT("RequestPinTip", "The Courier Request asset to send. Pick one to get a pin per {{variable}}.").ToString();
	}
	if (UEdGraphPin* Variables = FindPin(CourierNode::VariablesPin))
	{
		// Extra values for variables without a pin, or for a Request wired at run time.
		Variables->bAdvancedView = true;
		AdvancedPinDisplay = ENodeAdvancedPins::Hidden;
	}
	CreateVariablePins();
}

void UK2Node_CourierRequest::CreateVariablePins()
{
	const UEdGraphPin* Request = FindPin(CourierNode::RequestPin);
	if (!RequestAsset || (Request && Request->LinkedTo.Num() > 0))
	{
		return;
	}
	const TMap<FString, FString> Defaults = FCourierHttp::ToVariableMap(RequestAsset->DefaultVariables);
	for (const FString& Variable : RequestAsset->GetVariableNames())
	{
		UEdGraphPin* Pin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, VariablePinName(Variable));
		Pin->PinFriendlyName = FText::FromString(Variable);
		const FString* Default = Defaults.Find(Variable);
		Pin->PinToolTip = Default
			? FString::Printf(TEXT("{{%s}}. Empty uses the asset's default: %s"), *Variable, **Default)
			: FString::Printf(TEXT("{{%s}}"), *Variable);
	}
}

void UK2Node_CourierRequest::RefreshVariablePins()
{
	struct FSaved
	{
		FString DefaultValue;
		TArray<UEdGraphPin*> LinkedTo;
	};
	TMap<FName, FSaved> Saved;
	TArray<UEdGraphPin*> OldPins;
	for (UEdGraphPin* Pin : Pins)
	{
		if (IsVariablePin(Pin))
		{
			Saved.Add(Pin->PinName, FSaved{ Pin->DefaultValue, Pin->LinkedTo });
			OldPins.Add(Pin);
		}
	}
	for (UEdGraphPin* Pin : OldPins)
	{
		RemovePin(Pin);
	}

	CreateVariablePins();

	for (UEdGraphPin* Pin : Pins)
	{
		if (const FSaved* Old = IsVariablePin(Pin) ? Saved.Find(Pin->PinName) : nullptr)
		{
			Pin->DefaultValue = Old->DefaultValue;
			for (UEdGraphPin* Linked : Old->LinkedTo)
			{
				Pin->MakeLinkTo(Linked);
			}
		}
	}

	if (UEdGraph* Graph = GetGraph())
	{
		Graph->NotifyNodeChanged(this);
	}
	if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(this))
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
}

void UK2Node_CourierRequest::ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins)
{
	// The old Request pin knows which asset was picked (or that it is wired).
	for (const UEdGraphPin* Old : OldPins)
	{
		if (Old->PinName == CourierNode::RequestPin)
		{
			RequestAsset = Old->LinkedTo.Num() == 0 ? Cast<UCourierRequestAsset>(Old->DefaultObject) : nullptr;
			break;
		}
	}
	Super::ReallocatePinsDuringReconstruction(OldPins);
}

void UK2Node_CourierRequest::PinDefaultValueChanged(UEdGraphPin* Pin)
{
	Super::PinDefaultValueChanged(Pin);
	if (Pin && Pin->PinName == CourierNode::RequestPin)
	{
		RequestAsset = Cast<UCourierRequestAsset>(Pin->DefaultObject);
		RefreshVariablePins();
	}
}

void UK2Node_CourierRequest::PinConnectionListChanged(UEdGraphPin* Pin)
{
	Super::PinConnectionListChanged(Pin);
	if (Pin && Pin->PinName == CourierNode::RequestPin)
	{
		RequestAsset = Pin->LinkedTo.Num() == 0 ? Cast<UCourierRequestAsset>(Pin->DefaultObject) : nullptr;
		RefreshVariablePins();
	}
}

FText UK2Node_CourierRequest::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (!RequestAsset)
	{
		return LOCTEXT("NodeTitleEmpty", "Courier Request");
	}
	const FText Name = FText::Format(LOCTEXT("NodeTitle", "Courier: {0}"), FText::FromString(RequestAsset->GetName()));
	if (TitleType == ENodeTitleType::FullTitle)
	{
		const FCourierRequestSpec& Spec = RequestAsset->Request;
		return FText::Format(LOCTEXT("NodeTitleFull", "{0}\n{1} {2}"), Name, FText::FromString(FCourierHttp::VerbToString(Spec.Verb, Spec.CustomVerb)), FText::FromString(Spec.Url));
	}
	return Name;
}

FText UK2Node_CourierRequest::GetTooltipText() const
{
	return LOCTEXT("NodeTooltip", "Sends a Courier Request asset. Fills its {{variables}} from the pins (empty pins use the asset's defaults), then fires On Success (2xx) or On Failure with the response.");
}

FText UK2Node_CourierRequest::GetMenuCategory() const
{
	return LOCTEXT("NodeCategory", "Courier|Requests");
}

void UK2Node_CourierRequest::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	Super::ValidateNodeDuringCompilation(MessageLog);
	const UEdGraphPin* Request = FindPin(CourierNode::RequestPin);
	if (Request && Request->LinkedTo.Num() == 0 && !Request->DefaultObject)
	{
		MessageLog.Error(*LOCTEXT("NoRequestAsset", "@@ has no Courier Request asset.").ToString(), this);
	}
}

void UK2Node_CourierRequest::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();
	UEdGraphPin* VariablesPin = FindPin(CourierNode::VariablesPin);

	TArray<UEdGraphPin*> VariablePins;
	for (UEdGraphPin* Pin : Pins)
	{
		if (IsVariablePin(Pin))
		{
			VariablePins.Add(Pin);
		}
	}

	// Fold the variable pins into the factory's Variables map: a chain of AddCourierVariable calls,
	// starting from whatever the Variables pin carries. A pin left empty and unwired is skipped, so
	// the asset's default for it still applies.
	if (VariablesPin && VariablePins.Num() > 0)
	{
		UEdGraphPin* LastMap = nullptr;
		bool bMovedBaseMap = false;
		for (UEdGraphPin* Pin : VariablePins)
		{
			if (Pin->LinkedTo.Num() == 0 && Pin->DefaultValue.IsEmpty())
			{
				continue;
			}
			UK2Node_CallFunction* Add = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
			Add->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UCourierLibrary, AddCourierVariable), UCourierLibrary::StaticClass());
			Add->AllocateDefaultPins();

			UEdGraphPin* InMap = Add->FindPinChecked(TEXT("Variables"));
			if (!LastMap)
			{
				CompilerContext.MovePinLinksToIntermediate(*VariablesPin, *InMap);
				bMovedBaseMap = true;
			}
			else
			{
				Schema->TryCreateConnection(LastMap, InMap);
			}
			Add->FindPinChecked(TEXT("Name"))->DefaultValue = VariableFromPin(Pin);
			CompilerContext.MovePinLinksToIntermediate(*Pin, *Add->FindPinChecked(TEXT("Value")));
			LastMap = Add->GetReturnValuePin();
		}
		if (LastMap && bMovedBaseMap)
		{
			Schema->TryCreateConnection(LastMap, VariablesPin);
		}
	}

	// The base expansion maps every input data pin onto a factory parameter; these have none.
	for (UEdGraphPin* Pin : VariablePins)
	{
		Pin->BreakAllPinLinks();
		RemovePin(Pin);
	}

	Super::ExpandNode(CompilerContext, SourceGraph);
}

void UK2Node_CourierRequest::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	UClass* ActionKey = GetClass();
	const UObject* Filter = ActionRegistrar.GetActionKeyFilter();

	auto RegisterAsset = [&ActionRegistrar, ActionKey](const FAssetData& AssetData)
	{
		UBlueprintNodeSpawner* Spawner = UBlueprintNodeSpawner::Create(ActionKey);
		if (UCourierRequestAsset* Loaded = Cast<UCourierRequestAsset>(AssetData.FastGetAsset(false)))
		{
			Spawner->CustomizeNodeDelegate = UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateStatic(CourierNode::SetupLoaded, MakeWeakObjectPtr(Loaded));
		}
		else
		{
			Spawner->CustomizeNodeDelegate = UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateStatic(CourierNode::SetupUnloaded, AssetData);
		}
		Spawner->DefaultMenuSignature.MenuName = CourierNode::MenuName(AssetData.AssetName);
		Spawner->DefaultMenuSignature.Tooltip = FText::FromString(AssetData.GetObjectPathString());
		Spawner->DefaultMenuSignature.Keywords = LOCTEXT("NodeKeywords", "courier http rest api request web");
		ActionRegistrar.AddBlueprintAction(AssetData, Spawner);
	};

	if (const UCourierRequestAsset* Asset = Cast<UCourierRequestAsset>(Filter))
	{
		// A single asset changed: refresh just its entry.
		RegisterAsset(FAssetData(Asset));
		return;
	}
	if (Filter == nullptr || Filter == ActionKey)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByClass(UCourierRequestAsset::StaticClass()->GetClassPathName(), Assets, true);
		for (const FAssetData& AssetData : Assets)
		{
			RegisterAsset(AssetData);
		}
	}

	// The plain node, to pick or wire an asset by hand.
	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UBlueprintNodeSpawner* Spawner = UBlueprintNodeSpawner::Create(ActionKey);
		ActionRegistrar.AddBlueprintAction(ActionKey, Spawner);
	}
}

#undef LOCTEXT_NAMESPACE
