# Courier

A Postman-style API client inside the Unreal Editor (UE 5.5), plus a Blueprint and C++ API for the same requests at run time.

## The editor window

**Tools > Courier API Client** (also under Window > Tools).

- **Protocols:** HTTP (GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS, custom methods), GraphQL, WebSocket (ws:// and wss://, text and binary messages) and Server-Sent Events (text/event-stream).
- **Request editor:** Params, Headers, Body, Auth and Settings tabs.
  - Body types: none, JSON (with Beautify), raw (any content type), x-www-form-urlencoded, form-data (text and file fields), binary file, and GraphQL (query plus variables).
  - Auth: Bearer token, Basic, and API key (sent as a header or in the query string).
- **Response viewer:** status, time and size. The body is pretty-printed when it is JSON; binary bodies can be saved to a file. Headers have their own tab.
- **WebSocket and SSE console:** connect or disconnect, a log of every message and event with timestamps, and a composer for WebSocket messages.
- **Collections:** requests grouped in folders, with collection variables. You can rename, duplicate, move, delete and filter requests.
- **Environments and globals:** `{{variables}}` work in every field. The order of precedence is globals, then collection variables, then the active environment (later wins, as in Postman). Postman's dynamic variables also work: `{{$guid}}`, `{{$randomUUID}}`, `{{$timestamp}}`, `{{$isoTimestamp}}` and `{{$randomInt}}`.
- **History:** the last 200 requests sent. Double-click one to reopen it.
- **Postman interop:** import collections (v2.0 and v2.1) and environments, and export both. Folder and collection auth is inherited the way Postman does it. WebSocket and SSE requests export as GET items with a `courier` extension field, which Postman ignores and Courier reads back.
- **Copy as cURL** from the request's More menu or the tree's context menu.
- **Keys:** Ctrl+Enter sends (or connects); Ctrl+S saves.

The workspace (collections, environments, history) lives in `Saved/Courier/Workspace.json`, one per machine. Share collections with your team by exporting them.

## Turning a request into a Blueprint node

Use **More > Save as Asset** in the request editor, or create a Courier Request asset from the Content Browser (Miscellaneous). Each asset then shows up in every Blueprint graph's right-click menu as **Courier: \<asset name\>**:

- one string input pin per `{{variable}}` the request uses (an empty pin falls back to the asset's Default Variables);
- an advanced **Variables** map pin for anything else;
- **On Success** (2xx) and **On Failure** (any other status, no response, or cancelled), each with the full **Response**;
- a **Task** output pin you can use to call Cancel.

Double-clicking the asset opens it in the Courier window, and Save writes your changes back to the asset.

## Blueprint API

| Node | What it does |
|---|---|
| Send Courier Request | Async. Sends a full `Courier Request Spec` (struct), with a Variables map. |
| Courier GET / POST JSON / PUT JSON / PATCH JSON / DELETE / POST Form | Async shortcuts. |
| Courier GraphQL | Async. Takes a query plus variables JSON. |
| Send Courier Request With Callback / ...Asset With Callback | Bind a delegate instead (works inside functions). |
| Create Courier WebSocket (...From Request) | Returns an object with Connect, Send Text, Send Binary and Close, and the events On Connected, On Connection Error, On Closed, On Message, On Binary Message and On Message Sent. |
| Create Courier Event Source (...From Request) | Server-Sent Events: Open and Close, with the events On Open, On Event (name, data, id, retry) and On Closed. |
| Get Json String / Number / Bool / Array Length / Keys | Read a response with paths like `data.users[0].name`. |
| Make Json Object, Pretty Print Json, Is Valid Json | |
| Courier Request To Curl, Resolve Courier Variables, Get Courier Response Header | |
| Import / Export Postman Collection, Import / Export Postman Environment | Usable at run time too. |

Keep WebSocket and Event Source objects in a variable: they are garbage collected when nothing references them, and that closes them.

## C++ API

```cpp
#include "CourierHttp.h"

FCourierRequestSpec Spec;
Spec.Verb = ECourierVerb::Post;
Spec.Url = TEXT("{{base}}/scores");
Spec.BodyType = ECourierBodyType::Json;
Spec.Body = TEXT("{\"player\":\"{{player}}\",\"score\":42}");

FCourierHttp::Send(Spec, { { TEXT("base"), TEXT("https://api.example.com") }, { TEXT("player"), TEXT("Ada") } },
	[](const FCourierResponse& Response)
	{
		UE_LOG(LogTemp, Log, TEXT("%d %s"), Response.StatusCode, *Response.Body);
	});
```

`FCourierSseConnection` and `UCourierWebSocket::MakeSocket` give the same streams without UObjects. `FCourierPostman` reads and writes Postman JSON.

## Python and editor scripts

The async nodes are Blueprint-only, so scripts use **Create Courier Request**. It returns the task without sending it: bind `on_success` and `on_failure`, then call `send()`. WebSocket and Event Source objects work the same way from Python.

Known UE 5.5 limit: binding a Python callable to a delegate whose parameter is a **struct** (the response, or an SSE event) crashes inside the engine's Python bridge (`PyGenUtil::InvokePythonCallableFromUnrealFunctionThunk`). Delegates with string or int parameters work from Python, for example the WebSocket events. Blueprint events and C++ `AddDynamic` / `BindUFunction` are not affected, and the `Courier.Live.*` tests cover them.

Console commands: `Courier.Open` opens the tab; `Courier.Send` sends or connects the open request.

## Not supported (yet)

Postman's pre-request and test scripts, gRPC, MQTT, Socket.IO, OAuth 2.0 / Digest / AWS signature auth, cookie management, and mock servers.

## Tests

Automation tests are under `Courier.*` (Session Frontend > Automation, or `Automation RunTests Courier`). They cover variable resolution, request building (form, multipart, GraphQL, auth), cURL, JSON paths, the Postman round trip, and the SSE parser. `Courier.Editor.RequestNode` builds a Blueprint with a `Courier: <asset>` node, compiles it, and checks that the variable pins survive a reconstruct.

`Courier.Live.Http` and `Courier.Live.ServerSentEvents` send real requests to postman-echo.com and stream.wikimedia.org. They are in the Stress filter, so run them by name: `Automation SetFilter Stress; RunTests Courier.Live`.
