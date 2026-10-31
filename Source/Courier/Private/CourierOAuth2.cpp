#include "CourierOAuth2.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "CourierHttp.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HttpPath.h"
#include "HttpRequestHandler.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Misc/Base64.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace CourierOAuth2
{
	FCriticalSection CacheLock;
	TMap<FString, FCourierOAuth2Token> Cache;
	TMap<FString, TArray<FCourierOAuth2Client::FOnToken>> Pending;

	/** A browser sign-in in progress; one per redirect port. */
	struct FInteractive
	{
		TSharedPtr<IHttpRouter> Router;
		FHttpRouteHandle Route;
		FTSTicker::FDelegateHandle Timeout;
	};
	TMap<uint32, FInteractive> Interactive;

	constexpr double ExpiryMarginSeconds = 30.0;
	constexpr float BrowserTimeoutSeconds = 300.f;

	bool IsFresh(const FCourierOAuth2Token& Token, bool bWithMargin)
	{
		if (Token.AccessToken.IsEmpty())
		{
			return false;
		}
		if (Token.ExpiresAt.GetTicks() == 0)
		{
			return true;
		}
		const FDateTime Limit = bWithMargin ? Token.ExpiresAt - FTimespan::FromSeconds(ExpiryMarginSeconds) : Token.ExpiresAt;
		return FDateTime::UtcNow() < Limit;
	}

	FString RandomString(int32 Length)
	{
		static const TCHAR* Alphabet = TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~");
		FString Out;
		Out.Reserve(Length);
		for (int32 Index = 0; Index < Length; ++Index)
		{
			// Mix two GUID-quality sources so the verifier is not guessable from FMath's seed alone.
			const uint32 Random = FGuid::NewGuid().A ^ static_cast<uint32>(FMath::Rand());
			Out.AppendChar(Alphabet[Random % 66]);
		}
		return Out;
	}

	FString FormEncode(const TArray<TPair<FString, FString>>& Fields)
	{
		TArray<FString> Parts;
		for (const TPair<FString, FString>& Field : Fields)
		{
			if (!Field.Value.IsEmpty())
			{
				Parts.Add(FCourierHttp::UrlEncode(Field.Key) + TEXT("=") + FCourierHttp::UrlEncode(Field.Value));
			}
		}
		return FString::Join(Parts, TEXT("&"));
	}

	/** Runs every waiting callback for Key with Token and caches a good token. */
	void Complete(const FString& Key, const FCourierOAuth2Token& Token)
	{
		TArray<FCourierOAuth2Client::FOnToken> Callbacks;
		{
			FScopeLock Lock(&CacheLock);
			if (Token.bSuccess)
			{
				FCourierOAuth2Token Stored = Token;
				// Keep the old refresh token when a refresh reply does not send a new one.
				if (Stored.RefreshToken.IsEmpty())
				{
					if (const FCourierOAuth2Token* Previous = Cache.Find(Key))
					{
						Stored.RefreshToken = Previous->RefreshToken;
					}
				}
				Cache.Add(Key, Stored);
			}
			Pending.RemoveAndCopyValue(Key, Callbacks);
		}
		if (Token.bSuccess)
		{
			FCourierOAuth2Client::OnCacheChanged().Broadcast();
		}
		else
		{
			UE_LOG(LogCourier, Warning, TEXT("Courier: OAuth 2.0 token request failed: %s"), *Token.Error);
		}
		for (FCourierOAuth2Client::FOnToken& Callback : Callbacks)
		{
			if (Callback)
			{
				Callback(Token);
			}
		}
	}

	void RequestToken(const FString& Key, const FCourierOAuth2& Resolved, const TArray<TPair<FString, FString>>& Fields)
	{
		if (Resolved.AccessTokenUrl.IsEmpty())
		{
			FCourierOAuth2Token Failed;
			Failed.Error = TEXT("The access token URL is empty.");
			AsyncTask(ENamedThreads::GameThread, [Key, Failed]() { Complete(Key, Failed); });
			return;
		}
		const FCourierPreparedRequest Request = FCourierOAuth2Client::BuildTokenRequest(Resolved, Fields);
		FCourierHttp::SendPrepared(Request, [Key](const FCourierResponse& Response)
		{
			FCourierOAuth2Token Token = Response.bReceivedResponse
				? FCourierOAuth2Client::ParseTokenResponse(Response.StatusCode, Response.Body, FDateTime::UtcNow())
				: FCourierOAuth2Token();
			if (!Response.bReceivedResponse)
			{
				Token.Error = Response.Error;
			}
			Complete(Key, Token);
		});
	}

	void StopInteractive(uint32 Port)
	{
		FInteractive Session;
		if (Interactive.RemoveAndCopyValue(Port, Session))
		{
			if (Session.Router.IsValid() && Session.Route.IsValid())
			{
				Session.Router->UnbindRoute(Session.Route);
			}
			FTSTicker::GetCoreTicker().RemoveTicker(Session.Timeout);
		}
	}

	void RunAuthorizationCode(const FString& Key, const FCourierOAuth2& Resolved)
	{
		auto Fail = [Key](const FString& Error)
		{
			FCourierOAuth2Token Failed;
			Failed.Error = Error;
			Complete(Key, Failed);
		};

		uint32 Port = 0;
		FString Path;
		if (!FCourierOAuth2Client::ParseLoopbackRedirect(Resolved.RedirectUri, Port, Path))
		{
			Fail(TEXT("The redirect URI must be http://127.0.0.1:<port>/<path> or http://localhost:<port>/<path> so Courier can catch the redirect."));
			return;
		}
		if (Resolved.AuthUrl.IsEmpty())
		{
			Fail(TEXT("The authorization URL is empty."));
			return;
		}
		if (Interactive.Contains(Port))
		{
			Fail(FString::Printf(TEXT("Another sign-in is already waiting on port %u."), Port));
			return;
		}

		const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure*/ true);
		if (!Router.IsValid())
		{
			Fail(FString::Printf(TEXT("Cannot listen on port %u for the redirect."), Port));
			return;
		}

		const FString State = RandomString(32);
		const FString Verifier = Resolved.bUsePkce ? FCourierOAuth2Client::MakeCodeVerifier() : FString();
		const FString Challenge = Resolved.bUsePkce ? FCourierOAuth2Client::MakeCodeChallenge(Verifier) : FString();

		FInteractive Session;
		Session.Router = Router;
		Session.Route = Router->BindRoute(FHttpPath(Path), EHttpServerRequestVerbs::VERB_GET,
			FHttpRequestHandler::CreateLambda([Key, Resolved, State, Verifier, Port](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				const FString* Code = Request.QueryParams.Find(TEXT("code"));
				const FString* ReturnedState = Request.QueryParams.Find(TEXT("state"));
				const FString* Error = Request.QueryParams.Find(TEXT("error"));
				FString Message;
				FCourierOAuth2Token Failed;
				if (Error)
				{
					const FString* Description = Request.QueryParams.Find(TEXT("error_description"));
					Failed.Error = Description ? FString::Printf(TEXT("%s: %s"), **Error, *FGenericPlatformHttp::UrlDecode(*Description)) : *Error;
					Message = TEXT("Sign-in failed. You can close this tab and check Unreal Engine.");
				}
				else if (!Code || !ReturnedState || *ReturnedState != State)
				{
					Failed.Error = TEXT("The redirect had no code or a wrong state.");
					Message = TEXT("Sign-in failed. You can close this tab and check Unreal Engine.");
				}
				else
				{
					Message = TEXT("Signed in. You can close this tab and return to Unreal Engine.");
				}
				const FString Html = FString::Printf(TEXT("<!doctype html><html><head><meta charset=\"utf-8\"><title>Courier</title></head>")
					TEXT("<body style=\"font-family:sans-serif;background:#161b2e;color:#e6edf3;display:flex;align-items:center;justify-content:center;height:100vh;margin:0\">")
					TEXT("<p style=\"font-size:18px\">%s</p></body></html>"), *Message);
				OnComplete(FHttpServerResponse::Create(Html, TEXT("text/html")));

				const FString CodeValue = Code ? FGenericPlatformHttp::UrlDecode(*Code) : FString();
				// Unbind and exchange the code after this handler returns.
				AsyncTask(ENamedThreads::GameThread, [Key, Resolved, Verifier, Port, CodeValue, Failed]()
				{
					StopInteractive(Port);
					if (!Failed.Error.IsEmpty())
					{
						Complete(Key, Failed);
						return;
					}
					TArray<TPair<FString, FString>> Fields;
					Fields.Emplace(TEXT("grant_type"), TEXT("authorization_code"));
					Fields.Emplace(TEXT("code"), CodeValue);
					Fields.Emplace(TEXT("redirect_uri"), Resolved.RedirectUri);
					Fields.Emplace(TEXT("code_verifier"), Verifier);
					RequestToken(Key, Resolved, Fields);
				});
				return true;
			}));
		Session.Timeout = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Key, Port](float)
		{
			StopInteractive(Port);
			FCourierOAuth2Token Failed;
			Failed.Error = TEXT("Timed out waiting for the browser sign-in.");
			Complete(Key, Failed);
			return false;
		}), BrowserTimeoutSeconds);
		Interactive.Add(Port, Session);
		FHttpServerModule::Get().StartAllListeners();

		const FString Url = FCourierOAuth2Client::BuildAuthorizationUrl(Resolved, State, Challenge);
		FString LaunchError;
		FPlatformProcess::LaunchURL(*Url, nullptr, &LaunchError);
		if (!LaunchError.IsEmpty())
		{
			StopInteractive(Port);
			Fail(FString::Printf(TEXT("Cannot open the browser: %s"), *LaunchError));
			return;
		}
		UE_LOG(LogCourier, Log, TEXT("Courier: waiting for the OAuth 2.0 sign-in in the browser (redirect to %s)."), *Resolved.RedirectUri);
	}

	// ---- SHA-256 (FIPS 180-4), small and dependency free ----

	constexpr uint32 K[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

	FORCEINLINE uint32 Rotr(uint32 X, uint32 N) { return (X >> N) | (X << (32 - N)); }

	void Sha256Block(uint32 State[8], const uint8* Block)
	{
		uint32 W[64];
		for (int32 I = 0; I < 16; ++I)
		{
			W[I] = (uint32(Block[I * 4]) << 24) | (uint32(Block[I * 4 + 1]) << 16) | (uint32(Block[I * 4 + 2]) << 8) | uint32(Block[I * 4 + 3]);
		}
		for (int32 I = 16; I < 64; ++I)
		{
			const uint32 S0 = Rotr(W[I - 15], 7) ^ Rotr(W[I - 15], 18) ^ (W[I - 15] >> 3);
			const uint32 S1 = Rotr(W[I - 2], 17) ^ Rotr(W[I - 2], 19) ^ (W[I - 2] >> 10);
			W[I] = W[I - 16] + S0 + W[I - 7] + S1;
		}
		uint32 A = State[0], B = State[1], C = State[2], D = State[3], E = State[4], F = State[5], G = State[6], H = State[7];
		for (int32 I = 0; I < 64; ++I)
		{
			const uint32 S1 = Rotr(E, 6) ^ Rotr(E, 11) ^ Rotr(E, 25);
			const uint32 Ch = (E & F) ^ (~E & G);
			const uint32 T1 = H + S1 + Ch + K[I] + W[I];
			const uint32 S0 = Rotr(A, 2) ^ Rotr(A, 13) ^ Rotr(A, 22);
			const uint32 Maj = (A & B) ^ (A & C) ^ (B & C);
			const uint32 T2 = S0 + Maj;
			H = G; G = F; F = E; E = D + T1; D = C; C = B; B = A; A = T1 + T2;
		}
		State[0] += A; State[1] += B; State[2] += C; State[3] += D; State[4] += E; State[5] += F; State[6] += G; State[7] += H;
	}
}

FSimpleMulticastDelegate& FCourierOAuth2Client::OnCacheChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

FCourierOAuth2 FCourierOAuth2Client::Resolve(const FCourierOAuth2& Settings, const TMap<FString, FString>& Variables)
{
	FCourierOAuth2 Out = Settings;
	for (FString* Field : { &Out.AccessTokenUrl, &Out.AuthUrl, &Out.ClientId, &Out.ClientSecret, &Out.Scope, &Out.Audience,
		&Out.Username, &Out.Password, &Out.RedirectUri, &Out.HeaderPrefix, &Out.AccessToken })
	{
		*Field = FCourierHttp::ResolveVariables(*Field, Variables).TrimStartAndEnd();
	}
	return Out;
}

FString FCourierOAuth2Client::CacheKey(const FCourierOAuth2& Resolved)
{
	return FString::Printf(TEXT("%d|%s|%s|%s|%s|%s"), static_cast<int32>(Resolved.GrantType), *Resolved.AccessTokenUrl, *Resolved.ClientId,
		*Resolved.Scope, *Resolved.Audience, Resolved.GrantType == ECourierOAuth2Grant::Password ? *Resolved.Username : TEXT(""));
}

bool FCourierOAuth2Client::GetCachedToken(const FCourierOAuth2& Resolved, FCourierOAuth2Token& OutToken)
{
	FScopeLock Lock(&CourierOAuth2::CacheLock);
	if (const FCourierOAuth2Token* Found = CourierOAuth2::Cache.Find(CacheKey(Resolved)))
	{
		OutToken = *Found;
		return true;
	}
	return false;
}

void FCourierOAuth2Client::ClearCache()
{
	{
		FScopeLock Lock(&CourierOAuth2::CacheLock);
		CourierOAuth2::Cache.Reset();
	}
	OnCacheChanged().Broadcast();
}

FString FCourierOAuth2Client::FindUsableToken(const FCourierOAuth2& Resolved, bool bWithMargin)
{
	if (!Resolved.AccessToken.IsEmpty())
	{
		return Resolved.AccessToken;
	}
	FCourierOAuth2Token Cached;
	return GetCachedToken(Resolved, Cached) && CourierOAuth2::IsFresh(Cached, bWithMargin) ? Cached.AccessToken : FString();
}

void FCourierOAuth2Client::GetToken(const FCourierOAuth2& Resolved, FOnToken OnDone, bool bForceNew)
{
	check(IsInGameThread());
	const FString Key = CacheKey(Resolved);

	FCourierOAuth2Token Cached;
	const bool bHasCached = GetCachedToken(Resolved, Cached);
	if (!bForceNew)
	{
		if (!Resolved.AccessToken.IsEmpty())
		{
			FCourierOAuth2Token Given;
			Given.bSuccess = true;
			Given.AccessToken = Resolved.AccessToken;
			Given.TokenType = Resolved.HeaderPrefix;
			AsyncTask(ENamedThreads::GameThread, [OnDone = MoveTemp(OnDone), Given]() { if (OnDone) { OnDone(Given); } });
			return;
		}
		if (bHasCached && CourierOAuth2::IsFresh(Cached, true))
		{
			AsyncTask(ENamedThreads::GameThread, [OnDone = MoveTemp(OnDone), Cached]() { if (OnDone) { OnDone(Cached); } });
			return;
		}
	}

	{
		FScopeLock Lock(&CourierOAuth2::CacheLock);
		if (TArray<FOnToken>* Waiting = CourierOAuth2::Pending.Find(Key))
		{
			Waiting->Add(MoveTemp(OnDone));
			return;
		}
		CourierOAuth2::Pending.Add(Key).Add(MoveTemp(OnDone));
	}

	TArray<TPair<FString, FString>> Fields;
	if (!bForceNew && bHasCached && !Cached.RefreshToken.IsEmpty())
	{
		Fields.Emplace(TEXT("grant_type"), TEXT("refresh_token"));
		Fields.Emplace(TEXT("refresh_token"), Cached.RefreshToken);
		Fields.Emplace(TEXT("scope"), Resolved.Scope);
		CourierOAuth2::RequestToken(Key, Resolved, Fields);
		return;
	}

	switch (Resolved.GrantType)
	{
	case ECourierOAuth2Grant::ClientCredentials:
		Fields.Emplace(TEXT("grant_type"), TEXT("client_credentials"));
		Fields.Emplace(TEXT("scope"), Resolved.Scope);
		Fields.Emplace(TEXT("audience"), Resolved.Audience);
		CourierOAuth2::RequestToken(Key, Resolved, Fields);
		break;
	case ECourierOAuth2Grant::Password:
		Fields.Emplace(TEXT("grant_type"), TEXT("password"));
		Fields.Emplace(TEXT("username"), Resolved.Username);
		Fields.Emplace(TEXT("password"), Resolved.Password);
		Fields.Emplace(TEXT("scope"), Resolved.Scope);
		Fields.Emplace(TEXT("audience"), Resolved.Audience);
		CourierOAuth2::RequestToken(Key, Resolved, Fields);
		break;
	case ECourierOAuth2Grant::AuthorizationCode:
		CourierOAuth2::RunAuthorizationCode(Key, Resolved);
		break;
	}
}

FString FCourierOAuth2Client::MakeCodeVerifier()
{
	return CourierOAuth2::RandomString(64);
}

TArray<uint8> FCourierOAuth2Client::Sha256(const uint8* Data, int64 Length)
{
	uint32 State[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	int64 Offset = 0;
	for (; Offset + 64 <= Length; Offset += 64)
	{
		CourierOAuth2::Sha256Block(State, Data + Offset);
	}
	uint8 Tail[128] = {};
	const int64 Remaining = Length - Offset;
	if (Remaining > 0)
	{
		FMemory::Memcpy(Tail, Data + Offset, Remaining);
	}
	Tail[Remaining] = 0x80;
	const int32 TailBlocks = Remaining + 9 > 64 ? 2 : 1;
	const uint64 Bits = static_cast<uint64>(Length) * 8;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Tail[TailBlocks * 64 - 1 - Index] = static_cast<uint8>(Bits >> (Index * 8));
	}
	for (int32 Block = 0; Block < TailBlocks; ++Block)
	{
		CourierOAuth2::Sha256Block(State, Tail + Block * 64);
	}
	TArray<uint8> Digest;
	Digest.SetNumUninitialized(32);
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Digest[Index * 4] = static_cast<uint8>(State[Index] >> 24);
		Digest[Index * 4 + 1] = static_cast<uint8>(State[Index] >> 16);
		Digest[Index * 4 + 2] = static_cast<uint8>(State[Index] >> 8);
		Digest[Index * 4 + 3] = static_cast<uint8>(State[Index]);
	}
	return Digest;
}

FString FCourierOAuth2Client::Base64UrlEncode(const TArray<uint8>& Bytes)
{
	FString Encoded = FBase64::Encode(Bytes);
	Encoded.ReplaceCharInline(TEXT('+'), TEXT('-'));
	Encoded.ReplaceCharInline(TEXT('/'), TEXT('_'));
	while (Encoded.EndsWith(TEXT("=")))
	{
		Encoded.LeftChopInline(1);
	}
	return Encoded;
}

FString FCourierOAuth2Client::MakeCodeChallenge(const FString& Verifier)
{
	const FTCHARToUTF8 Utf8(*Verifier);
	return Base64UrlEncode(Sha256(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

FString FCourierOAuth2Client::BuildAuthorizationUrl(const FCourierOAuth2& Resolved, const FString& State, const FString& CodeChallenge)
{
	TArray<TPair<FString, FString>> Fields;
	Fields.Emplace(TEXT("response_type"), TEXT("code"));
	Fields.Emplace(TEXT("client_id"), Resolved.ClientId);
	Fields.Emplace(TEXT("redirect_uri"), Resolved.RedirectUri);
	Fields.Emplace(TEXT("scope"), Resolved.Scope);
	Fields.Emplace(TEXT("audience"), Resolved.Audience);
	Fields.Emplace(TEXT("state"), State);
	if (!CodeChallenge.IsEmpty())
	{
		Fields.Emplace(TEXT("code_challenge"), CodeChallenge);
		Fields.Emplace(TEXT("code_challenge_method"), TEXT("S256"));
	}
	return Resolved.AuthUrl + (Resolved.AuthUrl.Contains(TEXT("?")) ? TEXT("&") : TEXT("?")) + CourierOAuth2::FormEncode(Fields);
}

FCourierPreparedRequest FCourierOAuth2Client::BuildTokenRequest(const FCourierOAuth2& Resolved, const TArray<TPair<FString, FString>>& Fields)
{
	TArray<TPair<FString, FString>> AllFields = Fields;
	FCourierPreparedRequest Out;
	Out.Verb = TEXT("POST");
	Out.Url = Resolved.AccessTokenUrl;
	Out.TimeoutSeconds = 30.f;
	if (Resolved.ClientAuthentication == ECourierOAuth2ClientAuth::BasicHeader && !Resolved.ClientSecret.IsEmpty())
	{
		const FTCHARToUTF8 Pair(*(Resolved.ClientId + TEXT(":") + Resolved.ClientSecret));
		Out.Headers.Emplace(TEXT("Authorization"), TEXT("Basic ") + FBase64::Encode(reinterpret_cast<const uint8*>(Pair.Get()), Pair.Length()));
	}
	else
	{
		// Body authentication, or a public client (no secret) identifying itself.
		AllFields.Emplace(TEXT("client_id"), Resolved.ClientId);
		AllFields.Emplace(TEXT("client_secret"), Resolved.ClientSecret);
	}
	Out.Headers.Emplace(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));
	Out.Headers.Emplace(TEXT("Accept"), TEXT("application/json"));
	const FTCHARToUTF8 Body(*CourierOAuth2::FormEncode(AllFields));
	Out.Body.Append(reinterpret_cast<const uint8*>(Body.Get()), Body.Length());
	return Out;
}

FCourierOAuth2Token FCourierOAuth2Client::ParseTokenResponse(int32 StatusCode, const FString& Body, const FDateTime& NowUtc)
{
	FCourierOAuth2Token Token;
	Token.RawResponse = Body;

	TMap<FString, FString> Fields;
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Json->Values)
		{
			FString Value;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(Value))
			{
				Fields.Add(Pair.Key, Value);
			}
		}
	}
	else
	{
		// GitHub and some older servers reply form-encoded.
		TArray<FString> Pairs;
		Body.ParseIntoArray(Pairs, TEXT("&"));
		for (const FString& Pair : Pairs)
		{
			FString Key, Value;
			if (Pair.Split(TEXT("="), &Key, &Value))
			{
				Fields.Add(FGenericPlatformHttp::UrlDecode(Key), FGenericPlatformHttp::UrlDecode(Value));
			}
		}
	}

	auto Get = [&Fields](const TCHAR* Name) { const FString* Found = Fields.Find(Name); return Found ? *Found : FString(); };
	Token.AccessToken = Get(TEXT("access_token"));
	Token.TokenType = Get(TEXT("token_type"));
	Token.RefreshToken = Get(TEXT("refresh_token"));
	Token.IdToken = Get(TEXT("id_token"));
	Token.Scope = Get(TEXT("scope"));
	const FString ExpiresIn = Get(TEXT("expires_in"));
	if (!ExpiresIn.IsEmpty() && FCString::Atod(*ExpiresIn) > 0.0)
	{
		Token.ExpiresAt = NowUtc + FTimespan::FromSeconds(FCString::Atod(*ExpiresIn));
	}

	const FString Error = Get(TEXT("error"));
	if (!Error.IsEmpty())
	{
		const FString Description = Get(TEXT("error_description"));
		Token.Error = Description.IsEmpty() ? Error : FString::Printf(TEXT("%s: %s"), *Error, *Description);
	}
	else if (StatusCode < 200 || StatusCode >= 300)
	{
		Token.Error = FString::Printf(TEXT("HTTP %d from the token URL"), StatusCode);
	}
	else if (Token.AccessToken.IsEmpty())
	{
		Token.Error = TEXT("The token URL answered without an access_token.");
	}
	Token.bSuccess = Token.Error.IsEmpty();
	return Token;
}

bool FCourierOAuth2Client::ParseLoopbackRedirect(const FString& RedirectUri, uint32& OutPort, FString& OutPath)
{
	FString Rest = RedirectUri.TrimStartAndEnd();
	if (!Rest.RemoveFromStart(TEXT("http://"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	int32 Slash = INDEX_NONE;
	const FString Authority = Rest.FindChar(TEXT('/'), Slash) ? Rest.Left(Slash) : Rest;
	OutPath = Slash == INDEX_NONE ? FString(TEXT("/")) : Rest.Mid(Slash);
	int32 Query = INDEX_NONE;
	if (OutPath.FindChar(TEXT('?'), Query))
	{
		OutPath.LeftInline(Query);
	}
	FString Host, PortText;
	if (!Authority.Split(TEXT(":"), &Host, &PortText))
	{
		Host = Authority;
		PortText = TEXT("80");
	}
	Host = Host.ToLower();
	if (Host != TEXT("127.0.0.1") && Host != TEXT("localhost"))
	{
		return false;
	}
	const int32 Port = FCString::Atoi(*PortText);
	if (Port <= 0 || Port > 65535)
	{
		return false;
	}
	OutPort = static_cast<uint32>(Port);
	return true;
}

// ------------------------------------------------------------------------------------------------

UCourierOAuth2Task* UCourierOAuth2Task::GetCourierOAuth2Token(UObject* WorldContextObject, const FCourierOAuth2& InSettings, const TMap<FString, FString>& Variables, bool bInForceNew)
{
	UCourierOAuth2Task* Task = NewObject<UCourierOAuth2Task>();
	Task->Settings = FCourierOAuth2Client::Resolve(InSettings, Variables);
	Task->bForceNew = bInForceNew;
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (World && World->GetGameInstance())
	{
		Task->RegisterWithGameInstance(World->GetGameInstance());
	}
	else
	{
		Task->AddToRoot();
		Task->bRooted = true;
	}
	return Task;
}

void UCourierOAuth2Task::Activate()
{
	TWeakObjectPtr<UCourierOAuth2Task> WeakThis(this);
	FCourierOAuth2Client::GetToken(Settings, [WeakThis](const FCourierOAuth2Token& Token)
	{
		UCourierOAuth2Task* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		(Token.bSuccess ? Self->OnSuccess : Self->OnFailure).Broadcast(Token);
		if (Self->bRooted)
		{
			Self->RemoveFromRoot();
			Self->bRooted = false;
		}
		Self->SetReadyToDestroy();
	}, bForceNew);
}
