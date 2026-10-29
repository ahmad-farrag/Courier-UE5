#include "CourierWebSocket.h"

#include "CourierHttp.h"
#include "IWebSocket.h"
#include "WebSocketsModule.h"

UCourierWebSocket* UCourierWebSocket::CreateCourierWebSocket(const FString& InUrl, const TArray<FString>& InProtocols, const TMap<FString, FString>& InHeaders)
{
	UCourierWebSocket* Socket = NewObject<UCourierWebSocket>();
	Socket->Url = InUrl;
	Socket->Protocols = InProtocols;
	Socket->Headers = InHeaders;
	return Socket;
}

UCourierWebSocket* UCourierWebSocket::CreateCourierWebSocketFromRequest(const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables)
{
	FCourierRequestSpec Spec = Request;
	Spec.Protocol = ECourierProtocol::WebSocket;
	FCourierPreparedRequest Prepared;
	FString Error;
	Spec.BodyType = ECourierBodyType::None;
	FCourierHttp::Prepare(Spec, Variables, Prepared, Error);

	UCourierWebSocket* Socket = NewObject<UCourierWebSocket>();
	Socket->Url = Prepared.Url;
	Socket->Protocols = Request.WebSocketProtocols;
	for (const TPair<FString, FString>& Header : Prepared.Headers)
	{
		Socket->Headers.Add(Header.Key, Header.Value);
	}
	return Socket;
}

TSharedPtr<IWebSocket> UCourierWebSocket::MakeSocket(const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables, FString& OutUrl)
{
	FCourierRequestSpec Spec = Request;
	Spec.Protocol = ECourierProtocol::WebSocket;
	Spec.BodyType = ECourierBodyType::None;
	FCourierPreparedRequest Prepared;
	FString Error;
	FCourierHttp::Prepare(Spec, Variables, Prepared, Error);
	OutUrl = Prepared.Url;
	if (OutUrl.IsEmpty())
	{
		return nullptr;
	}
	TMap<FString, FString> UpgradeHeaders;
	for (const TPair<FString, FString>& Header : Prepared.Headers)
	{
		UpgradeHeaders.Add(Header.Key, Header.Value);
	}
	FWebSocketsModule& Module = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	return Module.CreateWebSocket(OutUrl, Request.WebSocketProtocols, UpgradeHeaders);
}

void UCourierWebSocket::Connect()
{
	if (Socket.IsValid())
	{
		Unbind();
		Socket->Close();
		Socket.Reset();
	}
	if (Url.IsEmpty())
	{
		OnConnectionError.Broadcast(TEXT("The URL is empty."));
		return;
	}

	FWebSocketsModule& Module = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	Socket = Module.CreateWebSocket(Url, Protocols, Headers);
	PendingBinary.Reset();

	TWeakObjectPtr<UCourierWebSocket> WeakThis(this);
	Socket->OnConnected().AddLambda([WeakThis]()
	{
		if (UCourierWebSocket* Self = WeakThis.Get()) { Self->OnConnected.Broadcast(); }
	});
	Socket->OnConnectionError().AddLambda([WeakThis](const FString& Error)
	{
		if (UCourierWebSocket* Self = WeakThis.Get()) { Self->OnConnectionError.Broadcast(Error); }
	});
	Socket->OnClosed().AddLambda([WeakThis](int32 StatusCode, const FString& Reason, bool bWasClean)
	{
		if (UCourierWebSocket* Self = WeakThis.Get()) { Self->OnClosed.Broadcast(StatusCode, Reason, bWasClean); }
	});
	Socket->OnMessage().AddLambda([WeakThis](const FString& Message)
	{
		if (UCourierWebSocket* Self = WeakThis.Get()) { Self->OnMessage.Broadcast(Message); }
	});
	Socket->OnBinaryMessage().AddLambda([WeakThis](const void* Data, SIZE_T Size, bool bIsLastFragment)
	{
		if (UCourierWebSocket* Self = WeakThis.Get())
		{
			Self->PendingBinary.Append(static_cast<const uint8*>(Data), static_cast<int32>(Size));
			if (bIsLastFragment)
			{
				const TArray<uint8> Message = MoveTemp(Self->PendingBinary);
				Self->PendingBinary.Reset();
				Self->OnBinaryMessage.Broadcast(Message);
			}
		}
	});
	Socket->OnMessageSent().AddLambda([WeakThis](const FString& Message)
	{
		if (UCourierWebSocket* Self = WeakThis.Get()) { Self->OnMessageSent.Broadcast(Message); }
	});
	Socket->Connect();
}

void UCourierWebSocket::Close(int32 StatusCode, const FString& Reason)
{
	if (Socket.IsValid())
	{
		Socket->Close(StatusCode, Reason);
	}
}

bool UCourierWebSocket::SendText(const FString& Message)
{
	if (!IsConnected())
	{
		return false;
	}
	Socket->Send(Message);
	return true;
}

bool UCourierWebSocket::SendBinary(const TArray<uint8>& Data)
{
	if (!IsConnected())
	{
		return false;
	}
	Socket->Send(Data.GetData(), Data.Num(), true);
	return true;
}

bool UCourierWebSocket::IsConnected() const
{
	return Socket.IsValid() && Socket->IsConnected();
}

void UCourierWebSocket::Unbind()
{
	if (Socket.IsValid())
	{
		Socket->OnConnected().Clear();
		Socket->OnConnectionError().Clear();
		Socket->OnClosed().Clear();
		Socket->OnMessage().Clear();
		Socket->OnBinaryMessage().Clear();
		Socket->OnMessageSent().Clear();
	}
}

void UCourierWebSocket::BeginDestroy()
{
	if (Socket.IsValid())
	{
		Unbind();
		if (Socket->IsConnected())
		{
			Socket->Close();
		}
		Socket.Reset();
	}
	Super::BeginDestroy();
}
