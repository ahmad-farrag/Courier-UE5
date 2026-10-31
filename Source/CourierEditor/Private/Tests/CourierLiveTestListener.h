#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"
#include "UObject/Object.h"
#include "CourierLiveTestListener.generated.h"

/** Receives Courier's dynamic delegates through UFUNCTIONs, the same dispatch Blueprint events use. */
UCLASS(Transient)
class UCourierLiveTestListener : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void HandleSuccess(const FCourierResponse& Response) { Successes.Add(Response); }

	UFUNCTION()
	void HandleFailure(const FCourierResponse& Response) { Failures.Add(Response); }

	UFUNCTION()
	void HandleCallback(const FCourierResponse& Response) { Callbacks.Add(Response); }

	UFUNCTION()
	void HandleSseOpen(int32 StatusCode) { SseOpenStatus = StatusCode; }

	UFUNCTION()
	void HandleSseEvent(const FCourierSseEvent& Event) { SseEvents.Add(Event); }

	UFUNCTION()
	void HandleSseClosed(const FString& Error, int32 StatusCode) { SseClosedError = Error; bSseClosed = true; }

	UFUNCTION()
	void HandleToken(const FCourierOAuth2Token& Token) { Tokens.Add(Token); }

	UFUNCTION()
	void HandleSioConnected(const FString& SocketId) { SioSocketId = SocketId; }

	UFUNCTION()
	void HandleSioError(const FString& Message) { SioErrors.Add(Message); }

	UFUNCTION()
	void HandleSioDisconnected(const FString& Reason) { SioDisconnects.Add(Reason); }

	UFUNCTION()
	void HandleSioEvent(const FString& EventName, const FString& ArgsJson) { SioEvents.Add(EventName + TEXT(" ") + ArgsJson); }

	UFUNCTION()
	void HandleSioAck(int32 AckId, const FString& ArgsJson) { SioAcks.Add(FString::Printf(TEXT("%d %s"), AckId, *ArgsJson)); }

	TArray<FCourierOAuth2Token> Tokens;
	FString SioSocketId;
	TArray<FString> SioErrors;
	TArray<FString> SioDisconnects;
	TArray<FString> SioEvents;
	TArray<FString> SioAcks;

	TArray<FCourierResponse> Successes;
	TArray<FCourierResponse> Failures;
	TArray<FCourierResponse> Callbacks;
	TArray<FCourierSseEvent> SseEvents;
	int32 SseOpenStatus = 0;
	FString SseClosedError;
	bool bSseClosed = false;
};
