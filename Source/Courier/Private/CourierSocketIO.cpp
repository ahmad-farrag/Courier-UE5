#include "CourierSocketIO.h"

#include "CourierHttp.h"
#include "Dom/JsonObject.h"
#include "IWebSocket.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WebSocketsModule.h"

namespace CourierSocketIO
{
	FString NamespacePrefix(const FString& Namespace)
	{
		return Namespace.IsEmpty() || Namespace == TEXT("/") ? FString() : Namespace + TEXT(",");
	}

	FString NormalizeNamespace(const FString& Namespace)
	{
		FString Out = Namespace.TrimStartAndEnd();
		if (Out.IsEmpty())
		{
			return TEXT("/");
		}
		return Out.StartsWith(TEXT("/")) ? Out : TEXT("/") + Out;
	}

	FString Condense(const TSharedPtr<FJsonValue>& Value)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Value, FString(), Writer);
		return Out;
	}

	bool ParseArray(const FString& Text, TArray<TSharedPtr<FJsonValue>>& Out)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Out);
	}
}

FString FCourierSocketIO::MakeEngineIoUrl(const FString& Url)
{
	FString Base = Url.TrimStartAndEnd();
	FString Query;
	Base.Split(TEXT("?"), &Base, &Query);
	if (Base.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
	{
		Base = TEXT("wss://") + Base.Mid(8);
	}
	else if (Base.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase))
	{
		Base = TEXT("ws://") + Base.Mid(7);
	}
	else if (!Base.Contains(TEXT("://")))
	{
		Base = TEXT("ws://") + Base;
	}
	// No path (or just "/") means the default /socket.io/ path.
	const int32 HostStart = Base.Find(TEXT("://")) + 3;
	int32 PathStart = INDEX_NONE;
	for (int32 Index = HostStart; Index < Base.Len(); ++Index)
	{
		if (Base[Index] == TEXT('/'))
		{
			PathStart = Index;
			break;
		}
	}
	const FString Path = PathStart == INDEX_NONE ? FString() : Base.Mid(PathStart);
	if (Path.IsEmpty() || Path == TEXT("/"))
	{
		Base = (PathStart == INDEX_NONE ? Base : Base.Left(PathStart)) + TEXT("/socket.io/");
	}
	else if (!Base.EndsWith(TEXT("/")))
	{
		Base += TEXT("/");
	}
	TArray<FString> Parts;
	Query.ParseIntoArray(Parts, TEXT("&"));
	Parts.RemoveAll([](const FString& Part) { return Part.StartsWith(TEXT("EIO=")) || Part.StartsWith(TEXT("transport=")); });
	Parts.Insert(TEXT("transport=websocket"), 0);
	Parts.Insert(TEXT("EIO=4"), 0);
	return Base + TEXT("?") + FString::Join(Parts, TEXT("&"));
}

FString FCourierSocketIO::EncodeEvent(const FString& Namespace, const FString& EventName, const FString& ArgsJsonArray, int32 AckId)
{
	TArray<TSharedPtr<FJsonValue>> Args;
	if (!ArgsJsonArray.TrimStartAndEnd().IsEmpty())
	{
		CourierSocketIO::ParseArray(ArgsJsonArray, Args);
	}
	Args.Insert(MakeShared<FJsonValueString>(EventName), 0);
	const FString Payload = CourierSocketIO::Condense(MakeShared<FJsonValueArray>(Args));
	return TEXT("2") + CourierSocketIO::NamespacePrefix(Namespace) + (AckId >= 0 ? FString::FromInt(AckId) : FString()) + Payload;
}

FString FCourierSocketIO::EncodeAck(const FString& Namespace, int32 AckId, const FString& ArgsJsonArray)
{
	return TEXT("3") + CourierSocketIO::NamespacePrefix(Namespace) + FString::FromInt(AckId) + (ArgsJsonArray.IsEmpty() ? FString(TEXT("[]")) : ArgsJsonArray);
}

FString FCourierSocketIO::EncodeConnect(const FString& Namespace, const FString& AuthJson)
{
	return TEXT("0") + CourierSocketIO::NamespacePrefix(Namespace) + AuthJson.TrimStartAndEnd();
}

FString FCourierSocketIO::EncodeDisconnect(const FString& Namespace)
{
	return TEXT("1") + CourierSocketIO::NamespacePrefix(Namespace);
}

bool FCourierSocketIO::Decode(const FString& Text, FCourierSocketIOPacket& Out)
{
	Out = FCourierSocketIOPacket();
	if (Text.IsEmpty() || !FChar::IsDigit(Text[0]))
	{
		return false;
	}
	Out.Type = Text[0] - TEXT('0');
	if (Out.Type > 6)
	{
		return false;
	}
	int32 Index = 1;
	// Binary packets carry "<attachments>-" first; Courier reads the header and leaves the attachments.
	if (Out.Type == 5 || Out.Type == 6)
	{
		while (Index < Text.Len() && FChar::IsDigit(Text[Index]))
		{
			++Index;
		}
		if (Index < Text.Len() && Text[Index] == TEXT('-'))
		{
			++Index;
		}
	}
	if (Index < Text.Len() && Text[Index] == TEXT('/'))
	{
		const int32 Comma = Text.Find(TEXT(","), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index);
		Out.Namespace = Comma == INDEX_NONE ? Text.Mid(Index) : Text.Mid(Index, Comma - Index);
		Index = Comma == INDEX_NONE ? Text.Len() : Comma + 1;
	}
	const int32 IdStart = Index;
	while (Index < Text.Len() && FChar::IsDigit(Text[Index]))
	{
		++Index;
	}
	if (Index > IdStart)
	{
		Out.AckId = FCString::Atoi(*Text.Mid(IdStart, Index - IdStart));
	}
	Out.Payload = Text.Mid(Index);
	return true;
}

bool FCourierSocketIO::SplitEvent(const FString& PayloadArray, FString& OutEventName, FString& OutArgsJsonArray)
{
	TArray<TSharedPtr<FJsonValue>> Items;
	if (!CourierSocketIO::ParseArray(PayloadArray, Items) || Items.Num() == 0 || !Items[0].IsValid() || !Items[0]->TryGetString(OutEventName))
	{
		return false;
	}
	Items.RemoveAt(0);
	OutArgsJsonArray = CourierSocketIO::Condense(MakeShared<FJsonValueArray>(Items));
	return true;
}

FString FCourierSocketIO::ArgsFromText(const FString& Text)
{
	const FString Trimmed = Text.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return TEXT("[]");
	}
	// UE's reader only takes objects and arrays at the top level, so parse the value inside an array.
	TArray<TSharedPtr<FJsonValue>> Wrapped;
	if (CourierSocketIO::ParseArray(TEXT("[") + Trimmed + TEXT("]"), Wrapped) && Wrapped.Num() == 1 && Wrapped[0].IsValid())
	{
		return CourierSocketIO::Condense(MakeShared<FJsonValueArray>(Wrapped));
	}
	return CourierSocketIO::Condense(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueString>(Text) }));
}

// ------------------------------------------------------------------------------------------------

FCourierSocketIOConnection::~FCourierSocketIOConnection()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	if (Socket.IsValid())
	{
		Socket->OnConnected().Clear();
		Socket->OnConnectionError().Clear();
		Socket->OnClosed().Clear();
		Socket->OnMessage().Clear();
		if (Socket->IsConnected())
		{
			Socket->Close();
		}
	}
}

bool FCourierSocketIOConnection::Connect(const FString& Url, const TMap<FString, FString>& Headers, const FString& InNamespace, const FString& InAuthJson)
{
	if (Url.TrimStartAndEnd().IsEmpty())
	{
		return false;
	}
	EngineIoUrl = FCourierSocketIO::MakeEngineIoUrl(Url);
	Namespace = CourierSocketIO::NormalizeNamespace(InNamespace);
	AuthJson = InAuthJson;
	bConnected = false;
	bClosing = false;
	SocketId.Reset();

	FWebSocketsModule& Module = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	Socket = Module.CreateWebSocket(EngineIoUrl, TArray<FString>(), Headers);

	TWeakPtr<FCourierSocketIOConnection> WeakSelf = AsShared();
	Socket->OnConnectionError().AddLambda([WeakSelf](const FString& Error)
	{
		if (const TSharedPtr<FCourierSocketIOConnection> Self = WeakSelf.Pin())
		{
			if (Self->OnConnectError) { Self->OnConnectError(Error); }
		}
	});
	Socket->OnClosed().AddLambda([WeakSelf](int32 StatusCode, const FString& Reason, bool)
	{
		if (const TSharedPtr<FCourierSocketIOConnection> Self = WeakSelf.Pin())
		{
			const bool bWasConnected = Self->bConnected;
			Self->bConnected = false;
			FTSTicker::GetCoreTicker().RemoveTicker(Self->TickHandle);
			if (Self->OnDisconnected && (bWasConnected || !Self->bClosing))
			{
				Self->OnDisconnected(Self->bClosing ? FString(TEXT("io client disconnect")) : FString::Printf(TEXT("transport close (%d%s%s)"), StatusCode, Reason.IsEmpty() ? TEXT("") : TEXT(", "), *Reason));
			}
		}
	});
	Socket->OnMessage().AddLambda([WeakSelf](const FString& Frame)
	{
		if (const TSharedPtr<FCourierSocketIOConnection> Self = WeakSelf.Pin())
		{
			Self->HandleFrame(Frame);
		}
	});
	Socket->Connect();
	return true;
}

void FCourierSocketIOConnection::Send(const FString& Frame)
{
	if (Socket.IsValid() && Socket->IsConnected())
	{
		Socket->Send(Frame);
		if (OnFrame) { OnFrame(true, Frame); }
	}
}

void FCourierSocketIOConnection::HandleFrame(const FString& Frame)
{
	if (Frame.IsEmpty())
	{
		return;
	}
	if (OnFrame) { OnFrame(false, Frame); }
	PingDeadline = FPlatformTime::Seconds() + PingWindow;

	switch (Frame[0])
	{
	case TEXT('0'):	// Engine.IO open: read the ping timing, then join the namespace.
	{
		TSharedPtr<FJsonObject> Open;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Frame.Mid(1));
		if (FJsonSerializer::Deserialize(Reader, Open) && Open.IsValid())
		{
			double Interval = 25000.0;
			double Timeout = 20000.0;
			Open->TryGetNumberField(TEXT("pingInterval"), Interval);
			Open->TryGetNumberField(TEXT("pingTimeout"), Timeout);
			PingWindow = (Interval + Timeout) / 1000.0 + 1.0;
			PingDeadline = FPlatformTime::Seconds() + PingWindow;
			TWeakPtr<FCourierSocketIOConnection> WeakSelf = AsShared();
			FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
			TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakSelf](float DeltaTime)
			{
				const TSharedPtr<FCourierSocketIOConnection> Self = WeakSelf.Pin();
				return Self.IsValid() && Self->Tick(DeltaTime);
			}), 1.f);
		}
		Send(TEXT("4") + FCourierSocketIO::EncodeConnect(Namespace, AuthJson));
		break;
	}
	case TEXT('1'):	// Engine.IO close.
		Close(TEXT("transport close"));
		break;
	case TEXT('2'):	// Ping from the server (Engine.IO 4): answer with pong.
		Send(TEXT("3") + Frame.Mid(1));
		break;
	case TEXT('4'):
	{
		FCourierSocketIOPacket Packet;
		if (!FCourierSocketIO::Decode(Frame.Mid(1), Packet) || Packet.Namespace != Namespace)
		{
			break;
		}
		switch (Packet.Type)
		{
		case 0:
		{
			TSharedPtr<FJsonObject> Info;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Packet.Payload);
			if (FJsonSerializer::Deserialize(Reader, Info) && Info.IsValid())
			{
				Info->TryGetStringField(TEXT("sid"), SocketId);
			}
			bConnected = true;
			if (OnConnected) { OnConnected(SocketId); }
			break;
		}
		case 1:
			bConnected = false;
			if (OnDisconnected) { OnDisconnected(TEXT("io server disconnect")); }
			break;
		case 2:
		{
			FString Name, Args;
			if (FCourierSocketIO::SplitEvent(Packet.Payload, Name, Args))
			{
				if (OnEvent) { OnEvent(Name, Args); }
				// The server asked for an acknowledgement; answer with no arguments.
				if (Packet.AckId >= 0)
				{
					Send(TEXT("4") + FCourierSocketIO::EncodeAck(Namespace, Packet.AckId, TEXT("[]")));
				}
			}
			break;
		}
		case 3:
			if (OnAck) { OnAck(Packet.AckId, Packet.Payload); }
			break;
		case 4:
		{
			FString Message = Packet.Payload;
			TSharedPtr<FJsonObject> Error;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Packet.Payload);
			if (FJsonSerializer::Deserialize(Reader, Error) && Error.IsValid())
			{
				Error->TryGetStringField(TEXT("message"), Message);
			}
			if (OnConnectError) { OnConnectError(Message); }
			break;
		}
		default:
			UE_LOG(LogCourier, Warning, TEXT("Courier: Socket.IO binary packets are not supported; one was skipped."));
			break;
		}
		break;
	}
	default:
		break;
	}
}

bool FCourierSocketIOConnection::Tick(float)
{
	if (PingWindow > 0.0 && Socket.IsValid() && Socket->IsConnected() && FPlatformTime::Seconds() > PingDeadline)
	{
		Close(TEXT("ping timeout"));
		return false;
	}
	return Socket.IsValid();
}

int32 FCourierSocketIOConnection::Emit(const FString& EventName, const FString& ArgsJsonArray, bool bWantAck)
{
	if (!bConnected)
	{
		return -1;
	}
	const int32 AckId = bWantAck ? NextAckId++ : -1;
	Send(TEXT("4") + FCourierSocketIO::EncodeEvent(Namespace, EventName, ArgsJsonArray, AckId));
	return bWantAck ? AckId : 0;
}

void FCourierSocketIOConnection::Close(const FString& Reason)
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	const bool bWasConnected = bConnected;
	bConnected = false;
	bClosing = true;
	if (Socket.IsValid() && Socket->IsConnected())
	{
		Socket->Close();
	}
	if (bWasConnected && OnDisconnected && Reason != TEXT("io client disconnect"))
	{
		OnDisconnected(Reason);
	}
}

void FCourierSocketIOConnection::Disconnect()
{
	if (bConnected)
	{
		Send(TEXT("4") + FCourierSocketIO::EncodeDisconnect(Namespace));
	}
	const bool bWasConnected = bConnected;
	Close(TEXT("io client disconnect"));
	if (bWasConnected && OnDisconnected)
	{
		OnDisconnected(TEXT("io client disconnect"));
	}
	if (Socket.IsValid())
	{
		// The close handshake would report the reason again.
		Socket->OnClosed().Clear();
	}
}

// ------------------------------------------------------------------------------------------------

UCourierSocketIO* UCourierSocketIO::CreateCourierSocketIO(const FString& InUrl, const FString& InNamespace, const FString& InAuthJson, const TMap<FString, FString>& InHeaders)
{
	UCourierSocketIO* Client = NewObject<UCourierSocketIO>();
	Client->Url = InUrl;
	Client->Namespace = InNamespace;
	Client->AuthJson = InAuthJson;
	Client->Headers = InHeaders;
	return Client;
}

UCourierSocketIO* UCourierSocketIO::CreateCourierSocketIOFromRequest(const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables)
{
	FCourierRequestSpec Spec = Request;
	Spec.Protocol = ECourierProtocol::SocketIO;
	Spec.BodyType = ECourierBodyType::None;
	FCourierPreparedRequest Prepared;
	FString Error;
	FCourierHttp::Prepare(Spec, Variables, Prepared, Error);

	UCourierSocketIO* Client = NewObject<UCourierSocketIO>();
	Client->Url = Prepared.Url;
	Client->Namespace = FCourierHttp::ResolveVariables(Request.SocketIONamespace, Variables);
	Client->AuthJson = FCourierHttp::ResolveVariables(Request.SocketIOAuth, Variables);
	for (const TPair<FString, FString>& Header : Prepared.Headers)
	{
		Client->Headers.Add(Header.Key, Header.Value);
	}
	return Client;
}

void UCourierSocketIO::Connect()
{
	if (Connection.IsValid())
	{
		Connection->OnDisconnected = nullptr;
		Connection->Disconnect();
	}
	Connection = MakeShared<FCourierSocketIOConnection>();
	TWeakObjectPtr<UCourierSocketIO> WeakThis(this);
	Connection->OnConnected = [WeakThis](const FString& Sid) { if (UCourierSocketIO* Self = WeakThis.Get()) { Self->OnConnected.Broadcast(Sid); } };
	Connection->OnConnectError = [WeakThis](const FString& Message) { if (UCourierSocketIO* Self = WeakThis.Get()) { Self->OnConnectError.Broadcast(Message); } };
	Connection->OnDisconnected = [WeakThis](const FString& Reason) { if (UCourierSocketIO* Self = WeakThis.Get()) { Self->OnDisconnected.Broadcast(Reason); } };
	Connection->OnEvent = [WeakThis](const FString& Name, const FString& Args) { if (UCourierSocketIO* Self = WeakThis.Get()) { Self->OnEvent.Broadcast(Name, Args); } };
	Connection->OnAck = [WeakThis](int32 Id, const FString& Args) { if (UCourierSocketIO* Self = WeakThis.Get()) { Self->OnAck.Broadcast(Id, Args); } };
	if (!Connection->Connect(Url, Headers, Namespace, AuthJson))
	{
		OnConnectError.Broadcast(TEXT("The URL is empty."));
	}
}

void UCourierSocketIO::Disconnect()
{
	if (Connection.IsValid())
	{
		Connection->Disconnect();
	}
}

bool UCourierSocketIO::Emit(const FString& EventName, const FString& Data)
{
	return Connection.IsValid() && Connection->Emit(EventName, FCourierSocketIO::ArgsFromText(Data)) >= 0;
}

int32 UCourierSocketIO::EmitWithAck(const FString& EventName, const FString& Data)
{
	return Connection.IsValid() ? Connection->Emit(EventName, FCourierSocketIO::ArgsFromText(Data), true) : -1;
}

bool UCourierSocketIO::EmitArgs(const FString& EventName, const FString& ArgsJsonArray)
{
	return Connection.IsValid() && Connection->Emit(EventName, ArgsJsonArray) >= 0;
}

bool UCourierSocketIO::IsConnected() const
{
	return Connection.IsValid() && Connection->IsConnected();
}

FString UCourierSocketIO::GetSocketId() const
{
	return Connection.IsValid() ? Connection->GetSocketId() : FString();
}

void UCourierSocketIO::BeginDestroy()
{
	if (Connection.IsValid())
	{
		Connection->OnConnected = nullptr;
		Connection->OnConnectError = nullptr;
		Connection->OnDisconnected = nullptr;
		Connection->OnEvent = nullptr;
		Connection->OnAck = nullptr;
		Connection.Reset();
	}
	Super::BeginDestroy();
}
