#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"

/**
 * Cookies per domain and path, following RFC 6265 the way browsers and Postman do: Domain and
 * host-only cookies, default paths, Max-Age over Expires, Secure, deletion by expiry. One jar is
 * shared by every Courier request in the process (FCourierCookieJar::Get()). Thread safe.
 */
class COURIER_API FCourierCookieJar
{
public:
	static FCourierCookieJar& Get();

	/** Stores every Set-Cookie header of a response to RequestUrl. Header lines are "Name: Value". */
	void StoreFromResponseHeaders(const FString& RequestUrl, const TArray<FString>& HeaderLines);

	/** Parses one Set-Cookie value and stores (or deletes) the cookie. False when it is invalid or rejected. */
	bool StoreSetCookie(const FString& RequestUrl, const FString& SetCookieValue);

	/** The Cookie header value for a request to Url ("a=1; b=2"), empty when nothing matches. */
	FString GetCookieHeader(const FString& Url) const;

	/** Cookies that would be sent to Url; every cookie when Url is empty. Expired ones are skipped. */
	TArray<FCourierCookie> GetCookies(const FString& Url = FString()) const;

	/** Adds or replaces (same domain, path and name). */
	void SetCookie(const FCourierCookie& Cookie);

	/** Removes cookies of Domain; only the one called Name when Name is set. Returns how many went. */
	int32 Remove(const FString& Domain, const FString& Name = FString());

	void Clear();

	bool SaveToFile(const FString& Path) const;
	bool LoadFromFile(const FString& Path);

	/** Broadcast (on the calling thread) whenever the cookies change. */
	FSimpleMulticastDelegate OnChanged;

	/** Parses a Set-Cookie value against the URL it came from. Exposed for tests. */
	static bool ParseSetCookie(const FString& RequestUrl, const FString& SetCookieValue, const FDateTime& NowUtc, FCourierCookie& OutCookie, bool& bOutDelete);

	/**
	 * The engine joins repeated headers with ", ", so several Set-Cookie values arrive as one line. Splits it
	 * again, cutting only where a new "name=" starts (Expires dates contain commas too).
	 */
	static TArray<FString> SplitMergedSetCookie(const FString& Merged);

	/** Scheme, lower-case host and path of a URL ("/" when it has none). */
	static void SplitUrl(const FString& Url, FString& OutScheme, FString& OutHost, FString& OutPath);

	static bool DomainMatches(const FString& Host, const FCourierCookie& Cookie);
	static bool PathMatches(const FString& RequestPath, const FString& CookiePath);

private:
	void SetCookieLocked(const FCourierCookie& Cookie);

	mutable FCriticalSection Lock;
	TArray<FCourierCookie> Cookies;
};
