#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"
#include "Engine/DataAsset.h"
#include "CourierRequestAsset.generated.h"

/**
 * A saved request. Made from the Courier window (Save as Asset) or the Content Browser; every
 * asset shows up in Blueprint graphs as a "Courier: <name>" node with one pin per {{variable}}.
 */
UCLASS(BlueprintType)
class COURIER_API UCourierRequestAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier", meta = (ShowOnlyInnerProperties))
	FCourierRequestSpec Request;

	/** Values used for variables the caller does not pass (e.g. a base URL). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	TArray<FCourierKeyValue> DefaultVariables;

	/** Variables the request uses, in order. */
	UFUNCTION(BlueprintPure, Category = "Courier")
	TArray<FString> GetVariableNames() const;
};
