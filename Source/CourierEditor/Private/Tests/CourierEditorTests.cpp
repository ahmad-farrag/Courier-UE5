#include "CourierRequestAsset.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CourierRequest.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierNodeCompileTest, "Courier.Editor.RequestNode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCourierNodeCompileTest::RunTest(const FString& Parameters)
{
	UCourierRequestAsset* Asset = NewObject<UCourierRequestAsset>(GetTransientPackage(), TEXT("CR_CourierNodeTest"), RF_Transient);
	Asset->Request.Verb = ECourierVerb::Get;
	Asset->Request.Url = TEXT("https://{{host}}/users/{{id}}");
	Asset->DefaultVariables.Emplace(TEXT("host"), TEXT("example.com"));

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), GetTransientPackage(), TEXT("BP_CourierNodeTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("event graph"), Graph))
	{
		return false;
	}

	// A custom event drives the node; an unwired node would be pruned and never expanded.
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph);
	Event->CustomFunctionName = TEXT("CourierTest");
	Graph->AddNode(Event, false, false);
	Event->CreateNewGuid();
	Event->AllocateDefaultPins();

	UK2Node_CourierRequest* Node = NewObject<UK2Node_CourierRequest>(Graph);
	Node->RequestAsset = Asset;
	Graph->AddNode(Node, false, false);
	Node->CreateNewGuid();
	Node->PostPlacedNewNode();
	Node->AllocateDefaultPins();

	UEdGraphPin* HostPin = Node->FindPin(TEXT("Var_host"));
	UEdGraphPin* IdPin = Node->FindPin(TEXT("Var_id"));
	TestNotNull(TEXT("host pin"), HostPin);
	if (!TestNotNull(TEXT("id pin"), IdPin))
	{
		return false;
	}
	TestNotNull(TEXT("On Success pin"), Node->FindPin(TEXT("OnSuccess")));
	TestNotNull(TEXT("On Failure pin"), Node->FindPin(TEXT("OnFailure")));
	TestNotNull(TEXT("Response pin"), Node->FindPin(TEXT("Response")));
	IdPin->DefaultValue = TEXT("42");

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	TestTrue(TEXT("exec wired"), Schema->TryCreateConnection(Event->FindPinChecked(UEdGraphSchema_K2::PN_Then), Node->FindPinChecked(UEdGraphSchema_K2::PN_Execute)));

	FCompilerResultsLog Results;
	Results.bSilentMode = true;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
	for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
	{
		if (Message->GetSeverity() == EMessageSeverity::Error)
		{
			AddError(Message->ToText().ToString());
		}
	}
	TestEqual(TEXT("compile errors"), Results.NumErrors, 0);
	TestTrue(TEXT("blueprint up to date"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

	// A new variable in the asset adds a pin on reconstruction and keeps the old pin's value.
	Asset->Request.Headers.Emplace(TEXT("Authorization"), TEXT("Bearer {{token}}"));
	Node->ReconstructNode();
	TestNotNull(TEXT("new token pin"), Node->FindPin(TEXT("Var_token")));
	const UEdGraphPin* IdAfter = Node->FindPin(TEXT("Var_id"));
	TestTrue(TEXT("id value kept"), IdAfter && IdAfter->DefaultValue == TEXT("42"));
	TestTrue(TEXT("still wired"), Node->FindPinChecked(UEdGraphSchema_K2::PN_Execute)->LinkedTo.Num() == 1);

	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
	TestEqual(TEXT("compile errors after reconstruct"), Results.NumErrors, 0);
	return true;
}

#endif
