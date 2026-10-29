#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"
#include "Interfaces/IHttpRequest.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "CourierSendRequest.generated.h"

class UCourierRequestAsset;

/**
 * The async Blueprint nodes: Send Courier Request, Courier GET / POST JSON / PUT JSON / PATCH JSON /
 * DELETE / POST Form and Courier GraphQL. Each fires On Success (2xx) or On Failure (anything else,
 * including no response) exactly once, with the full response.
 */
UCLASS(meta = (ExposedAsyncProxy = "Task"))
class COURIER_API UCourierSendRequest : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** A 2xx response. */
	UPROPERTY(BlueprintAssignable)
	FCourierResponseMulticastDelegate OnSuccess;

	/** Any other status, or no response at all (see Response.Error). */
	UPROPERTY(BlueprintAssignable)
	FCourierResponseMulticastDelegate OnFailure;

	/** Sends a full request spec. Variables fill its {{placeholders}}. */
	UFUNCTION(BlueprintCallable, Category = "Courier|HTTP", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Variables", DisplayName = "Send Courier Request"))
	static UCourierSendRequest* SendCourierRequest(UObject* WorldContextObject, const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables);

	UFUNCTION(BlueprintCallable, Category = "Courier|HTTP", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Headers", DisplayName = "Courier GET"))
	static UCourierSendRequest* CourierGet(UObject* WorldContextObject, const FString& Url, const TMap<FString, FString>& Headers);

	UFUNCTION(BlueprintCallable, Category = "Courier|HTTP", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Headers", DisplayName = "Courier POST JSON"))
	static UCourierSendRequest* CourierPostJson(UObject* WorldContextObject, const FString& Url, const FString& Json, const TMap<FString, FString>& Headers);

	UFUNCTION(BlueprintCallable, Category = "Courier|HTTP", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Headers", DisplayName = "Courier PUT JSON"))
	static UCourierSendRequest* CourierPutJson(UObject* WorldContextObject, const FString& Url, const FString& Json, const TMap<FString, FString>& Headers);

	UFUNCTION(BlueprintCallable, Category = "Courier|HTTP", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Headers", DisplayName = "Courier PATCH JSON"))
	static UCourierSendRequest* CourierPatchJson(UObject* WorldContextObject, const FString& Url, const FString& Json, const TMap<FString, FString>& Headers);

	UFUNCTION(BlueprintCallable, Category = "Courier|HTTP", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Headers", DisplayName = "Courier DELETE"))
	static UCourierSendRequest* CourierDelete(UObject* WorldContextObject, const FString& Url, const TMap<FString, FString>& Headers);

	/** application/x-www-form-urlencoded POST. */
	UFUNCTION(BlueprintCallable, Category = "Courier|HTTP", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Fields,Headers", DisplayName = "Courier POST Form"))
	static UCourierSendRequest* CourierPostForm(UObject* WorldContextObject, const FString& Url, const TMap<FString, FString>& Fields, const TMap<FString, FString>& Headers);

	/** POSTs {"query": Query, "variables": VariablesJson}. VariablesJson may be empty. */
	UFUNCTION(BlueprintCallable, Category = "Courier|GraphQL", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Headers", DisplayName = "Courier GraphQL"))
	static UCourierSendRequest* CourierGraphQL(UObject* WorldContextObject, const FString& Url, const FString& Query, const FString& VariablesJson, const TMap<FString, FString>& Headers);

	/**
	 * The factory behind the "Courier: <asset>" node (UCourierNodeLibrary forwards here); the asset's
	 * DefaultVariables fill what Variables leaves out.
	 */
	static UCourierSendRequest* SendRequestAsset(UObject* WorldContextObject, UCourierRequestAsset* RequestAsset, const TMap<FString, FString>& Variables);

	/** Starts a task made by Create Courier Request (the async nodes start themselves). */
	UFUNCTION(BlueprintCallable, Category = "Courier|HTTP")
	void Send() { Activate(); }

	/** Stops the request; On Failure fires with Error "Cancelled". */
	UFUNCTION(BlueprintCallable, Category = "Courier|HTTP")
	void Cancel();

	UFUNCTION(BlueprintPure, Category = "Courier|HTTP")
	bool IsInFlight() const { return bInFlight; }

	/** Also called when the request finishes (UCourierLibrary's ...WithCallback functions). */
	void SetCallback(const FCourierResponseDelegate& InCallback) { Callback = InCallback; }

	virtual void Activate() override;

private:
	static UCourierSendRequest* Make(UObject* WorldContextObject, const FCourierRequestSpec& Spec, const TMap<FString, FString>& Variables);
	static FCourierRequestSpec MakeSimpleSpec(ECourierVerb Verb, const FString& Url, const TMap<FString, FString>& Headers);
	void Finish(const FCourierResponse& Response);

	FCourierRequestSpec Spec;
	TMap<FString, FString> Variables;
	FHttpRequestPtr HttpRequest;
	FCourierResponseDelegate Callback;
	bool bInFlight = false;
	bool bFinished = false;
	bool bRooted = false;
};
