#include "CourierCookies.h"

#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"

namespace CourierCookies
{
	bool IsExpired(const FCourierCookie& Cookie, const FDateTime& NowUtc)
	{
		return Cookie.Expires.GetTicks() != 0 && Cookie.Expires <= NowUtc;
	}

	bool IsSecureScheme(const FString& Scheme)
	{
		return Scheme == TEXT("https") || Scheme == TEXT("wss");
	}

	/** RFC 6265 5.1.4: the request path up to (not including) its last slash, or "/". */
	FString DefaultPath(const FString& RequestPath)
	{
		if (!RequestPath.StartsWith(TEXT("/")))
		{
			return TEXT("/");
		}
		int32 LastSlash = INDEX_NONE;
		RequestPath.FindLastChar(TEXT('/'), LastSlash);
		return LastSlash <= 0 ? FString(TEXT("/")) : RequestPath.Left(LastSlash);
	}

	bool IsIpAddress(const FString& Host)
	{
		if (Host.Contains(TEXT(":")))
		{
			return true;
		}
		for (const TCHAR Char : Host)
		{
			if (!FChar::IsDigit(Char) && Char != TEXT('.'))
			{
				return false;
			}
		}
		return !Host.IsEmpty();
	}
}

FCourierCookieJar& FCourierCookieJar::Get()
{
	static FCourierCookieJar Jar;
	return Jar;
}

void FCourierCookieJar::SplitUrl(const FString& Url, FString& OutScheme, FString& OutHost, FString& OutPath)
{
	FString Rest = Url.TrimStartAndEnd();
	OutScheme.Reset();
	const int32 SchemeEnd = Rest.Find(TEXT("://"));
	if (SchemeEnd != INDEX_NONE)
	{
		OutScheme = Rest.Left(SchemeEnd).ToLower();
		Rest.RightChopInline(SchemeEnd + 3);
	}
	int32 PathStart = INDEX_NONE;
	for (int32 Index = 0; Index < Rest.Len(); ++Index)
	{
		if (Rest[Index] == TEXT('/') || Rest[Index] == TEXT('?') || Rest[Index] == TEXT('#'))
		{
			PathStart = Index;
			break;
		}
	}
	FString Authority = PathStart == INDEX_NONE ? Rest : Rest.Left(PathStart);
	FString Path = PathStart == INDEX_NONE ? FString() : Rest.Mid(PathStart);

	// Drop user info and port.
	int32 At = INDEX_NONE;
	if (Authority.FindLastChar(TEXT('@'), At))
	{
		Authority.RightChopInline(At + 1);
	}
	if (Authority.StartsWith(TEXT("[")))
	{
		const int32 Close = Authority.Find(TEXT("]"));
		OutHost = Close == INDEX_NONE ? Authority : Authority.Mid(1, Close - 1);
	}
	else
	{
		int32 Colon = INDEX_NONE;
		OutHost = Authority.FindChar(TEXT(':'), Colon) ? Authority.Left(Colon) : Authority;
	}
	OutHost = OutHost.ToLower();

	int32 Cut = INDEX_NONE;
	if (Path.FindChar(TEXT('?'), Cut) || Path.FindChar(TEXT('#'), Cut))
	{
		Path.LeftInline(Cut);
	}
	OutPath = Path.IsEmpty() ? FString(TEXT("/")) : Path;
}

bool FCourierCookieJar::DomainMatches(const FString& Host, const FCourierCookie& Cookie)
{
	if (Host == Cookie.Domain)
	{
		return true;
	}
	return !Cookie.bHostOnly && Host.EndsWith(TEXT(".") + Cookie.Domain) && !CourierCookies::IsIpAddress(Host);
}

bool FCourierCookieJar::PathMatches(const FString& RequestPath, const FString& CookiePath)
{
	if (RequestPath == CookiePath)
	{
		return true;
	}
	if (!RequestPath.StartsWith(CookiePath))
	{
		return false;
	}
	return CookiePath.EndsWith(TEXT("/")) || RequestPath[CookiePath.Len()] == TEXT('/');
}

bool FCourierCookieJar::ParseSetCookie(const FString& RequestUrl, const FString& SetCookieValue, const FDateTime& NowUtc, FCourierCookie& OutCookie, bool& bOutDelete)
{
	bOutDelete = false;
	FString Scheme, Host, RequestPath;
	SplitUrl(RequestUrl, Scheme, Host, RequestPath);
	if (Host.IsEmpty())
	{
		return false;
	}

	TArray<FString> Parts;
	SetCookieValue.ParseIntoArray(Parts, TEXT(";"), false);
	if (Parts.Num() == 0)
	{
		return false;
	}
	FString Name, Value;
	if (!Parts[0].Split(TEXT("="), &Name, &Value))
	{
		return false;
	}
	Name.TrimStartAndEndInline();
	Value.TrimStartAndEndInline();
	if (Name.IsEmpty())
	{
		return false;
	}
	if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
	{
		Value = Value.Mid(1, Value.Len() - 2);
	}

	FCourierCookie Cookie;
	Cookie.Name = Name;
	Cookie.Value = Value;
	Cookie.Domain = Host;
	Cookie.bHostOnly = true;
	Cookie.Path = CourierCookies::DefaultPath(RequestPath);

	bool bHasMaxAge = false;
	for (int32 Index = 1; Index < Parts.Num(); ++Index)
	{
		FString Key, AttrValue;
		if (!Parts[Index].Split(TEXT("="), &Key, &AttrValue))
		{
			Key = Parts[Index];
		}
		Key.TrimStartAndEndInline();
		AttrValue.TrimStartAndEndInline();
		if (Key.Equals(TEXT("Domain"), ESearchCase::IgnoreCase))
		{
			FString Domain = AttrValue.ToLower();
			Domain.RemoveFromStart(TEXT("."));
			if (Domain.IsEmpty())
			{
				continue;
			}
			// A server may only set cookies for its own host or a parent domain of it.
			if (Host != Domain && !(Host.EndsWith(TEXT(".") + Domain) && !CourierCookies::IsIpAddress(Host)))
			{
				return false;
			}
			Cookie.Domain = Domain;
			Cookie.bHostOnly = false;
		}
		else if (Key.Equals(TEXT("Path"), ESearchCase::IgnoreCase))
		{
			if (AttrValue.StartsWith(TEXT("/")))
			{
				Cookie.Path = AttrValue;
			}
		}
		else if (Key.Equals(TEXT("Max-Age"), ESearchCase::IgnoreCase))
		{
			if (!AttrValue.IsEmpty() && (FChar::IsDigit(AttrValue[0]) || AttrValue[0] == TEXT('-')))
			{
				const int64 Seconds = FCString::Atoi64(*AttrValue);
				bHasMaxAge = true;
				Cookie.Expires = Seconds <= 0 ? FDateTime(1) : NowUtc + FTimespan::FromSeconds(static_cast<double>(Seconds));
			}
		}
		else if (Key.Equals(TEXT("Expires"), ESearchCase::IgnoreCase))
		{
			FDateTime Parsed;
			if (!bHasMaxAge && FDateTime::ParseHttpDate(AttrValue, Parsed))
			{
				// A date before 1970 or "now" still means delete; keep it non-zero so it is not a session cookie.
				Cookie.Expires = Parsed.GetTicks() == 0 ? FDateTime(1) : Parsed;
			}
		}
		else if (Key.Equals(TEXT("Secure"), ESearchCase::IgnoreCase))
		{
			Cookie.bSecure = true;
		}
		else if (Key.Equals(TEXT("HttpOnly"), ESearchCase::IgnoreCase))
		{
			Cookie.bHttpOnly = true;
		}
	}

	// A Secure cookie may only be set over a secure connection.
	if (Cookie.bSecure && !Scheme.IsEmpty() && !CourierCookies::IsSecureScheme(Scheme))
	{
		return false;
	}
	bOutDelete = CourierCookies::IsExpired(Cookie, NowUtc);
	OutCookie = MoveTemp(Cookie);
	return true;
}

void FCourierCookieJar::SetCookieLocked(const FCourierCookie& Cookie)
{
	const int32 Existing = Cookies.IndexOfByPredicate([&Cookie](const FCourierCookie& Other)
	{
		return Other.Name == Cookie.Name && Other.Domain == Cookie.Domain && Other.Path == Cookie.Path;
	});
	if (Existing != INDEX_NONE)
	{
		Cookies[Existing] = Cookie;
	}
	else
	{
		Cookies.Add(Cookie);
	}
}

bool FCourierCookieJar::StoreSetCookie(const FString& RequestUrl, const FString& SetCookieValue)
{
	FCourierCookie Cookie;
	bool bDelete = false;
	if (!ParseSetCookie(RequestUrl, SetCookieValue, FDateTime::UtcNow(), Cookie, bDelete))
	{
		return false;
	}
	{
		FScopeLock ScopeLock(&Lock);
		if (bDelete)
		{
			Cookies.RemoveAll([&Cookie](const FCourierCookie& Other)
			{
				return Other.Name == Cookie.Name && Other.Domain == Cookie.Domain && Other.Path == Cookie.Path;
			});
		}
		else
		{
			SetCookieLocked(Cookie);
		}
	}
	OnChanged.Broadcast();
	return true;
}

TArray<FString> FCourierCookieJar::SplitMergedSetCookie(const FString& Merged)
{
	TArray<FString> Out;
	int32 Start = 0;
	int32 Search = 0;
	while (true)
	{
		const int32 Comma = Merged.Find(TEXT(","), ESearchCase::CaseSensitive, ESearchDir::FromStart, Search);
		if (Comma == INDEX_NONE)
		{
			break;
		}
		// A new cookie starts when the text after the comma is "token=" before any ';' or ','.
		int32 Index = Comma + 1;
		while (Index < Merged.Len() && Merged[Index] == TEXT(' '))
		{
			++Index;
		}
		const int32 TokenStart = Index;
		while (Index < Merged.Len() && Merged[Index] != TEXT('=') && Merged[Index] != TEXT(';') && Merged[Index] != TEXT(',') && Merged[Index] != TEXT(' '))
		{
			++Index;
		}
		if (Index < Merged.Len() && Merged[Index] == TEXT('=') && Index > TokenStart)
		{
			Out.Add(Merged.Mid(Start, Comma - Start).TrimStartAndEnd());
			Start = TokenStart;
		}
		Search = Comma + 1;
	}
	const FString Last = Merged.Mid(Start).TrimStartAndEnd();
	if (!Last.IsEmpty())
	{
		Out.Add(Last);
	}
	return Out;
}

void FCourierCookieJar::StoreFromResponseHeaders(const FString& RequestUrl, const TArray<FString>& HeaderLines)
{
	for (const FString& Line : HeaderLines)
	{
		FString Key, Value;
		if (Line.Split(TEXT(":"), &Key, &Value) && Key.TrimStartAndEnd().Equals(TEXT("Set-Cookie"), ESearchCase::IgnoreCase))
		{
			for (const FString& One : SplitMergedSetCookie(Value))
			{
				StoreSetCookie(RequestUrl, One);
			}
		}
	}
}

TArray<FCourierCookie> FCourierCookieJar::GetCookies(const FString& Url) const
{
	const FDateTime Now = FDateTime::UtcNow();
	TArray<FCourierCookie> Out;
	FString Scheme, Host, Path;
	if (!Url.IsEmpty())
	{
		SplitUrl(Url, Scheme, Host, Path);
	}
	FScopeLock ScopeLock(&Lock);
	for (const FCourierCookie& Cookie : Cookies)
	{
		if (CourierCookies::IsExpired(Cookie, Now))
		{
			continue;
		}
		if (!Url.IsEmpty())
		{
			if (!DomainMatches(Host, Cookie) || !PathMatches(Path, Cookie.Path))
			{
				continue;
			}
			if (Cookie.bSecure && !CourierCookies::IsSecureScheme(Scheme))
			{
				continue;
			}
		}
		Out.Add(Cookie);
	}
	// RFC 6265 5.4: longer paths first.
	Out.StableSort([](const FCourierCookie& A, const FCourierCookie& B) { return A.Path.Len() > B.Path.Len(); });
	return Out;
}

FString FCourierCookieJar::GetCookieHeader(const FString& Url) const
{
	TArray<FString> Pairs;
	for (const FCourierCookie& Cookie : GetCookies(Url))
	{
		Pairs.Add(Cookie.Name + TEXT("=") + Cookie.Value);
	}
	return FString::Join(Pairs, TEXT("; "));
}

void FCourierCookieJar::SetCookie(const FCourierCookie& Cookie)
{
	if (Cookie.Name.IsEmpty() || Cookie.Domain.IsEmpty())
	{
		return;
	}
	FCourierCookie Normalized = Cookie;
	Normalized.Domain = Normalized.Domain.ToLower();
	Normalized.Domain.RemoveFromStart(TEXT("."));
	if (Normalized.Path.IsEmpty())
	{
		Normalized.Path = TEXT("/");
	}
	{
		FScopeLock ScopeLock(&Lock);
		SetCookieLocked(Normalized);
	}
	OnChanged.Broadcast();
}

int32 FCourierCookieJar::Remove(const FString& Domain, const FString& Name)
{
	FString Normalized = Domain.ToLower();
	Normalized.RemoveFromStart(TEXT("."));
	int32 Removed = 0;
	{
		FScopeLock ScopeLock(&Lock);
		Removed = Cookies.RemoveAll([&Normalized, &Name](const FCourierCookie& Cookie)
		{
			return Cookie.Domain == Normalized && (Name.IsEmpty() || Cookie.Name == Name);
		});
	}
	if (Removed > 0)
	{
		OnChanged.Broadcast();
	}
	return Removed;
}

void FCourierCookieJar::Clear()
{
	{
		FScopeLock ScopeLock(&Lock);
		Cookies.Reset();
	}
	OnChanged.Broadcast();
}

bool FCourierCookieJar::SaveToFile(const FString& Path) const
{
	const FDateTime Now = FDateTime::UtcNow();
	TArray<TSharedPtr<FJsonValue>> Array;
	{
		FScopeLock ScopeLock(&Lock);
		for (const FCourierCookie& Cookie : Cookies)
		{
			if (CourierCookies::IsExpired(Cookie, Now))
			{
				continue;
			}
			if (const TSharedPtr<FJsonObject> Object = FJsonObjectConverter::UStructToJsonObject(Cookie))
			{
				Array.Add(MakeShared<FJsonValueObject>(Object));
			}
		}
	}
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("cookies"), Array);
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Root, Writer);
	return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool FCourierCookieJar::LoadFromFile(const FString& Path)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Root->TryGetArrayField(TEXT("cookies"), Array))
	{
		return false;
	}
	TArray<FCourierCookie> Loaded;
	for (const TSharedPtr<FJsonValue>& Value : *Array)
	{
		FCourierCookie Cookie;
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value.IsValid() && Value->TryGetObject(Object) && FJsonObjectConverter::JsonObjectToUStruct(Object->ToSharedRef(), &Cookie))
		{
			Loaded.Add(Cookie);
		}
	}
	{
		FScopeLock ScopeLock(&Lock);
		for (const FCourierCookie& Cookie : Loaded)
		{
			SetCookieLocked(Cookie);
		}
	}
	OnChanged.Broadcast();
	return true;
}
