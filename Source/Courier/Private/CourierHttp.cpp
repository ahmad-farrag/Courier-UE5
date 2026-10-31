#include "CourierHttp.h"

#include "Async/Async.h"
#include "CourierCookies.h"
#include "CourierOAuth2.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace CourierHttp
{
	FString ResolveDynamic(const FString& Name, bool& bOutFound)
	{
		bOutFound = true;
		if (Name == TEXT("$guid") || Name == TEXT("$randomUUID"))
		{
			return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		}
		if (Name == TEXT("$timestamp"))
		{
			return FString::Printf(TEXT("%lld"), FDateTime::UtcNow().ToUnixTimestamp());
		}
		if (Name == TEXT("$isoTimestamp"))
		{
			return FDateTime::UtcNow().ToIso8601();
		}
		if (Name == TEXT("$randomInt"))
		{
			return FString::FromInt(FMath::RandRange(0, 1000));
		}
		bOutFound = false;
		return FString();
	}

	/** One left-to-right pass. Returns true when anything was replaced. */
	bool ResolveOnce(const FString& In, const TMap<FString, FString>& Variables, FString& Out)
	{
		Out.Reset(In.Len());
		bool bChanged = false;
		int32 Index = 0;
		while (Index < In.Len())
		{
			const int32 Open = In.Find(TEXT("{{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index);
			if (Open == INDEX_NONE)
			{
				Out.Append(*In + Index, In.Len() - Index);
				break;
			}
			const int32 Close = In.Find(TEXT("}}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 2);
			if (Close == INDEX_NONE)
			{
				Out.Append(*In + Index, In.Len() - Index);
				break;
			}
			Out.Append(*In + Index, Open - Index);
			const FString Name = In.Mid(Open + 2, Close - Open - 2).TrimStartAndEnd();
			bool bFound = false;
			FString Value;
			if (Name.StartsWith(TEXT("$")))
			{
				Value = ResolveDynamic(Name, bFound);
			}
			else if (const FString* Found = Variables.Find(Name))
			{
				Value = *Found;
				bFound = true;
			}
			if (bFound)
			{
				Out.Append(Value);
				bChanged = true;
			}
			else
			{
				Out.Append(*In + Open, Close + 2 - Open);
			}
			Index = Close + 2;
		}
		return bChanged;
	}

	void CollectNames(const FString& Text, TArray<FString>& Out)
	{
		int32 Index = 0;
		while (true)
		{
			const int32 Open = Text.Find(TEXT("{{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index);
			if (Open == INDEX_NONE)
			{
				return;
			}
			const int32 Close = Text.Find(TEXT("}}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 2);
			if (Close == INDEX_NONE)
			{
				return;
			}
			const FString Name = Text.Mid(Open + 2, Close - Open - 2).TrimStartAndEnd();
			if (!Name.IsEmpty() && !Name.StartsWith(TEXT("$")))
			{
				Out.AddUnique(Name);
			}
			Index = Close + 2;
		}
	}

	void AppendUtf8(TArray<uint8>& Out, const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	FString Base64Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	bool HasHeader(const TArray<TPair<FString, FString>>& Headers, const FString& Name)
	{
		return Headers.ContainsByPredicate([&Name](const TPair<FString, FString>& Header) { return Header.Key.Equals(Name, ESearchCase::IgnoreCase); });
	}

	FString ShellQuote(const FString& Text)
	{
		return TEXT("'") + Text.Replace(TEXT("'"), TEXT("'\\''")) + TEXT("'");
	}

	FCourierResponse MakeResponse(const FHttpRequestPtr& Request, const FHttpResponsePtr& Response, bool bConnected, double StartTime)
	{
		FCourierResponse Out;
		Out.ElapsedSeconds = static_cast<float>(FPlatformTime::Seconds() - StartTime);
		if (Request.IsValid())
		{
			Out.Url = Request->GetURL();
			Out.Verb = Request->GetVerb();
		}
		if (Response.IsValid() && Response->GetResponseCode() > 0)
		{
			Out.bReceivedResponse = true;
			Out.StatusCode = Response->GetResponseCode();
			Out.Content = Response->GetContent();
			Out.SizeBytes = Out.Content.Num();
			Out.Body = Response->GetContentAsString();
			Out.ContentType = Response->GetContentType();
			for (const FString& Line : Response->GetAllHeaders())
			{
				FString Key, Value;
				if (Line.Split(TEXT(":"), &Key, &Value))
				{
					Key.TrimStartAndEndInline();
					Value.TrimStartAndEndInline();
					if (FString* Existing = Out.Headers.Find(Key))
					{
						*Existing += TEXT(", ") + Value;
					}
					else
					{
						Out.Headers.Add(Key, Value);
					}
				}
			}
			Out.bSuccess = EHttpResponseCodes::IsOk(Out.StatusCode);
			if (!Out.bSuccess)
			{
				Out.Error = FString::Printf(TEXT("HTTP %d"), Out.StatusCode);
			}
		}
		else
		{
			Out.Error = Request.IsValid()
				? FString::Printf(TEXT("No response (%s)"), LexToString(Request->GetFailureReason()))
				: FString(TEXT("No response"));
		}
		return Out;
	}
}

FString FCourierHttp::ResolveVariables(const FString& Text, const TMap<FString, FString>& Variables)
{
	if (!Text.Contains(TEXT("{{")))
	{
		return Text;
	}
	// A few passes so a variable may hold another {{variable}}; unknown names stay, which stops the loop.
	FString Current = Text;
	FString Next;
	for (int32 Pass = 0; Pass < 4 && Current.Contains(TEXT("{{")); ++Pass)
	{
		if (!CourierHttp::ResolveOnce(Current, Variables, Next))
		{
			break;
		}
		Swap(Current, Next);
	}
	return Current;
}

TArray<FString> FCourierHttp::FindVariableNames(const FCourierRequestSpec& Spec)
{
	TArray<FString> Names;
	CourierHttp::CollectNames(Spec.Url, Names);
	for (const FCourierKeyValue& Row : Spec.QueryParams)
	{
		if (Row.bEnabled) { CourierHttp::CollectNames(Row.Key, Names); CourierHttp::CollectNames(Row.Value, Names); }
	}
	for (const FCourierKeyValue& Row : Spec.Headers)
	{
		if (Row.bEnabled) { CourierHttp::CollectNames(Row.Key, Names); CourierHttp::CollectNames(Row.Value, Names); }
	}
	switch (Spec.BodyType)
	{
	case ECourierBodyType::Raw:
	case ECourierBodyType::Json:
	case ECourierBodyType::Binary:
		CourierHttp::CollectNames(Spec.Body, Names);
		break;
	case ECourierBodyType::FormUrlEncoded:
	case ECourierBodyType::Multipart:
		for (const FCourierFormField& Field : Spec.FormFields)
		{
			if (Field.bEnabled) { CourierHttp::CollectNames(Field.Key, Names); CourierHttp::CollectNames(Field.Value, Names); }
		}
		break;
	case ECourierBodyType::GraphQL:
		CourierHttp::CollectNames(Spec.GraphQLQuery, Names);
		CourierHttp::CollectNames(Spec.GraphQLVariables, Names);
		break;
	default:
		break;
	}
	switch (Spec.Auth.Type)
	{
	case ECourierAuthType::Bearer:
		CourierHttp::CollectNames(Spec.Auth.Token, Names);
		break;
	case ECourierAuthType::Basic:
		CourierHttp::CollectNames(Spec.Auth.Username, Names);
		CourierHttp::CollectNames(Spec.Auth.Password, Names);
		break;
	case ECourierAuthType::ApiKey:
		CourierHttp::CollectNames(Spec.Auth.ApiKeyName, Names);
		CourierHttp::CollectNames(Spec.Auth.ApiKeyValue, Names);
		break;
	case ECourierAuthType::OAuth2:
	{
		const FCourierOAuth2& OAuth2 = Spec.Auth.OAuth2;
		for (const FString* Field : { &OAuth2.AccessTokenUrl, &OAuth2.AuthUrl, &OAuth2.ClientId, &OAuth2.ClientSecret, &OAuth2.Scope,
			&OAuth2.Audience, &OAuth2.Username, &OAuth2.Password, &OAuth2.RedirectUri, &OAuth2.AccessToken })
		{
			CourierHttp::CollectNames(*Field, Names);
		}
		break;
	}
	default:
		break;
	}
	if (Spec.Protocol == ECourierProtocol::SocketIO)
	{
		CourierHttp::CollectNames(Spec.SocketIONamespace, Names);
		CourierHttp::CollectNames(Spec.SocketIOAuth, Names);
	}
	return Names;
}

TMap<FString, FString> FCourierHttp::ToVariableMap(const TArray<FCourierKeyValue>& Rows)
{
	TMap<FString, FString> Map;
	for (const FCourierKeyValue& Row : Rows)
	{
		if (Row.bEnabled && !Row.Key.IsEmpty())
		{
			Map.Add(Row.Key, Row.Value);
		}
	}
	return Map;
}

FString FCourierHttp::VerbToString(ECourierVerb Verb, const FString& CustomVerb)
{
	switch (Verb)
	{
	case ECourierVerb::Get:		return TEXT("GET");
	case ECourierVerb::Post:	return TEXT("POST");
	case ECourierVerb::Put:		return TEXT("PUT");
	case ECourierVerb::Patch:	return TEXT("PATCH");
	case ECourierVerb::Delete:	return TEXT("DELETE");
	case ECourierVerb::Head:	return TEXT("HEAD");
	case ECourierVerb::Options:	return TEXT("OPTIONS");
	default:					return CustomVerb.IsEmpty() ? FString(TEXT("GET")) : CustomVerb.ToUpper();
	}
}

ECourierVerb FCourierHttp::VerbFromString(const FString& VerbString, FString& OutCustomVerb)
{
	const FString Upper = VerbString.TrimStartAndEnd().ToUpper();
	OutCustomVerb.Reset();
	if (Upper.IsEmpty() || Upper == TEXT("GET"))	return ECourierVerb::Get;
	if (Upper == TEXT("POST"))						return ECourierVerb::Post;
	if (Upper == TEXT("PUT"))						return ECourierVerb::Put;
	if (Upper == TEXT("PATCH"))						return ECourierVerb::Patch;
	if (Upper == TEXT("DELETE"))					return ECourierVerb::Delete;
	if (Upper == TEXT("HEAD"))						return ECourierVerb::Head;
	if (Upper == TEXT("OPTIONS"))					return ECourierVerb::Options;
	OutCustomVerb = Upper;
	return ECourierVerb::Custom;
}

FString FCourierHttp::UrlEncode(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text);
	FString Out;
	Out.Reserve(Utf8.Length() * 3);
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		const uint8 Byte = static_cast<uint8>(Utf8.Get()[Index]);
		// ASCII only: FChar::IsAlnum would call UTF-8 lead bytes such as 0xC3 letters.
		const bool bAlnum = (Byte >= 'A' && Byte <= 'Z') || (Byte >= 'a' && Byte <= 'z') || (Byte >= '0' && Byte <= '9');
		if (bAlnum || Byte == '-' || Byte == '_' || Byte == '.' || Byte == '~')
		{
			Out.AppendChar(static_cast<TCHAR>(Byte));
		}
		else
		{
			Out += FString::Printf(TEXT("%%%02X"), Byte);
		}
	}
	return Out;
}

void FCourierHttp::SplitUrl(const FString& FullUrl, FString& OutBaseUrl, TArray<FCourierKeyValue>& OutQuery)
{
	OutQuery.Reset();
	FString Query;
	if (!FullUrl.Split(TEXT("?"), &OutBaseUrl, &Query))
	{
		OutBaseUrl = FullUrl;
		return;
	}
	// A fragment never reaches the server.
	Query.Split(TEXT("#"), &Query, nullptr);
	TArray<FString> Pairs;
	Query.ParseIntoArray(Pairs, TEXT("&"));
	for (const FString& Pair : Pairs)
	{
		FString Key, Value;
		if (!Pair.Split(TEXT("="), &Key, &Value))
		{
			Key = Pair;
		}
		// Keep {{variables}} readable: decoding leaves them untouched since braces are not encoded by people typing.
		OutQuery.Emplace(FGenericPlatformHttp::UrlDecode(Key), FGenericPlatformHttp::UrlDecode(Value));
	}
}

FString FCourierHttp::BuildUrl(const FCourierRequestSpec& Spec, const TMap<FString, FString>& Variables)
{
	FString Url = ResolveVariables(Spec.Url, Variables).TrimStartAndEnd();
	if (!Url.IsEmpty() && !Url.Contains(TEXT("://")))
	{
		// Postman assumes http:// for a bare host; so do we, except for the stream protocols.
		Url = (Spec.Protocol == ECourierProtocol::WebSocket ? TEXT("ws://") : TEXT("http://")) + Url;
	}

	TArray<FString> Parts;
	for (const FCourierKeyValue& Row : Spec.QueryParams)
	{
		if (Row.bEnabled && !Row.Key.IsEmpty())
		{
			Parts.Add(UrlEncode(ResolveVariables(Row.Key, Variables)) + TEXT("=") + UrlEncode(ResolveVariables(Row.Value, Variables)));
		}
	}
	if (Spec.Auth.Type == ECourierAuthType::ApiKey && Spec.Auth.bApiKeyInQuery && !Spec.Auth.ApiKeyName.IsEmpty())
	{
		Parts.Add(UrlEncode(ResolveVariables(Spec.Auth.ApiKeyName, Variables)) + TEXT("=") + UrlEncode(ResolveVariables(Spec.Auth.ApiKeyValue, Variables)));
	}
	if (Parts.Num() > 0)
	{
		Url += (Url.Contains(TEXT("?")) ? TEXT("&") : TEXT("?")) + FString::Join(Parts, TEXT("&"));
	}
	return Url;
}

FString FCourierHttp::DefaultContentType(ECourierBodyType BodyType)
{
	switch (BodyType)
	{
	case ECourierBodyType::Json:
	case ECourierBodyType::GraphQL:			return TEXT("application/json");
	case ECourierBodyType::FormUrlEncoded:	return TEXT("application/x-www-form-urlencoded");
	case ECourierBodyType::Multipart:		return TEXT("multipart/form-data");
	case ECourierBodyType::Binary:			return TEXT("application/octet-stream");
	case ECourierBodyType::Raw:				return TEXT("text/plain");
	default:								return FString();
	}
}

bool FCourierHttp::Prepare(const FCourierRequestSpec& Spec, const TMap<FString, FString>& Variables, FCourierPreparedRequest& Out, FString& OutError)
{
	Out = FCourierPreparedRequest();
	Out.Verb = VerbToString(Spec.Verb, Spec.CustomVerb);
	Out.Url = BuildUrl(Spec, Variables);
	Out.TimeoutSeconds = Spec.TimeoutSeconds;
	if (Out.Url.IsEmpty())
	{
		OutError = TEXT("The URL is empty.");
		return false;
	}

	for (const FCourierKeyValue& Row : Spec.Headers)
	{
		if (Row.bEnabled && !Row.Key.IsEmpty())
		{
			Out.Headers.Emplace(ResolveVariables(Row.Key, Variables), ResolveVariables(Row.Value, Variables));
		}
	}

	switch (Spec.Auth.Type)
	{
	case ECourierAuthType::Bearer:
		if (!CourierHttp::HasHeader(Out.Headers, TEXT("Authorization")))
		{
			Out.Headers.Emplace(TEXT("Authorization"), TEXT("Bearer ") + ResolveVariables(Spec.Auth.Token, Variables));
		}
		break;
	case ECourierAuthType::Basic:
		if (!CourierHttp::HasHeader(Out.Headers, TEXT("Authorization")))
		{
			const FString Pair = ResolveVariables(Spec.Auth.Username, Variables) + TEXT(":") + ResolveVariables(Spec.Auth.Password, Variables);
			Out.Headers.Emplace(TEXT("Authorization"), TEXT("Basic ") + CourierHttp::Base64Utf8(Pair));
		}
		break;
	case ECourierAuthType::ApiKey:
		if (!Spec.Auth.bApiKeyInQuery && !Spec.Auth.ApiKeyName.IsEmpty())
		{
			Out.Headers.Emplace(ResolveVariables(Spec.Auth.ApiKeyName, Variables), ResolveVariables(Spec.Auth.ApiKeyValue, Variables));
		}
		break;
	case ECourierAuthType::OAuth2:
		if (!CourierHttp::HasHeader(Out.Headers, TEXT("Authorization")))
		{
			// The token was fetched (or given) before Prepare; Send takes care of that.
			const FCourierOAuth2 OAuth2 = FCourierOAuth2Client::Resolve(Spec.Auth.OAuth2, Variables);
			const FString Token = FCourierOAuth2Client::FindUsableToken(OAuth2, false);
			if (!Token.IsEmpty())
			{
				Out.Headers.Emplace(TEXT("Authorization"), OAuth2.HeaderPrefix.IsEmpty() ? Token : OAuth2.HeaderPrefix + TEXT(" ") + Token);
			}
		}
		break;
	default:
		break;
	}

	Out.bUseCookieJar = Spec.bUseCookieJar;
	if (Spec.bUseCookieJar && !CourierHttp::HasHeader(Out.Headers, TEXT("Cookie")))
	{
		const FString Cookies = FCourierCookieJar::Get().GetCookieHeader(Out.Url);
		if (!Cookies.IsEmpty())
		{
			Out.Headers.Emplace(TEXT("Cookie"), Cookies);
		}
	}

	FString ContentType = DefaultContentType(Spec.BodyType);
	switch (Spec.BodyType)
	{
	case ECourierBodyType::Raw:
		ContentType = Spec.RawContentType.IsEmpty() ? ContentType : Spec.RawContentType;
		CourierHttp::AppendUtf8(Out.Body, ResolveVariables(Spec.Body, Variables));
		break;

	case ECourierBodyType::Json:
		CourierHttp::AppendUtf8(Out.Body, ResolveVariables(Spec.Body, Variables));
		break;

	case ECourierBodyType::FormUrlEncoded:
	{
		TArray<FString> Parts;
		for (const FCourierFormField& Field : Spec.FormFields)
		{
			if (Field.bEnabled && !Field.Key.IsEmpty())
			{
				Parts.Add(UrlEncode(ResolveVariables(Field.Key, Variables)) + TEXT("=") + UrlEncode(ResolveVariables(Field.Value, Variables)));
			}
		}
		CourierHttp::AppendUtf8(Out.Body, FString::Join(Parts, TEXT("&")));
		break;
	}

	case ECourierBodyType::Multipart:
	{
		const FString Boundary = TEXT("----CourierBoundary") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		ContentType = TEXT("multipart/form-data; boundary=") + Boundary;
		for (const FCourierFormField& Field : Spec.FormFields)
		{
			if (!Field.bEnabled || Field.Key.IsEmpty())
			{
				continue;
			}
			const FString Key = ResolveVariables(Field.Key, Variables);
			const FString Value = ResolveVariables(Field.Value, Variables);
			CourierHttp::AppendUtf8(Out.Body, TEXT("--") + Boundary + TEXT("\r\n"));
			if (Field.bIsFile)
			{
				TArray<uint8> FileBytes;
				if (!FFileHelper::LoadFileToArray(FileBytes, *Value))
				{
					OutError = FString::Printf(TEXT("Cannot read the file for form field '%s': %s"), *Key, *Value);
					return false;
				}
				const FString PartType = Field.ContentType.IsEmpty() ? FString(TEXT("application/octet-stream")) : Field.ContentType;
				CourierHttp::AppendUtf8(Out.Body, FString::Printf(TEXT("Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: %s\r\n\r\n"),
					*Key, *FPaths::GetCleanFilename(Value), *PartType));
				Out.Body.Append(FileBytes);
			}
			else
			{
				CourierHttp::AppendUtf8(Out.Body, FString::Printf(TEXT("Content-Disposition: form-data; name=\"%s\"\r\n"), *Key));
				if (!Field.ContentType.IsEmpty())
				{
					CourierHttp::AppendUtf8(Out.Body, FString::Printf(TEXT("Content-Type: %s\r\n"), *Field.ContentType));
				}
				CourierHttp::AppendUtf8(Out.Body, TEXT("\r\n") + Value);
			}
			CourierHttp::AppendUtf8(Out.Body, TEXT("\r\n"));
		}
		CourierHttp::AppendUtf8(Out.Body, TEXT("--") + Boundary + TEXT("--\r\n"));
		break;
	}

	case ECourierBodyType::Binary:
	{
		const FString Path = ResolveVariables(Spec.Body, Variables);
		if (!FFileHelper::LoadFileToArray(Out.Body, *Path))
		{
			OutError = FString::Printf(TEXT("Cannot read the body file: %s"), *Path);
			return false;
		}
		break;
	}

	case ECourierBodyType::GraphQL:
	{
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("query"), ResolveVariables(Spec.GraphQLQuery, Variables));
		const FString VariablesText = ResolveVariables(Spec.GraphQLVariables, Variables).TrimStartAndEnd();
		if (!VariablesText.IsEmpty())
		{
			TSharedPtr<FJsonObject> VariablesObject;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(VariablesText);
			if (!FJsonSerializer::Deserialize(Reader, VariablesObject) || !VariablesObject.IsValid())
			{
				OutError = TEXT("GraphQL variables are not a JSON object.");
				return false;
			}
			Payload->SetObjectField(TEXT("variables"), VariablesObject);
		}
		FString PayloadText;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PayloadText);
		FJsonSerializer::Serialize(Payload, Writer);
		CourierHttp::AppendUtf8(Out.Body, PayloadText);
		break;
	}

	default:
		break;
	}

	if (!ContentType.IsEmpty() && !CourierHttp::HasHeader(Out.Headers, TEXT("Content-Type")))
	{
		Out.Headers.Emplace(TEXT("Content-Type"), ContentType);
	}
	return true;
}

FHttpRequestPtr FCourierHttp::Send(const FCourierRequestSpec& Spec, const TMap<FString, FString>& Variables, FOnComplete OnComplete, FOnProgress OnProgress)
{
	const bool bExplicitAuthorization = Spec.Headers.ContainsByPredicate([](const FCourierKeyValue& Row)
	{
		return Row.bEnabled && Row.Key.Equals(TEXT("Authorization"), ESearchCase::IgnoreCase);
	});
	if (Spec.Auth.Type == ECourierAuthType::OAuth2 && !bExplicitAuthorization)
	{
		const FCourierOAuth2 OAuth2 = FCourierOAuth2Client::Resolve(Spec.Auth.OAuth2, Variables);
		if (FCourierOAuth2Client::FindUsableToken(OAuth2, true).IsEmpty())
		{
			FCourierOAuth2Client::GetToken(OAuth2, [Spec, Variables, OnComplete, OnProgress](const FCourierOAuth2Token& Token) mutable
			{
				if (!Token.bSuccess)
				{
					FCourierResponse Failed;
					Failed.Error = TEXT("OAuth 2.0: ") + Token.Error;
					Failed.Url = BuildUrl(Spec, Variables);
					Failed.Verb = VerbToString(Spec.Verb, Spec.CustomVerb);
					if (OnComplete)
					{
						OnComplete(Failed);
					}
					return;
				}
				// The token is cached now, so Prepare puts it in the Authorization header.
				FCourierRequestSpec WithToken = Spec;
				if (FCourierOAuth2Client::FindUsableToken(FCourierOAuth2Client::Resolve(Spec.Auth.OAuth2, Variables), false).IsEmpty())
				{
					WithToken.Auth.OAuth2.AccessToken = Token.AccessToken;
				}
				FCourierPreparedRequest Prepared;
				FString Error;
				if (!Prepare(WithToken, Variables, Prepared, Error))
				{
					FCourierResponse Failed;
					Failed.Error = Error;
					Failed.Url = Prepared.Url;
					Failed.Verb = Prepared.Verb;
					if (OnComplete)
					{
						OnComplete(Failed);
					}
					return;
				}
				SendPrepared(Prepared, MoveTemp(OnComplete), MoveTemp(OnProgress));
			});
			return nullptr;
		}
	}

	FCourierPreparedRequest Prepared;
	FString Error;
	if (!Prepare(Spec, Variables, Prepared, Error))
	{
		FCourierResponse Failed;
		Failed.Error = Error;
		Failed.Url = Prepared.Url;
		Failed.Verb = Prepared.Verb;
		UE_LOG(LogCourier, Warning, TEXT("Courier: request '%s' not sent: %s"), *Spec.Name, *Error);
		AsyncTask(ENamedThreads::GameThread, [OnComplete = MoveTemp(OnComplete), Failed]()
		{
			if (OnComplete)
			{
				OnComplete(Failed);
			}
		});
		return nullptr;
	}
	return SendPrepared(Prepared, MoveTemp(OnComplete), MoveTemp(OnProgress));
}

FHttpRequestPtr FCourierHttp::SendPrepared(const FCourierPreparedRequest& Prepared, FOnComplete OnComplete, FOnProgress OnProgress)
{
	const FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetVerb(Prepared.Verb);
	Request->SetURL(Prepared.Url);
	for (const TPair<FString, FString>& Header : Prepared.Headers)
	{
		Request->SetHeader(Header.Key, Header.Value);
	}
	if (Prepared.Body.Num() > 0)
	{
		Request->SetContent(Prepared.Body);
	}
	if (Prepared.TimeoutSeconds > 0.f)
	{
		Request->SetTimeout(Prepared.TimeoutSeconds);
	}

	const double StartTime = FPlatformTime::Seconds();
	const bool bUseCookieJar = Prepared.bUseCookieJar;
	Request->OnProcessRequestComplete().BindLambda([OnComplete = MoveTemp(OnComplete), StartTime, bUseCookieJar](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnected)
	{
		if (bUseCookieJar && Req.IsValid() && Resp.IsValid() && Resp->GetResponseCode() > 0)
		{
			FCourierCookieJar::Get().StoreFromResponseHeaders(Req->GetURL(), Resp->GetAllHeaders());
		}
		const FCourierResponse Response = CourierHttp::MakeResponse(Req, Resp, bConnected, StartTime);
		UE_LOG(LogCourier, Verbose, TEXT("Courier: %s %s -> %d (%.0f ms)"), *Response.Verb, *Response.Url, Response.StatusCode, Response.ElapsedSeconds * 1000.f);
		if (OnComplete)
		{
			OnComplete(Response);
		}
	});
	if (OnProgress)
	{
		Request->OnRequestProgress64().BindLambda([OnProgress = MoveTemp(OnProgress)](FHttpRequestPtr, uint64 Sent, uint64 Received)
		{
			OnProgress(Sent, Received);
		});
	}
	Request->ProcessRequest();
	return Request;
}

FString FCourierHttp::ToCurl(const FCourierRequestSpec& Spec, const TMap<FString, FString>& Variables)
{
	FCourierPreparedRequest Prepared;
	FString Error;
	const bool bPrepared = Prepare(Spec, Variables, Prepared, Error);
	if (!bPrepared && Prepared.Url.IsEmpty())
	{
		return FString();
	}

	FString Curl = TEXT("curl");
	if (Prepared.Verb != TEXT("GET") || Spec.BodyType != ECourierBodyType::None)
	{
		Curl += TEXT(" -X ") + Prepared.Verb;
	}
	Curl += TEXT(" ") + CourierHttp::ShellQuote(Prepared.Url);

	const bool bMultipart = Spec.BodyType == ECourierBodyType::Multipart;
	for (const TPair<FString, FString>& Header : Prepared.Headers)
	{
		// curl writes its own multipart boundary.
		if (bMultipart && Header.Key.Equals(TEXT("Content-Type"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		Curl += TEXT(" \\\n  -H ") + CourierHttp::ShellQuote(Header.Key + TEXT(": ") + Header.Value);
	}

	switch (Spec.BodyType)
	{
	case ECourierBodyType::Multipart:
		for (const FCourierFormField& Field : Spec.FormFields)
		{
			if (Field.bEnabled && !Field.Key.IsEmpty())
			{
				const FString Value = ResolveVariables(Field.Value, Variables);
				Curl += TEXT(" \\\n  -F ") + CourierHttp::ShellQuote(ResolveVariables(Field.Key, Variables) + (Field.bIsFile ? TEXT("=@") : TEXT("=")) + Value);
			}
		}
		break;
	case ECourierBodyType::Binary:
		Curl += TEXT(" \\\n  --data-binary ") + CourierHttp::ShellQuote(TEXT("@") + ResolveVariables(Spec.Body, Variables));
		break;
	case ECourierBodyType::None:
		break;
	default:
		if (Prepared.Body.Num() > 0)
		{
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Prepared.Body.GetData()), Prepared.Body.Num());
			const FString BodyText(Converted.Length(), Converted.Get());
			Curl += TEXT(" \\\n  --data-raw ") + CourierHttp::ShellQuote(BodyText);
		}
		break;
	}
	return Curl;
}
