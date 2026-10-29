#include "CourierEventSource.h"

#include "Async/Async.h"
#include "CourierHttp.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/Archive.h"

// ---- Parser ----

void FCourierSseParser::Reset()
{
	LineBytes.Reset();
	EventName.Reset();
	DataBuffer.Reset();
	LastEventId.Reset();
	Retry = -1;
	bHasData = false;
	bLastWasCR = false;
}

void FCourierSseParser::Feed(const uint8* Data, int64 Length, TArray<FCourierSseEvent>& OutEvents)
{
	for (int64 Index = 0; Index < Length; ++Index)
	{
		const uint8 Byte = Data[Index];
		if (Byte == '\n' && bLastWasCR)
		{
			// The \n of a \r\n pair, possibly split across chunks.
			bLastWasCR = false;
			continue;
		}
		bLastWasCR = Byte == '\r';
		if (Byte == '\n' || Byte == '\r')
		{
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(LineBytes.GetData()), LineBytes.Num());
			ProcessLine(FString(Converted.Length(), Converted.Get()), OutEvents);
			LineBytes.Reset();
		}
		else
		{
			LineBytes.Add(Byte);
		}
	}
}

void FCourierSseParser::ProcessLine(const FString& Line, TArray<FCourierSseEvent>& OutEvents)
{
	if (Line.IsEmpty())
	{
		if (bHasData)
		{
			FCourierSseEvent& Event = OutEvents.AddDefaulted_GetRef();
			Event.EventName = EventName.IsEmpty() ? FString(TEXT("message")) : EventName;
			Event.Data = DataBuffer.EndsWith(TEXT("\n")) ? DataBuffer.LeftChop(1) : DataBuffer;
			Event.Id = LastEventId;
			Event.Retry = Retry;
		}
		EventName.Reset();
		DataBuffer.Reset();
		bHasData = false;
		Retry = -1;
		return;
	}
	if (Line[0] == TEXT(':'))
	{
		return; // comment / keep-alive
	}

	FString Field = Line;
	FString Value;
	int32 Colon = INDEX_NONE;
	if (Line.FindChar(TEXT(':'), Colon))
	{
		Field = Line.Left(Colon);
		Value = Line.Mid(Colon + 1);
		if (Value.StartsWith(TEXT(" ")))
		{
			Value.RightChopInline(1);
		}
	}

	if (Field == TEXT("data"))
	{
		DataBuffer += Value + TEXT("\n");
		bHasData = true;
	}
	else if (Field == TEXT("event"))
	{
		EventName = Value;
	}
	else if (Field == TEXT("id"))
	{
		LastEventId = Value;
	}
	else if (Field == TEXT("retry"))
	{
		if (!Value.IsEmpty() && Value.IsNumeric())
		{
			Retry = FCString::Atoi(*Value);
		}
	}
}

// ---- Connection ----

namespace CourierSse
{
	/** Receives the body as it streams in (HTTP thread) and hands complete events to the game thread. */
	class FStreamArchive : public FArchive
	{
	public:
		explicit FStreamArchive(TWeakPtr<FCourierSseConnection, ESPMode::ThreadSafe> InOwner)
			: Owner(MoveTemp(InOwner))
		{
			SetIsSaving(true);
			SetIsPersistent(false);
		}

		virtual void Serialize(void* Data, int64 Length) override
		{
			TArray<FCourierSseEvent> Events;
			Parser.Feed(static_cast<const uint8*>(Data), Length, Events);
			if (Events.Num() > 0)
			{
				AsyncTask(ENamedThreads::GameThread, [WeakOwner = Owner, Events = MoveTemp(Events)]()
				{
					if (const TSharedPtr<FCourierSseConnection, ESPMode::ThreadSafe> Pinned = WeakOwner.Pin())
					{
						Pinned->Dispatch(Events);
					}
				});
			}
		}

		virtual FString GetArchiveName() const override { return TEXT("CourierSseStream"); }

	private:
		TWeakPtr<FCourierSseConnection, ESPMode::ThreadSafe> Owner;
		FCourierSseParser Parser;
	};
}

FCourierSseConnection::~FCourierSseConnection()
{
	if (Request.IsValid())
	{
		Request->OnProcessRequestComplete().Unbind();
		Request->OnStatusCodeReceived().Unbind();
		Request->CancelRequest();
	}
}

bool FCourierSseConnection::Open(const FCourierPreparedRequest& Prepared)
{
	if (bOpen)
	{
		return false;
	}
	Request = FHttpModule::Get().CreateRequest();
	Request->SetVerb(Prepared.Verb.IsEmpty() ? FString(TEXT("GET")) : Prepared.Verb);
	Request->SetURL(Prepared.Url);
	Request->SetHeader(TEXT("Accept"), TEXT("text/event-stream"));
	Request->SetHeader(TEXT("Cache-Control"), TEXT("no-cache"));
	for (const TPair<FString, FString>& Header : Prepared.Headers)
	{
		Request->SetHeader(Header.Key, Header.Value);
	}
	if (Prepared.Body.Num() > 0)
	{
		Request->SetContent(Prepared.Body);
	}
	// A stream is open until someone closes it: no total timeout, and an hour without bytes before giving up.
	Request->ClearTimeout();
	Request->SetActivityTimeout(3600.f);
	Request->SetResponseBodyReceiveStream(MakeShared<CourierSse::FStreamArchive>(AsWeak()));

	TWeakPtr<FCourierSseConnection, ESPMode::ThreadSafe> WeakSelf = AsWeak();
	Request->OnStatusCodeReceived().BindLambda([WeakSelf](FHttpRequestPtr, int32 StatusCode)
	{
		if (const TSharedPtr<FCourierSseConnection, ESPMode::ThreadSafe> Self = WeakSelf.Pin())
		{
			if (Self->OnOpen)
			{
				Self->OnOpen(StatusCode);
			}
		}
	});
	Request->OnProcessRequestComplete().BindLambda([WeakSelf](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnected)
	{
		const TSharedPtr<FCourierSseConnection, ESPMode::ThreadSafe> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		const int32 Status = Resp.IsValid() ? Resp->GetResponseCode() : 0;
		FString Error;
		if (!Self->bClosing)
		{
			if (!bConnected || Status == 0)
			{
				Error = FString::Printf(TEXT("No response (%s)"), Req.IsValid() ? LexToString(Req->GetFailureReason()) : TEXT("unknown"));
			}
			else if (!EHttpResponseCodes::IsOk(Status))
			{
				Error = FString::Printf(TEXT("HTTP %d"), Status);
			}
		}
		Self->bOpen = false;
		Self->Request.Reset();
		if (Self->OnClosed)
		{
			Self->OnClosed(Error, Status);
		}
	});

	bOpen = true;
	bClosing = false;
	Request->ProcessRequest();
	return true;
}

void FCourierSseConnection::Close()
{
	if (bOpen && Request.IsValid())
	{
		bClosing = true;
		Request->CancelRequest();
	}
}

void FCourierSseConnection::Dispatch(const TArray<FCourierSseEvent>& Events)
{
	if (!OnEvent)
	{
		return;
	}
	for (const FCourierSseEvent& Event : Events)
	{
		OnEvent(Event);
	}
}

// ---- Blueprint object ----

UCourierEventSource* UCourierEventSource::CreateCourierEventSource(const FString& Url, const TMap<FString, FString>& Headers)
{
	UCourierEventSource* Source = NewObject<UCourierEventSource>();
	Source->Spec.Protocol = ECourierProtocol::ServerSentEvents;
	Source->Spec.Url = Url;
	for (const TPair<FString, FString>& Header : Headers)
	{
		Source->Spec.Headers.Emplace(Header.Key, Header.Value);
	}
	return Source;
}

UCourierEventSource* UCourierEventSource::CreateCourierEventSourceFromRequest(const FCourierRequestSpec& Request, const TMap<FString, FString>& InVariables)
{
	UCourierEventSource* Source = NewObject<UCourierEventSource>();
	Source->Spec = Request;
	Source->Variables = InVariables;
	return Source;
}

bool UCourierEventSource::Open()
{
	if (IsOpen())
	{
		return false;
	}
	FCourierPreparedRequest Prepared;
	FString Error;
	if (!FCourierHttp::Prepare(Spec, Variables, Prepared, Error))
	{
		OnClosed.Broadcast(Error, 0);
		return false;
	}
	Connection = MakeShared<FCourierSseConnection, ESPMode::ThreadSafe>();
	TWeakObjectPtr<UCourierEventSource> WeakSelf(this);
	Connection->OnOpen = [WeakSelf](int32 StatusCode)
	{
		if (UCourierEventSource* Self = WeakSelf.Get())
		{
			Self->OnOpen.Broadcast(StatusCode);
		}
	};
	Connection->OnEvent = [WeakSelf](const FCourierSseEvent& Event)
	{
		if (UCourierEventSource* Self = WeakSelf.Get())
		{
			Self->OnEvent.Broadcast(Event);
		}
	};
	Connection->OnClosed = [WeakSelf](const FString& InError, int32 StatusCode)
	{
		if (UCourierEventSource* Self = WeakSelf.Get())
		{
			Self->OnClosed.Broadcast(InError, StatusCode);
		}
	};
	return Connection->Open(Prepared);
}

void UCourierEventSource::Close()
{
	if (Connection.IsValid())
	{
		Connection->Close();
	}
}

bool UCourierEventSource::IsOpen() const
{
	return Connection.IsValid() && Connection->IsOpen();
}

void UCourierEventSource::BeginDestroy()
{
	if (Connection.IsValid())
	{
		Connection->OnOpen = nullptr;
		Connection->OnEvent = nullptr;
		Connection->OnClosed = nullptr;
		Connection.Reset();
	}
	Super::BeginDestroy();
}
