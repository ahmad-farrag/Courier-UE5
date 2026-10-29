#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"
#include "Interfaces/IHttpRequest.h"
#include "UObject/Object.h"
#include "CourierEventSource.generated.h"

struct FCourierPreparedRequest;

/** Incremental text/event-stream parser (WHATWG rules). Feed it bytes in any chunking. */
struct COURIER_API FCourierSseParser
{
	void Feed(const uint8* Data, int64 Length, TArray<FCourierSseEvent>& OutEvents);
	void Reset();

	/** The last "id:" seen, kept across events as the spec says. */
	FString LastEventId;

private:
	void ProcessLine(const FString& Line, TArray<FCourierSseEvent>& OutEvents);

	TArray<uint8> LineBytes;
	FString EventName;
	FString DataBuffer;
	int32 Retry = -1;
	bool bHasData = false;
	bool bLastWasCR = false;
};

/** A live Server-Sent Events stream in plain C++. Callbacks run on the game thread. */
class COURIER_API FCourierSseConnection : public TSharedFromThis<FCourierSseConnection, ESPMode::ThreadSafe>
{
public:
	TFunction<void(int32 StatusCode)> OnOpen;
	TFunction<void(const FCourierSseEvent& Event)> OnEvent;
	/** Error is empty after a clean end of stream or Close(). */
	TFunction<void(const FString& Error, int32 StatusCode)> OnClosed;

	~FCourierSseConnection();

	/** Starts the stream (verb, headers and body from the prepared request; Accept: text/event-stream is added). */
	bool Open(const FCourierPreparedRequest& Prepared);
	void Close();
	bool IsOpen() const { return bOpen; }

	/** Called on the game thread with events parsed on the HTTP thread. */
	void Dispatch(const TArray<FCourierSseEvent>& Events);

private:
	FHttpRequestPtr Request;
	bool bOpen = false;
	bool bClosing = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCourierSseOpenDelegate, int32, StatusCode);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCourierSseEventDelegate, const FCourierSseEvent&, Event);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCourierSseClosedDelegate, const FString&, Error, int32, StatusCode);

/**
 * A Server-Sent Events stream for Blueprints. Keep a reference to it (a variable), bind the events,
 * then call Open.
 */
UCLASS(BlueprintType)
class COURIER_API UCourierEventSource : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Courier|Server-Sent Events", meta = (AutoCreateRefTerm = "Headers"))
	static UCourierEventSource* CreateCourierEventSource(const FString& Url, const TMap<FString, FString>& Headers);

	/** From a request spec (its URL, query, headers and auth); Variables fill its placeholders. */
	UFUNCTION(BlueprintCallable, Category = "Courier|Server-Sent Events", meta = (AutoCreateRefTerm = "Variables"))
	static UCourierEventSource* CreateCourierEventSourceFromRequest(const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables);

	UFUNCTION(BlueprintCallable, Category = "Courier|Server-Sent Events")
	bool Open();

	UFUNCTION(BlueprintCallable, Category = "Courier|Server-Sent Events")
	void Close();

	UFUNCTION(BlueprintPure, Category = "Courier|Server-Sent Events")
	bool IsOpen() const;

	UPROPERTY(BlueprintAssignable, Category = "Courier|Server-Sent Events")
	FCourierSseOpenDelegate OnOpen;

	UPROPERTY(BlueprintAssignable, Category = "Courier|Server-Sent Events")
	FCourierSseEventDelegate OnEvent;

	UPROPERTY(BlueprintAssignable, Category = "Courier|Server-Sent Events")
	FCourierSseClosedDelegate OnClosed;

	virtual void BeginDestroy() override;

private:
	FCourierRequestSpec Spec;
	TMap<FString, FString> Variables;
	TSharedPtr<FCourierSseConnection, ESPMode::ThreadSafe> Connection;
};
