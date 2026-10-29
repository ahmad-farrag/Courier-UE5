#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"
#include "Interfaces/IHttpRequest.h"

/** A request with every variable resolved, ready for the wire. */
struct COURIER_API FCourierPreparedRequest
{
	FString Verb;
	FString Url;
	TArray<TPair<FString, FString>> Headers;
	TArray<uint8> Body;
	float TimeoutSeconds = 0.f;
};

/**
 * The C++ core shared by the Blueprint nodes and the editor client.
 * All callbacks run on the game thread.
 */
class COURIER_API FCourierHttp
{
public:
	using FOnComplete = TFunction<void(const FCourierResponse&)>;
	using FOnProgress = TFunction<void(uint64 BytesSent, uint64 BytesReceived)>;

	/** Replaces {{name}} from Variables and Postman's {{$dynamic}} values. Unknown names stay as typed. */
	static FString ResolveVariables(const FString& Text, const TMap<FString, FString>& Variables);

	/** Every {{name}} the spec uses (dynamic $ names excluded), in first-use order. */
	static TArray<FString> FindVariableNames(const FCourierRequestSpec& Spec);

	/** Enabled rows as a map; later rows win. */
	static TMap<FString, FString> ToVariableMap(const TArray<FCourierKeyValue>& Rows);

	static FString VerbToString(ECourierVerb Verb, const FString& CustomVerb = FString());
	/** Returns Custom (and sets OutCustomVerb) for anything that is not a standard verb. */
	static ECourierVerb VerbFromString(const FString& VerbString, FString& OutCustomVerb);

	/** Url plus enabled query parameters (and an API key sent in the query), variables resolved. */
	static FString BuildUrl(const FCourierRequestSpec& Spec, const TMap<FString, FString>& Variables);

	/** Builds the final verb, URL, headers and body. False with OutError when a file is missing or JSON is bad. */
	static bool Prepare(const FCourierRequestSpec& Spec, const TMap<FString, FString>& Variables, FCourierPreparedRequest& Out, FString& OutError);

	/**
	 * Sends the request. OnComplete always runs exactly once, on the game thread, also when the request
	 * cannot be built (then the next tick). Cancel through the returned request.
	 */
	static FHttpRequestPtr Send(const FCourierRequestSpec& Spec, const TMap<FString, FString>& Variables, FOnComplete OnComplete, FOnProgress OnProgress = nullptr);

	/** Same as Send, for a request that is already prepared. */
	static FHttpRequestPtr SendPrepared(const FCourierPreparedRequest& Prepared, FOnComplete OnComplete, FOnProgress OnProgress = nullptr);

	/** A cURL command line for the request, variables resolved. */
	static FString ToCurl(const FCourierRequestSpec& Spec, const TMap<FString, FString>& Variables);

	/** RFC 3986 percent-encoding (unreserved characters kept). */
	static FString UrlEncode(const FString& Text);

	/** Splits "https://x/y?a=1&b=2" into the base URL and decoded query rows. */
	static void SplitUrl(const FString& FullUrl, FString& OutBaseUrl, TArray<FCourierKeyValue>& OutQuery);

	/** Default content type for a body type ("" for None). */
	static FString DefaultContentType(ECourierBodyType BodyType);
};
