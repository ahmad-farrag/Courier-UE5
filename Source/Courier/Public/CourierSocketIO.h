#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "CourierTypes.h"
#include "UObject/Object.h"
#include "CourierSocketIO.generated.h"

class IWebSocket;

/** One decoded Socket.IO packet (the part after Engine.IO's "4" message prefix). */
struct COURIER_API FCourierSocketIOPacket
{
	/** 0 CONNECT, 1 DISCONNECT, 2 EVENT, 3 ACK, 4 CONNECT_ERROR, 5 BINARY_EVENT, 6 BINARY_ACK. */
	int32 Type = -1;
	FString Namespace = TEXT("/");
	/** -1 when the packet carries no acknowledgement id. */
	int32 AckId = -1;
	/** JSON text: an object for CONNECT / CONNECT_ERROR, an array for EVENT / ACK. */
	FString Payload;
};

/** Socket.IO v4 wire format over Engine.IO 4, plus a connection that speaks it over a WebSocket. */
class COURIER_API FCourierSocketIO
{
public:
	/** http(s)://host/ or ws(s)://host/ becomes ws(s)://host/socket.io/?EIO=4&transport=websocket (path kept if given). */
	static FString MakeEngineIoUrl(const FString& Url);

	/** "42/chat,7[\"move\",{...}]" from its parts. ArgsJsonArray is a JSON array ("[]" for no arguments). */
	static FString EncodeEvent(const FString& Namespace, const FString& EventName, const FString& ArgsJsonArray, int32 AckId = -1);
	static FString EncodeAck(const FString& Namespace, int32 AckId, const FString& ArgsJsonArray);
	static FString EncodeConnect(const FString& Namespace, const FString& AuthJson);
	static FString EncodeDisconnect(const FString& Namespace);

	/** Parses a Socket.IO packet (without the Engine.IO "4"). */
	static bool Decode(const FString& Text, FCourierSocketIOPacket& Out);

	/** Splits an EVENT payload into its name and the remaining arguments as a JSON array. */
	static bool SplitEvent(const FString& PayloadArray, FString& OutEventName, FString& OutArgsJsonArray);

	/** Text to send as one argument: valid JSON is sent as is, anything else as a JSON string. Empty gives "[]". */
	static FString ArgsFromText(const FString& Text);
};

/** A live Socket.IO connection in plain C++. Callbacks run on the game thread. */
class COURIER_API FCourierSocketIOConnection : public TSharedFromThis<FCourierSocketIOConnection>
{
public:
	TFunction<void(const FString& SocketId)> OnConnected;
	TFunction<void(const FString& Message)> OnConnectError;
	TFunction<void(const FString& Reason)> OnDisconnected;
	TFunction<void(const FString& EventName, const FString& ArgsJson)> OnEvent;
	TFunction<void(int32 AckId, const FString& ArgsJson)> OnAck;
	/** Every Engine.IO frame, for consoles. bSent is true for frames Courier wrote. */
	TFunction<void(bool bSent, const FString& Frame)> OnFrame;

	~FCourierSocketIOConnection();

	/** Url is the server's http(s) or ws(s) address; Headers go on the WebSocket upgrade. */
	bool Connect(const FString& Url, const TMap<FString, FString>& Headers, const FString& Namespace, const FString& AuthJson);
	void Disconnect();
	bool IsConnected() const { return bConnected; }
	const FString& GetSocketId() const { return SocketId; }
	const FString& GetUrl() const { return EngineIoUrl; }

	/** Emits an event. With bWantAck, returns the acknowledgement id (OnAck fires with it); otherwise 0. -1 when not connected. */
	int32 Emit(const FString& EventName, const FString& ArgsJsonArray, bool bWantAck = false);

private:
	void HandleFrame(const FString& Frame);
	void Send(const FString& Frame);
	void Close(const FString& Reason);
	bool Tick(float DeltaTime);

	TSharedPtr<IWebSocket> Socket;
	FString EngineIoUrl;
	FString Namespace = TEXT("/");
	FString AuthJson;
	FString SocketId;
	bool bConnected = false;
	bool bClosing = false;
	int32 NextAckId = 0;
	double PingDeadline = 0.0;
	double PingWindow = 0.0;
	FTSTicker::FDelegateHandle TickHandle;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCourierSocketIOConnectedDelegate, const FString&, SocketId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCourierSocketIOMessageDelegate, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCourierSocketIOEventDelegate, const FString&, EventName, const FString&, ArgsJson);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCourierSocketIOAckDelegate, int32, AckId, const FString&, ArgsJson);

/**
 * A Socket.IO client for Blueprints (servers on Socket.IO v3 or v4). Keep a reference to it, bind the
 * events, then call Connect. Event arguments arrive as a JSON array: read them with Get Json String
 * and paths such as "[0].score". Events fire on the game thread.
 */
UCLASS(BlueprintType)
class COURIER_API UCourierSocketIO : public UObject
{
	GENERATED_BODY()

public:
	/** Url: the server address (https://example.com, or with a custom path such as https://example.com/realtime/). */
	UFUNCTION(BlueprintCallable, Category = "Courier|Socket.IO", meta = (AutoCreateRefTerm = "Headers"))
	static UCourierSocketIO* CreateCourierSocketIO(const FString& Url, const FString& Namespace, const FString& AuthJson, const TMap<FString, FString>& Headers);

	/** Url, query, headers, auth (bearer and friends become upgrade headers), namespace and auth payload from a request. */
	UFUNCTION(BlueprintCallable, Category = "Courier|Socket.IO", meta = (AutoCreateRefTerm = "Variables"))
	static UCourierSocketIO* CreateCourierSocketIOFromRequest(const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables);

	UFUNCTION(BlueprintCallable, Category = "Courier|Socket.IO")
	void Connect();

	UFUNCTION(BlueprintCallable, Category = "Courier|Socket.IO")
	void Disconnect();

	/** Data: any JSON value (sent as is) or plain text (sent as a string). Empty sends no argument. False when not connected. */
	UFUNCTION(BlueprintCallable, Category = "Courier|Socket.IO")
	bool Emit(const FString& EventName, const FString& Data);

	/** Like Emit, and asks the server to acknowledge: On Ack fires with the returned id. -1 when not connected. */
	UFUNCTION(BlueprintCallable, Category = "Courier|Socket.IO")
	int32 EmitWithAck(const FString& EventName, const FString& Data);

	/** Sends several arguments: ArgsJsonArray is a JSON array such as [1, "two", {"three": 3}]. */
	UFUNCTION(BlueprintCallable, Category = "Courier|Socket.IO")
	bool EmitArgs(const FString& EventName, const FString& ArgsJsonArray);

	UFUNCTION(BlueprintPure, Category = "Courier|Socket.IO")
	bool IsConnected() const;

	UFUNCTION(BlueprintPure, Category = "Courier|Socket.IO")
	FString GetSocketId() const;

	UPROPERTY(BlueprintAssignable, Category = "Courier|Socket.IO")
	FCourierSocketIOConnectedDelegate OnConnected;

	/** The server refused the namespace (CONNECT_ERROR) or the WebSocket failed. */
	UPROPERTY(BlueprintAssignable, Category = "Courier|Socket.IO")
	FCourierSocketIOMessageDelegate OnConnectError;

	UPROPERTY(BlueprintAssignable, Category = "Courier|Socket.IO")
	FCourierSocketIOMessageDelegate OnDisconnected;

	/** Every event from the server. Switch on Event Name; Args Json is a JSON array of its arguments. */
	UPROPERTY(BlueprintAssignable, Category = "Courier|Socket.IO")
	FCourierSocketIOEventDelegate OnEvent;

	UPROPERTY(BlueprintAssignable, Category = "Courier|Socket.IO")
	FCourierSocketIOAckDelegate OnAck;

	virtual void BeginDestroy() override;

private:
	FString Url;
	FString Namespace = TEXT("/");
	FString AuthJson;
	TMap<FString, FString> Headers;
	TSharedPtr<FCourierSocketIOConnection> Connection;
};
