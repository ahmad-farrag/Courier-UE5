#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "CourierOAuth2.generated.h"

struct FCourierPreparedRequest;

/**
 * OAuth 2.0 token handling shared by the editor, the Blueprint nodes and FCourierHttp::Send.
 * Tokens are cached per (token URL, client id, scope, grant, user) and refreshed with the refresh
 * token when they expire. Callbacks run on the game thread.
 */
class COURIER_API FCourierOAuth2Client
{
public:
	using FOnToken = TFunction<void(const FCourierOAuth2Token&)>;

	/** Copy of Settings with {{variables}} filled in every text field. */
	static FCourierOAuth2 Resolve(const FCourierOAuth2& Settings, const TMap<FString, FString>& Variables);

	/**
	 * A token that can go out now: Settings.AccessToken when set, else a cached token. With bWithMargin,
	 * a cached token that expires within 30 seconds does not count. Empty when there is none.
	 */
	static FString FindUsableToken(const FCourierOAuth2& Resolved, bool bWithMargin = true);

	/**
	 * Calls OnDone once with a token: a valid cached one, a refreshed one, or a new one from the grant
	 * (Authorization Code opens the system browser and waits for the redirect). Concurrent calls for the
	 * same settings share one token request. bForceNew skips the cache and the refresh token.
	 */
	static void GetToken(const FCourierOAuth2& Resolved, FOnToken OnDone, bool bForceNew = false);

	static bool GetCachedToken(const FCourierOAuth2& Resolved, FCourierOAuth2Token& OutToken);
	static void ClearCache();

	/** Broadcast on the game thread when a token is stored or the cache is cleared. */
	static FSimpleMulticastDelegate& OnCacheChanged();

	// Building blocks, public for tests and custom flows.

	static FString CacheKey(const FCourierOAuth2& Resolved);
	/** 64 characters from the RFC 7636 unreserved set. */
	static FString MakeCodeVerifier();
	/** BASE64URL(SHA256(verifier)) without padding (S256). */
	static FString MakeCodeChallenge(const FString& Verifier);
	static TArray<uint8> Sha256(const uint8* Data, int64 Length);
	static FString Base64UrlEncode(const TArray<uint8>& Bytes);
	static FString BuildAuthorizationUrl(const FCourierOAuth2& Resolved, const FString& State, const FString& CodeChallenge);
	/** A POST to AccessTokenUrl with Fields form-encoded and the client credentials placed per ClientAuthentication. */
	static FCourierPreparedRequest BuildTokenRequest(const FCourierOAuth2& Resolved, const TArray<TPair<FString, FString>>& Fields);
	/** Reads a token endpoint reply (JSON, or form-encoded as some servers send). */
	static FCourierOAuth2Token ParseTokenResponse(int32 StatusCode, const FString& Body, const FDateTime& NowUtc);
	/** Splits http://127.0.0.1:8976/callback into port and path. False when it is not a loopback http URL. */
	static bool ParseLoopbackRedirect(const FString& RedirectUri, uint32& OutPort, FString& OutPath);
};

/** Gets an OAuth 2.0 access token. On Success / On Failure fire once with the result. */
UCLASS(meta = (ExposedAsyncProxy = "Task"))
class COURIER_API UCourierOAuth2Task : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCourierOAuth2TokenMulticastDelegate OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FCourierOAuth2TokenMulticastDelegate OnFailure;

	/**
	 * Uses the cache when the token is still valid and the refresh token when it has expired; otherwise
	 * runs the grant. Authorization Code opens the browser. Force New skips the cache.
	 */
	UFUNCTION(BlueprintCallable, Category = "Courier|OAuth2", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Variables", DisplayName = "Get Courier OAuth2 Token"))
	static UCourierOAuth2Task* GetCourierOAuth2Token(UObject* WorldContextObject, const FCourierOAuth2& Settings, const TMap<FString, FString>& Variables, bool bForceNew = false);

	virtual void Activate() override;

private:
	FCourierOAuth2 Settings;
	bool bForceNew = false;
	bool bRooted = false;
};
