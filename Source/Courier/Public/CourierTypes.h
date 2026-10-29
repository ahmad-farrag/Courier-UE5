#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.generated.h"

COURIER_API DECLARE_LOG_CATEGORY_EXTERN(LogCourier, Log, All);

/** What a request talks over. HTTP covers REST and GraphQL; the other two keep a connection open. */
UENUM(BlueprintType)
enum class ECourierProtocol : uint8
{
	Http				UMETA(DisplayName = "HTTP"),
	WebSocket			UMETA(DisplayName = "WebSocket"),
	ServerSentEvents	UMETA(DisplayName = "Server-Sent Events")
};

/** HTTP method. Custom sends CustomVerb as is. (Not GET/DELETE: winnt.h defines DELETE as a macro.) */
UENUM(BlueprintType)
enum class ECourierVerb : uint8
{
	Get		UMETA(DisplayName = "GET"),
	Post	UMETA(DisplayName = "POST"),
	Put		UMETA(DisplayName = "PUT"),
	Patch	UMETA(DisplayName = "PATCH"),
	Delete	UMETA(DisplayName = "DELETE"),
	Head	UMETA(DisplayName = "HEAD"),
	Options	UMETA(DisplayName = "OPTIONS"),
	Custom	UMETA(DisplayName = "Custom")
};

UENUM(BlueprintType)
enum class ECourierBodyType : uint8
{
	None,
	/** Body text sent with RawContentType. */
	Raw,
	/** Body text sent as application/json. */
	Json			UMETA(DisplayName = "JSON"),
	/** FormFields sent as application/x-www-form-urlencoded. */
	FormUrlEncoded	UMETA(DisplayName = "x-www-form-urlencoded"),
	/** FormFields sent as multipart/form-data; file fields read the file at Value. */
	Multipart		UMETA(DisplayName = "form-data"),
	/** The file at Body sent as application/octet-stream. */
	Binary			UMETA(DisplayName = "Binary File"),
	/** GraphQLQuery and GraphQLVariables sent as a JSON POST body. */
	GraphQL			UMETA(DisplayName = "GraphQL")
};

UENUM(BlueprintType)
enum class ECourierAuthType : uint8
{
	None,
	Bearer	UMETA(DisplayName = "Bearer Token"),
	Basic	UMETA(DisplayName = "Basic Auth"),
	ApiKey	UMETA(DisplayName = "API Key")
};

/** A header, query parameter or variable. Disabled rows are kept but not sent. */
USTRUCT(BlueprintType)
struct COURIER_API FCourierKeyValue
{
	GENERATED_BODY()

	FCourierKeyValue() = default;
	FCourierKeyValue(const FString& InKey, const FString& InValue, bool bInEnabled = true)
		: Key(InKey), Value(InValue), bEnabled(bInEnabled) {}

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	FString Key;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	FString Value;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	bool bEnabled = true;

	bool operator==(const FCourierKeyValue& Other) const
	{
		return Key == Other.Key && Value == Other.Value && bEnabled == Other.bEnabled;
	}
};

/** One field of a form body. A file field sends the file whose path is in Value. */
USTRUCT(BlueprintType)
struct COURIER_API FCourierFormField
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	FString Key;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	FString Value;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	bool bIsFile = false;

	/** Content type of a multipart part; empty means text/plain for text and application/octet-stream for files. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	FString ContentType;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	bool bEnabled = true;
};

USTRUCT(BlueprintType)
struct COURIER_API FCourierAuth
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	ECourierAuthType Type = ECourierAuthType::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier", meta = (EditCondition = "Type == ECourierAuthType::Bearer", EditConditionHides))
	FString Token;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier", meta = (EditCondition = "Type == ECourierAuthType::Basic", EditConditionHides))
	FString Username;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier", meta = (EditCondition = "Type == ECourierAuthType::Basic", EditConditionHides))
	FString Password;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier", meta = (EditCondition = "Type == ECourierAuthType::ApiKey", EditConditionHides))
	FString ApiKeyName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier", meta = (EditCondition = "Type == ECourierAuthType::ApiKey", EditConditionHides))
	FString ApiKeyValue;

	/** Send the API key as a query parameter instead of a header. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier", meta = (EditCondition = "Type == ECourierAuthType::ApiKey", EditConditionHides))
	bool bApiKeyInQuery = false;
};

/**
 * Everything needed to send one request. Any text field may hold {{variables}}, resolved at send time
 * from the variables passed in (environment, collection, Blueprint pins), plus Postman's dynamic
 * variables {{$guid}}, {{$randomUUID}}, {{$timestamp}}, {{$isoTimestamp}} and {{$randomInt}}.
 */
USTRUCT(BlueprintType)
struct COURIER_API FCourierRequestSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	FString Name = TEXT("New Request");

	/** Folder inside its collection, "Users/Admin" style. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	FString Folder;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier", meta = (MultiLine = true))
	FString Description;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	ECourierProtocol Protocol = ECourierProtocol::Http;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	ECourierVerb Verb = ECourierVerb::Get;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier", meta = (EditCondition = "Verb == ECourierVerb::Custom", EditConditionHides))
	FString CustomVerb;

	/** Base URL without the query string; QueryParams are appended. A query string typed here is kept. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	FString Url;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	TArray<FCourierKeyValue> QueryParams;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	TArray<FCourierKeyValue> Headers;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier|Body")
	ECourierBodyType BodyType = ECourierBodyType::None;

	/** Raw or JSON text, or the file path for a Binary body. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier|Body", meta = (MultiLine = true))
	FString Body;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier|Body")
	FString RawContentType = TEXT("text/plain");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier|Body")
	TArray<FCourierFormField> FormFields;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier|Body", meta = (MultiLine = true))
	FString GraphQLQuery;

	/** JSON object text. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier|Body", meta = (MultiLine = true))
	FString GraphQLVariables;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	FCourierAuth Auth;

	/** Seconds; 0 or less uses the engine default. Ignored by WebSocket and SSE. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	float TimeoutSeconds = 30.f;

	/** Sub-protocols for a WebSocket handshake. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	TArray<FString> WebSocketProtocols;

	/** Identity inside the editor's workspace. */
	UPROPERTY(EditAnywhere, Category = "Courier", AdvancedDisplay)
	FGuid Id;
};

USTRUCT(BlueprintType)
struct COURIER_API FCourierResponse
{
	GENERATED_BODY()

	/** The server answered with a 2xx status. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	bool bSuccess = false;

	/** The server answered at all (any status). False means a connection, DNS, TLS or timeout failure. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	bool bReceivedResponse = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	int32 StatusCode = 0;

	/** The body decoded as UTF-8. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	FString Body;

	/** The body bytes as received (images, binary payloads). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	TArray<uint8> Content;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	TMap<FString, FString> Headers;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	FString ContentType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	float ElapsedSeconds = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	int64 SizeBytes = 0;

	/** Why the request failed before a response arrived, or why it could not be built. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	FString Error;

	/** The final URL, after variables and query parameters. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	FString Url;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	FString Verb;
};

/** A named set of requests with its own variables, like a Postman collection. */
USTRUCT(BlueprintType)
struct COURIER_API FCourierCollection
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	FString Name = TEXT("New Collection");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier", meta = (MultiLine = true))
	FString Description;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	TArray<FCourierKeyValue> Variables;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	TArray<FCourierRequestSpec> Requests;

	/** Empty folders, which no request names yet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	TArray<FString> Folders;

	UPROPERTY(EditAnywhere, Category = "Courier", AdvancedDisplay)
	FGuid Id;
};

/** A named set of variables (dev, staging, production), like a Postman environment. */
USTRUCT(BlueprintType)
struct COURIER_API FCourierEnvironment
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	FString Name = TEXT("New Environment");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Courier")
	TArray<FCourierKeyValue> Variables;

	UPROPERTY(EditAnywhere, Category = "Courier", AdvancedDisplay)
	FGuid Id;
};

/** One Server-Sent Events message. */
USTRUCT(BlueprintType)
struct COURIER_API FCourierSseEvent
{
	GENERATED_BODY()

	/** The "event:" field; "message" when the server sent none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	FString EventName = TEXT("message");

	/** The "data:" lines joined with newlines. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	FString Data;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	FString Id;

	/** The "retry:" field in milliseconds, or -1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Courier")
	int32 Retry = -1;
};

DECLARE_DYNAMIC_DELEGATE_OneParam(FCourierResponseDelegate, const FCourierResponse&, Response);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCourierResponseMulticastDelegate, const FCourierResponse&, Response);
