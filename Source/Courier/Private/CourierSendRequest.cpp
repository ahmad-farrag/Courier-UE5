#include "CourierSendRequest.h"

#include "CourierHttp.h"
#include "CourierRequestAsset.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

UCourierSendRequest* UCourierSendRequest::Make(UObject* WorldContextObject, const FCourierRequestSpec& InSpec, const TMap<FString, FString>& InVariables)
{
	UCourierSendRequest* Action = NewObject<UCourierSendRequest>();
	Action->Spec = InSpec;
	Action->Variables = InVariables;

	// A game instance keeps the action alive while the request flies. Editor utilities and
	// editor-world callers have none, so the action roots itself until it finishes.
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (World && World->GetGameInstance())
	{
		Action->RegisterWithGameInstance(World->GetGameInstance());
	}
	else
	{
		Action->AddToRoot();
		Action->bRooted = true;
	}
	return Action;
}

FCourierRequestSpec UCourierSendRequest::MakeSimpleSpec(ECourierVerb Verb, const FString& Url, const TMap<FString, FString>& Headers)
{
	FCourierRequestSpec Out;
	Out.Name = FCourierHttp::VerbToString(Verb) + TEXT(" ") + Url;
	Out.Verb = Verb;
	Out.Url = Url;
	for (const TPair<FString, FString>& Header : Headers)
	{
		Out.Headers.Emplace(Header.Key, Header.Value);
	}
	return Out;
}

UCourierSendRequest* UCourierSendRequest::SendCourierRequest(UObject* WorldContextObject, const FCourierRequestSpec& Request, const TMap<FString, FString>& InVariables)
{
	return Make(WorldContextObject, Request, InVariables);
}

UCourierSendRequest* UCourierSendRequest::CourierGet(UObject* WorldContextObject, const FString& Url, const TMap<FString, FString>& Headers)
{
	return Make(WorldContextObject, MakeSimpleSpec(ECourierVerb::Get, Url, Headers), {});
}

UCourierSendRequest* UCourierSendRequest::CourierPostJson(UObject* WorldContextObject, const FString& Url, const FString& Json, const TMap<FString, FString>& Headers)
{
	FCourierRequestSpec Out = MakeSimpleSpec(ECourierVerb::Post, Url, Headers);
	Out.BodyType = ECourierBodyType::Json;
	Out.Body = Json;
	return Make(WorldContextObject, Out, {});
}

UCourierSendRequest* UCourierSendRequest::CourierPutJson(UObject* WorldContextObject, const FString& Url, const FString& Json, const TMap<FString, FString>& Headers)
{
	FCourierRequestSpec Out = MakeSimpleSpec(ECourierVerb::Put, Url, Headers);
	Out.BodyType = ECourierBodyType::Json;
	Out.Body = Json;
	return Make(WorldContextObject, Out, {});
}

UCourierSendRequest* UCourierSendRequest::CourierPatchJson(UObject* WorldContextObject, const FString& Url, const FString& Json, const TMap<FString, FString>& Headers)
{
	FCourierRequestSpec Out = MakeSimpleSpec(ECourierVerb::Patch, Url, Headers);
	Out.BodyType = ECourierBodyType::Json;
	Out.Body = Json;
	return Make(WorldContextObject, Out, {});
}

UCourierSendRequest* UCourierSendRequest::CourierDelete(UObject* WorldContextObject, const FString& Url, const TMap<FString, FString>& Headers)
{
	return Make(WorldContextObject, MakeSimpleSpec(ECourierVerb::Delete, Url, Headers), {});
}

UCourierSendRequest* UCourierSendRequest::CourierPostForm(UObject* WorldContextObject, const FString& Url, const TMap<FString, FString>& Fields, const TMap<FString, FString>& Headers)
{
	FCourierRequestSpec Out = MakeSimpleSpec(ECourierVerb::Post, Url, Headers);
	Out.BodyType = ECourierBodyType::FormUrlEncoded;
	for (const TPair<FString, FString>& Field : Fields)
	{
		FCourierFormField& Row = Out.FormFields.AddDefaulted_GetRef();
		Row.Key = Field.Key;
		Row.Value = Field.Value;
	}
	return Make(WorldContextObject, Out, {});
}

UCourierSendRequest* UCourierSendRequest::CourierGraphQL(UObject* WorldContextObject, const FString& Url, const FString& Query, const FString& VariablesJson, const TMap<FString, FString>& Headers)
{
	FCourierRequestSpec Out = MakeSimpleSpec(ECourierVerb::Post, Url, Headers);
	Out.BodyType = ECourierBodyType::GraphQL;
	Out.GraphQLQuery = Query;
	Out.GraphQLVariables = VariablesJson;
	return Make(WorldContextObject, Out, {});
}

UCourierSendRequest* UCourierSendRequest::SendRequestAsset(UObject* WorldContextObject, UCourierRequestAsset* RequestAsset, const TMap<FString, FString>& InVariables)
{
	if (!RequestAsset)
	{
		FCourierRequestSpec Empty;
		Empty.Name = TEXT("(no request asset)");
		Empty.Url.Reset();
		return Make(WorldContextObject, Empty, {});
	}
	TMap<FString, FString> Merged = FCourierHttp::ToVariableMap(RequestAsset->DefaultVariables);
	Merged.Append(InVariables);
	return Make(WorldContextObject, RequestAsset->Request, Merged);
}

void UCourierSendRequest::Activate()
{
	if (bInFlight || bFinished)
	{
		return;
	}
	bInFlight = true;
	TWeakObjectPtr<UCourierSendRequest> WeakThis(this);
	HttpRequest = FCourierHttp::Send(Spec, Variables, [WeakThis](const FCourierResponse& Response)
	{
		if (UCourierSendRequest* Self = WeakThis.Get())
		{
			Self->Finish(Response);
		}
	});
}

void UCourierSendRequest::Cancel()
{
	if (!bInFlight)
	{
		return;
	}
	FCourierResponse Cancelled;
	Cancelled.Error = TEXT("Cancelled");
	Cancelled.Url = HttpRequest.IsValid() ? HttpRequest->GetURL() : Spec.Url;
	Cancelled.Verb = FCourierHttp::VerbToString(Spec.Verb, Spec.CustomVerb);
	if (HttpRequest.IsValid())
	{
		HttpRequest->OnProcessRequestComplete().Unbind();
		HttpRequest->CancelRequest();
	}
	Finish(Cancelled);
}

void UCourierSendRequest::Finish(const FCourierResponse& Response)
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	bInFlight = false;
	HttpRequest.Reset();

	if (Response.bSuccess)
	{
		OnSuccess.Broadcast(Response);
	}
	else
	{
		OnFailure.Broadcast(Response);
	}
	Callback.ExecuteIfBound(Response);

	if (bRooted)
	{
		RemoveFromRoot();
		bRooted = false;
	}
	SetReadyToDestroy();
}
