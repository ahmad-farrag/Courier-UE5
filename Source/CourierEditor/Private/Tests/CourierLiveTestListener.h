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

	TArray<FCourierResponse> Successes;
	TArray<FCourierResponse> Failures;
	TArray<FCourierResponse> Callbacks;
	TArray<FCourierSseEvent> SseEvents;
	int32 SseOpenStatus = 0;
	FString SseClosedError;
	bool bSseClosed = false;
};
