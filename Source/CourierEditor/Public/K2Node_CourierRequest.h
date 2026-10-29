#pragma once

#include "CoreMinimal.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_CourierRequest.generated.h"

class UCourierRequestAsset;

/**
 * "Courier: <asset>": sends a saved request asset. Every {{variable}} the request uses becomes a
 * string input pin (empty pins fall back to the asset's default variables). Fires On Success or
 * On Failure with the response, like the other Courier nodes.
 */
UCLASS()
class COURIEREDITOR_API UK2Node_CourierRequest : public UK2Node_BaseAsyncTask
{
	GENERATED_BODY()

public:
	UK2Node_CourierRequest();

	// UEdGraphNode
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual void PinDefaultValueChanged(UEdGraphPin* Pin) override;
	virtual void PinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const override;

	// UK2Node
	virtual void ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;

	/** The request the variable pins were made from. */
	UPROPERTY()
	TObjectPtr<UCourierRequestAsset> RequestAsset;

private:
	static FName VariablePinName(const FString& Variable);
	static bool IsVariablePin(const UEdGraphPin* Pin);
	static FString VariableFromPin(const UEdGraphPin* Pin);

	/** Creates one string pin per variable of RequestAsset (none when the Request pin is wired). */
	void CreateVariablePins();
	/** Replaces the variable pins in place, keeping links and values of pins whose name stays. */
	void RefreshVariablePins();
};
