# C++ reference

Add `"Courier"` to your module's dependencies:

```csharp
PublicDependencyModuleNames.Add("Courier");
```

Everything here works in packaged games (the `Courier` module is Runtime). Callbacks run on the game thread.

## Types (`CourierTypes.h`)

| Type | Purpose |
|---|---|
| `FCourierRequestSpec` | One request: protocol, verb, URL, params, headers, body, auth, timeout, WebSocket sub-protocols |
| `FCourierResponse` | Result of an HTTP request (see the [Blueprint reference](blueprint-reference.md#courier-response) for fields) |
| `FCourierKeyValue` | `Key`, `Value`, `bEnabled`; used for params, headers and variables |
| `FCourierFormField` | Form row; `bIsFile` makes `Value` a file path |
| `FCourierAuth` | `Type` (None, Bearer, Basic, ApiKey), `Token`, `Username`, `Password`, `ApiKeyName`, `ApiKeyValue`, `bApiKeyInQuery` |
| `FCourierCollection` | `Name`, `Description`, `Variables`, `Requests`, `Folders` |
| `FCourierEnvironment` | `Name`, `Variables` |
| `FCourierSseEvent` | `EventName`, `Data`, `Id`, `Retry` |
| `ECourierProtocol` | `Http`, `WebSocket`, `ServerSentEvents` |
| `ECourierVerb` | `Get`, `Post`, `Put`, `Patch`, `Delete`, `Head`, `Options`, `Custom` |
| `ECourierBodyType` | `None`, `Raw`, `Json`, `FormUrlEncoded`, `Multipart`, `Binary`, `GraphQL` |

## `FCourierHttp` (`CourierHttp.h`)

The core used by the editor window, the async nodes and the Blueprint node.

```cpp
using FOnComplete = TFunction<void(const FCourierResponse&)>;
using FOnProgress = TFunction<void(uint64 BytesSent, uint64 BytesReceived)>;

static FHttpRequestPtr Send(const FCourierRequestSpec& Spec, const TMap<FString, FString>& Variables,
                            FOnComplete OnComplete, FOnProgress OnProgress = nullptr);
```

`OnComplete` runs exactly once, also when the request cannot be built (a missing upload file, invalid GraphQL variables); in that case it runs on the next tick with `Error` set. Cancel with `Request->CancelRequest()` on the returned pointer.

| Function | What it does |
|---|---|
| `ResolveVariables(Text, Variables)` | Replaces `{{name}}` and dynamic `{{$...}}` values; unknown names stay as typed |
| `FindVariableNames(Spec)` | Every `{{name}}` the spec uses (no `$` names), in first-use order |
| `ToVariableMap(Rows)` | Enabled rows as a map; later rows win |
| `BuildUrl(Spec, Variables)` | URL plus enabled query rows (and an API key in the query), resolved. Adds `http://` (or `ws://`) when no scheme is given |
| `Prepare(Spec, Variables, Out, OutError)` | Final verb, URL, headers and body bytes in an `FCourierPreparedRequest` |
| `SendPrepared(Prepared, OnComplete, OnProgress)` | Sends an already prepared request |
| `ToCurl(Spec, Variables)` | A cURL command line |
| `UrlEncode(Text)` | RFC 3986 percent-encoding of UTF-8 |
| `SplitUrl(FullUrl, OutBase, OutQuery)` | Splits a URL into base and decoded query rows |
| `VerbToString`, `VerbFromString`, `DefaultContentType` | Small helpers |

### Example: GraphQL with a bearer token

```cpp
FCourierRequestSpec Spec;
Spec.Verb = ECourierVerb::Post;
Spec.Url = TEXT("{{api}}/graphql");
Spec.BodyType = ECourierBodyType::GraphQL;
Spec.GraphQLQuery = TEXT("query($id: ID!) { player(id: $id) { name rank } }");
Spec.GraphQLVariables = TEXT("{\"id\": \"{{playerId}}\"}");
Spec.Auth.Type = ECourierAuthType::Bearer;
Spec.Auth.Token = TEXT("{{token}}");

const TMap<FString, FString> Vars = { { TEXT("api"), ApiBase }, { TEXT("playerId"), PlayerId }, { TEXT("token"), SessionToken } };
FCourierHttp::Send(Spec, Vars, [WeakThis = TWeakObjectPtr<UMyComponent>(this)](const FCourierResponse& Response)
{
	if (WeakThis.IsValid() && Response.bSuccess)
	{
		WeakThis->OnPlayerLoaded(Response.Body);
	}
});
```

Capture a weak pointer: the callback can arrive after the object that started the request is gone.

## Blueprint-facing classes from C++

| Class | Use from C++ |
|---|---|
| `UCourierSendRequest` | `UCourierLibrary::SendCourierRequestWithCallback(...)` with an `FCourierResponseDelegate`, or `UCourierLibrary::CreateCourierRequest(...)`, bind `OnSuccess` / `OnFailure` with `AddDynamic`, then `Send()` |
| `UCourierRequestAsset` | `Request`, `DefaultVariables`, `GetVariableNames()`; send with `UCourierSendRequest::SendRequestAsset` or `UCourierLibrary::SendCourierRequestAssetWithCallback` |
| `UCourierWebSocket` | `CreateCourierWebSocket(Url, Protocols, Headers)`, bind the dynamic delegates, `Connect()`. Hold it in a `UPROPERTY` |
| `UCourierEventSource` | `CreateCourierEventSource(Url, Headers)`, bind, `Open()`. Hold it in a `UPROPERTY` |

A `UCourierSendRequest` keeps itself alive until it finishes (registered with the game instance, or rooted when there is none, as in editor utilities).

## Streams without UObjects

```cpp
FCourierPreparedRequest Prepared;
FString Error;
if (FCourierHttp::Prepare(Spec, Vars, Prepared, Error))
{
	Sse = MakeShared<FCourierSseConnection, ESPMode::ThreadSafe>();   // keep this alive
	Sse->OnEvent = [](const FCourierSseEvent& Event) { UE_LOG(LogTemp, Log, TEXT("%s: %s"), *Event.EventName, *Event.Data); };
	Sse->OnClosed = [](const FString& Err, int32 Status) { /* Err is empty after a clean end or Close() */ };
	Sse->Open(Prepared);
}
```

`FCourierSseParser` is the parser on its own: `Feed(Bytes, Length, OutEvents)` accepts any chunking and keeps `LastEventId`.

For WebSockets, `UCourierWebSocket::MakeSocket(Spec, Vars, OutUrl)` returns the engine's `TSharedPtr<IWebSocket>` with headers and sub-protocols applied.

## Postman (`CourierPostman.h`)

```cpp
FCourierCollection Collection;
FString Error;
if (FCourierPostman::ImportCollection(JsonText, Collection, Error)) { ... }
FString Json = FCourierPostman::ExportCollection(Collection);
```

`ImportEnvironment`, `ExportEnvironment` and `IsEnvironmentJson` work the same way. See [Postman compatibility](postman-compatibility.md).
