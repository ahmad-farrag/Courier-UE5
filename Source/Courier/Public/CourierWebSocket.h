#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"
#include "UObject/Object.h"
#include "CourierWebSocket.generated.h"

class IWebSocket;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FCourierWebSocketConnectedDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCourierWebSocketErrorDelegate, const FString&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCourierWebSocketClosedDelegate, int32, StatusCode, const FString&, Reason, bool, bWasClean);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCourierWebSocketMessageDelegate, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCourierWebSocketBinaryDelegate, const TArray<uint8>&, Data);

/**
 * A WebSocket for Blueprints (ws:// and wss://). Keep a reference to it (a variable), bind the
 * events, then call Connect. Events fire on the game thread.
 */
UCLASS(BlueprintType)
class COURIER_API UCourierWebSocket : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Courier|WebSocket", meta = (AutoCreateRefTerm = "Protocols,Headers"))
	static UCourierWebSocket* CreateCourierWebSocket(const FString& Url, const TArray<FString>& Protocols, const TMap<FString, FString>& Headers);

	/** From a request spec (URL, query, headers, auth, sub-protocols); Variables fill its placeholders. */
	UFUNCTION(BlueprintCallable, Category = "Courier|WebSocket", meta = (AutoCreateRefTerm = "Variables"))
	static UCourierWebSocket* CreateCourierWebSocketFromRequest(const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables);

	UFUNCTION(BlueprintCallable, Category = "Courier|WebSocket")
	void Connect();

	UFUNCTION(BlueprintCallable, Category = "Courier|WebSocket")
	void Close(int32 StatusCode = 1000, const FString& Reason = TEXT(""));

	UFUNCTION(BlueprintCallable, Category = "Courier|WebSocket")
	bool SendText(const FString& Message);

	UFUNCTION(BlueprintCallable, Category = "Courier|WebSocket")
	bool SendBinary(const TArray<uint8>& Data);

	UFUNCTION(BlueprintPure, Category = "Courier|WebSocket")
	bool IsConnected() const;

	UFUNCTION(BlueprintPure, Category = "Courier|WebSocket")
	const FString& GetUrl() const { return Url; }

	UPROPERTY(BlueprintAssignable, Category = "Courier|WebSocket")
	FCourierWebSocketConnectedDelegate OnConnected;

	UPROPERTY(BlueprintAssignable, Category = "Courier|WebSocket")
	FCourierWebSocketErrorDelegate OnConnectionError;

	UPROPERTY(BlueprintAssignable, Category = "Courier|WebSocket")
	FCourierWebSocketClosedDelegate OnClosed;

	UPROPERTY(BlueprintAssignable, Category = "Courier|WebSocket")
	FCourierWebSocketMessageDelegate OnMessage;

	/** A whole binary message (fragments are joined first). */
	UPROPERTY(BlueprintAssignable, Category = "Courier|WebSocket")
	FCourierWebSocketBinaryDelegate OnBinaryMessage;

	UPROPERTY(BlueprintAssignable, Category = "Courier|WebSocket")
	FCourierWebSocketMessageDelegate OnMessageSent;

	virtual void BeginDestroy() override;

	/** The C++ socket with Courier's resolved URL, protocols and headers; null when the URL is empty. */
	static TSharedPtr<IWebSocket> MakeSocket(const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables, FString& OutUrl);

private:
	void Unbind();

	FString Url;
	TArray<FString> Protocols;
	TMap<FString, FString> Headers;
	TSharedPtr<IWebSocket> Socket;
	TArray<uint8> PendingBinary;
};
