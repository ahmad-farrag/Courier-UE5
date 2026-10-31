#include "CourierCookies.h"
#include "CourierHttp.h"
#include "CourierOAuth2.h"
#include "CourierPostman.h"
#include "CourierSocketIO.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace CourierFeatureTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString Hex(const TArray<uint8>& Bytes)
	{
		return BytesToHex(Bytes.GetData(), Bytes.Num()).ToLower();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierCookiesTest, "Courier.Cookies", CourierFeatureTests::Flags)
bool FCourierCookiesTest::RunTest(const FString& Parameters)
{
	const FDateTime Now(2026, 10, 31, 12, 0, 0);
	FCourierCookie Cookie;
	bool bDelete = false;

	TestTrue(TEXT("plain cookie"), FCourierCookieJar::ParseSetCookie(TEXT("https://api.example.com/v1/login"), TEXT("session=abc123; HttpOnly"), Now, Cookie, bDelete));
	TestEqual(TEXT("name"), Cookie.Name, FString(TEXT("session")));
	TestEqual(TEXT("value"), Cookie.Value, FString(TEXT("abc123")));
	TestEqual(TEXT("host-only domain"), Cookie.Domain, FString(TEXT("api.example.com")));
	TestTrue(TEXT("host-only"), Cookie.bHostOnly);
	TestEqual(TEXT("default path is the directory"), Cookie.Path, FString(TEXT("/v1")));
	TestTrue(TEXT("http only"), Cookie.bHttpOnly);
	TestEqual(TEXT("session cookie"), Cookie.Expires.GetTicks(), int64(0));

	TestTrue(TEXT("domain cookie"), FCourierCookieJar::ParseSetCookie(TEXT("https://api.example.com/"), TEXT("pref=dark; Domain=.Example.com; Path=/; Max-Age=3600; Secure"), Now, Cookie, bDelete));
	TestEqual(TEXT("domain normalized"), Cookie.Domain, FString(TEXT("example.com")));
	TestFalse(TEXT("not host-only"), Cookie.bHostOnly);
	TestEqual(TEXT("max-age"), Cookie.Expires, Now + FTimespan::FromHours(1));
	TestTrue(TEXT("secure"), Cookie.bSecure);

	TestFalse(TEXT("foreign domain rejected"), FCourierCookieJar::ParseSetCookie(TEXT("https://api.example.com/"), TEXT("x=1; Domain=evil.com"), Now, Cookie, bDelete));
	TestFalse(TEXT("secure over http rejected"), FCourierCookieJar::ParseSetCookie(TEXT("http://example.com/"), TEXT("x=1; Secure"), Now, Cookie, bDelete));
	TestTrue(TEXT("max-age 0 parses"), FCourierCookieJar::ParseSetCookie(TEXT("https://example.com/"), TEXT("x=; Max-Age=0"), Now, Cookie, bDelete));
	TestTrue(TEXT("max-age 0 deletes"), bDelete);
	TestTrue(TEXT("expires parses"), FCourierCookieJar::ParseSetCookie(TEXT("https://example.com/"), TEXT("y=2; Expires=Wed, 21 Oct 2015 07:28:00 GMT"), Now, Cookie, bDelete));
	TestTrue(TEXT("past expires deletes"), bDelete);
	TestTrue(TEXT("quoted value"), FCourierCookieJar::ParseSetCookie(TEXT("https://example.com/"), TEXT("q=\"hello\""), Now, Cookie, bDelete) && Cookie.Value == TEXT("hello"));

	const TArray<FString> Split = FCourierCookieJar::SplitMergedSetCookie(TEXT("a=1; Path=/, b=2; Expires=Wed, 21 Oct 2026 07:28:00 GMT; HttpOnly, c=3"));
	TestEqual(TEXT("merged header split"), Split.Num(), 3);
	if (Split.Num() == 3)
	{
		TestEqual(TEXT("split keeps the date"), Split[1], FString(TEXT("b=2; Expires=Wed, 21 Oct 2026 07:28:00 GMT; HttpOnly")));
		TestEqual(TEXT("split last"), Split[2], FString(TEXT("c=3")));
	}

	FCourierCookieJar Jar;
	Jar.StoreSetCookie(TEXT("https://api.example.com/v1/login"), TEXT("session=abc; Path=/"));
	Jar.StoreSetCookie(TEXT("https://api.example.com/v1/login"), TEXT("scoped=1; Path=/v1"));
	Jar.StoreSetCookie(TEXT("https://api.example.com/"), TEXT("shared=yes; Domain=example.com"));
	Jar.StoreSetCookie(TEXT("https://api.example.com/"), TEXT("secret=s; Secure"));
	TestEqual(TEXT("longer path first"), Jar.GetCookieHeader(TEXT("https://api.example.com/v1/users")), FString(TEXT("scoped=1; session=abc; shared=yes; secret=s")));
	TestEqual(TEXT("path filters"), Jar.GetCookieHeader(TEXT("https://api.example.com/v2")), FString(TEXT("session=abc; shared=yes; secret=s")));
	TestEqual(TEXT("secure needs https"), Jar.GetCookieHeader(TEXT("http://api.example.com/")), FString(TEXT("session=abc; shared=yes")));
	TestEqual(TEXT("domain cookie reaches siblings"), Jar.GetCookieHeader(TEXT("https://www.example.com/")), FString(TEXT("shared=yes")));
	TestEqual(TEXT("other sites get nothing"), Jar.GetCookieHeader(TEXT("https://example.org/")), FString());
	Jar.StoreSetCookie(TEXT("https://api.example.com/"), TEXT("session=gone; Path=/; Max-Age=0"));
	TestFalse(TEXT("deleted"), Jar.GetCookieHeader(TEXT("https://api.example.com/")).Contains(TEXT("session=")));
	TestEqual(TEXT("remove by domain"), Jar.Remove(TEXT("example.com")), 1);

	const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Courier") / TEXT("CookieTest.json"));
	TestTrue(TEXT("save"), Jar.SaveToFile(File));
	FCourierCookieJar Loaded;
	TestTrue(TEXT("load"), Loaded.LoadFromFile(File));
	TestEqual(TEXT("round trip"), Loaded.GetCookieHeader(TEXT("https://api.example.com/v1/x")), Jar.GetCookieHeader(TEXT("https://api.example.com/v1/x")));
	IFileManager::Get().Delete(*File);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierOAuth2Test, "Courier.OAuth2", CourierFeatureTests::Flags)
bool FCourierOAuth2Test::RunTest(const FString& Parameters)
{
	// FIPS 180-2 and RFC 7636 appendix B test vectors.
	const FTCHARToUTF8 Abc(TEXT("abc"));
	TestEqual(TEXT("sha256(abc)"), CourierFeatureTests::Hex(FCourierOAuth2Client::Sha256(reinterpret_cast<const uint8*>(Abc.Get()), Abc.Length())),
		FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	const FTCHARToUTF8 Long(TEXT("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"));
	TestEqual(TEXT("sha256 two blocks"), CourierFeatureTests::Hex(FCourierOAuth2Client::Sha256(reinterpret_cast<const uint8*>(Long.Get()), Long.Length())),
		FString(TEXT("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")));
	TestEqual(TEXT("sha256 empty"), CourierFeatureTests::Hex(FCourierOAuth2Client::Sha256(nullptr, 0)),
		FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	TestEqual(TEXT("PKCE S256"), FCourierOAuth2Client::MakeCodeChallenge(TEXT("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")),
		FString(TEXT("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM")));
	const FString Verifier = FCourierOAuth2Client::MakeCodeVerifier();
	TestEqual(TEXT("verifier length"), Verifier.Len(), 64);

	FCourierOAuth2 Settings;
	Settings.AccessTokenUrl = TEXT("{{auth}}/token");
	Settings.ClientId = TEXT("game-client");
	Settings.ClientSecret = TEXT("{{secret}}");
	Settings.Scope = TEXT("profile scores:write");
	const FCourierOAuth2 Resolved = FCourierOAuth2Client::Resolve(Settings, { { TEXT("auth"), TEXT("https://id.example.com") }, { TEXT("secret"), TEXT("s3cr3t") } });
	TestEqual(TEXT("resolved url"), Resolved.AccessTokenUrl, FString(TEXT("https://id.example.com/token")));

	const FCourierPreparedRequest Header = FCourierOAuth2Client::BuildTokenRequest(Resolved, { { TEXT("grant_type"), TEXT("client_credentials") }, { TEXT("scope"), Resolved.Scope } });
	const TPair<FString, FString>* Basic = Header.Headers.FindByPredicate([](const TPair<FString, FString>& H) { return H.Key == TEXT("Authorization"); });
	TestTrue(TEXT("basic client auth"), Basic && Basic->Value == TEXT("Basic Z2FtZS1jbGllbnQ6czNjcjN0"));
	const FUTF8ToTCHAR HeaderBody(reinterpret_cast<const ANSICHAR*>(Header.Body.GetData()), Header.Body.Num());
	TestEqual(TEXT("form body"), FString(HeaderBody.Length(), HeaderBody.Get()), FString(TEXT("grant_type=client_credentials&scope=profile%20scores%3Awrite")));

	FCourierOAuth2 BodyAuth = Resolved;
	BodyAuth.ClientAuthentication = ECourierOAuth2ClientAuth::Body;
	const FCourierPreparedRequest InBody = FCourierOAuth2Client::BuildTokenRequest(BodyAuth, { { TEXT("grant_type"), TEXT("client_credentials") } });
	const FUTF8ToTCHAR BodyText(reinterpret_cast<const ANSICHAR*>(InBody.Body.GetData()), InBody.Body.Num());
	TestEqual(TEXT("credentials in body"), FString(BodyText.Length(), BodyText.Get()), FString(TEXT("grant_type=client_credentials&client_id=game-client&client_secret=s3cr3t")));

	const FDateTime Now(2026, 10, 31, 12, 0, 0);
	FCourierOAuth2Token Token = FCourierOAuth2Client::ParseTokenResponse(200, TEXT("{\"access_token\":\"at\",\"token_type\":\"Bearer\",\"expires_in\":3600,\"refresh_token\":\"rt\",\"scope\":\"profile\"}"), Now);
	TestTrue(TEXT("json token"), Token.bSuccess);
	TestEqual(TEXT("access"), Token.AccessToken, FString(TEXT("at")));
	TestEqual(TEXT("refresh"), Token.RefreshToken, FString(TEXT("rt")));
	TestEqual(TEXT("expiry"), Token.ExpiresAt, Now + FTimespan::FromHours(1));
	Token = FCourierOAuth2Client::ParseTokenResponse(200, TEXT("access_token=gho_x&scope=repo&token_type=bearer"), Now);
	TestTrue(TEXT("form token (GitHub style)"), Token.bSuccess && Token.AccessToken == TEXT("gho_x"));
	Token = FCourierOAuth2Client::ParseTokenResponse(400, TEXT("{\"error\":\"invalid_client\",\"error_description\":\"Unknown client\"}"), Now);
	TestFalse(TEXT("error reply"), Token.bSuccess);
	TestEqual(TEXT("error text"), Token.Error, FString(TEXT("invalid_client: Unknown client")));

	FCourierOAuth2 Code = Resolved;
	Code.AuthUrl = TEXT("https://id.example.com/authorize");
	const FString AuthorizeUrl = FCourierOAuth2Client::BuildAuthorizationUrl(Code, TEXT("st"), TEXT("ch"));
	TestTrue(TEXT("authorize url"), AuthorizeUrl.StartsWith(TEXT("https://id.example.com/authorize?response_type=code&client_id=game-client&redirect_uri=http%3A%2F%2F127.0.0.1%3A8976%2Fcallback")));
	TestTrue(TEXT("pkce in url"), AuthorizeUrl.EndsWith(TEXT("&state=st&code_challenge=ch&code_challenge_method=S256")));

	uint32 Port = 0;
	FString Path;
	TestTrue(TEXT("loopback redirect"), FCourierOAuth2Client::ParseLoopbackRedirect(TEXT("http://localhost:9090/oauth/done?x=1"), Port, Path) && Port == 9090 && Path == TEXT("/oauth/done"));
	TestFalse(TEXT("remote redirect rejected"), FCourierOAuth2Client::ParseLoopbackRedirect(TEXT("https://example.com/cb"), Port, Path));

	// A given token goes straight into the header.
	FCourierRequestSpec Spec;
	Spec.Url = TEXT("https://api.example.com/me");
	Spec.Auth.Type = ECourierAuthType::OAuth2;
	Spec.Auth.OAuth2.AccessToken = TEXT("{{tok}}");
	FCourierPreparedRequest Prepared;
	FString Error;
	TestTrue(TEXT("prepared"), FCourierHttp::Prepare(Spec, { { TEXT("tok"), TEXT("abc") } }, Prepared, Error));
	const TPair<FString, FString>* Authorization = Prepared.Headers.FindByPredicate([](const TPair<FString, FString>& H) { return H.Key == TEXT("Authorization"); });
	TestTrue(TEXT("bearer header"), Authorization && Authorization->Value == TEXT("Bearer abc"));

	// Postman round trip keeps the OAuth 2.0 settings.
	FCourierCollection Collection;
	Collection.Name = TEXT("OAuth");
	FCourierRequestSpec WithOAuth = Spec;
	WithOAuth.Auth.OAuth2 = Code;
	WithOAuth.Auth.OAuth2.GrantType = ECourierOAuth2Grant::AuthorizationCode;
	Collection.Requests.Add(WithOAuth);
	FCourierCollection Back;
	TestTrue(TEXT("re-import"), FCourierPostman::ImportCollection(FCourierPostman::ExportCollection(Collection), Back, Error) && Back.Requests.Num() == 1);
	if (Back.Requests.Num() == 1)
	{
		const FCourierOAuth2& Read = Back.Requests[0].Auth.OAuth2;
		TestTrue(TEXT("auth type"), Back.Requests[0].Auth.Type == ECourierAuthType::OAuth2);
		TestTrue(TEXT("grant"), Read.GrantType == ECourierOAuth2Grant::AuthorizationCode);
		TestTrue(TEXT("pkce kept"), Read.bUsePkce);
		TestEqual(TEXT("auth url"), Read.AuthUrl, Code.AuthUrl);
		TestEqual(TEXT("scope"), Read.Scope, Code.Scope);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierSocketIOTest, "Courier.SocketIO", CourierFeatureTests::Flags)
bool FCourierSocketIOTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("default path"), FCourierSocketIO::MakeEngineIoUrl(TEXT("https://chat.example.com")), FString(TEXT("wss://chat.example.com/socket.io/?EIO=4&transport=websocket")));
	TestEqual(TEXT("custom path and query"), FCourierSocketIO::MakeEngineIoUrl(TEXT("http://localhost:3000/realtime?token=t")), FString(TEXT("ws://localhost:3000/realtime/?EIO=4&transport=websocket&token=t")));

	TestEqual(TEXT("event"), FCourierSocketIO::EncodeEvent(TEXT("/"), TEXT("move"), TEXT("[{\"x\":1}]")), FString(TEXT("2[\"move\",{\"x\":1}]")));
	TestEqual(TEXT("event with namespace and ack"), FCourierSocketIO::EncodeEvent(TEXT("/match"), TEXT("goal"), TEXT("[]"), 12), FString(TEXT("2/match,12[\"goal\"]")));
	TestEqual(TEXT("connect with auth"), FCourierSocketIO::EncodeConnect(TEXT("/match"), TEXT("{\"token\":\"t\"}")), FString(TEXT("0/match,{\"token\":\"t\"}")));
	TestEqual(TEXT("connect default"), FCourierSocketIO::EncodeConnect(TEXT("/"), FString()), FString(TEXT("0")));
	TestEqual(TEXT("disconnect"), FCourierSocketIO::EncodeDisconnect(TEXT("/match")), FString(TEXT("1/match,")));

	FCourierSocketIOPacket Packet;
	TestTrue(TEXT("decode event"), FCourierSocketIO::Decode(TEXT("2/match,7[\"score\",{\"home\":2},\"x\"]"), Packet));
	TestEqual(TEXT("type"), Packet.Type, 2);
	TestEqual(TEXT("namespace"), Packet.Namespace, FString(TEXT("/match")));
	TestEqual(TEXT("ack id"), Packet.AckId, 7);
	FString Name, Args;
	TestTrue(TEXT("split"), FCourierSocketIO::SplitEvent(Packet.Payload, Name, Args));
	TestEqual(TEXT("event name"), Name, FString(TEXT("score")));
	TestEqual(TEXT("args"), Args, FString(TEXT("[{\"home\":2},\"x\"]")));

	TestTrue(TEXT("decode connect"), FCourierSocketIO::Decode(TEXT("0{\"sid\":\"abc\"}"), Packet) && Packet.Type == 0 && Packet.Namespace == TEXT("/") && Packet.Payload == TEXT("{\"sid\":\"abc\"}"));
	TestTrue(TEXT("decode ack"), FCourierSocketIO::Decode(TEXT("33[\"ok\"]"), Packet) && Packet.Type == 3 && Packet.AckId == 3 && Packet.Payload == TEXT("[\"ok\"]"));
	TestTrue(TEXT("binary header skipped"), FCourierSocketIO::Decode(TEXT("51-/m,[\"img\",{\"_placeholder\":true,\"num\":0}]"), Packet) && Packet.Type == 5 && Packet.Namespace == TEXT("/m"));
	TestFalse(TEXT("garbage"), FCourierSocketIO::Decode(TEXT("x"), Packet));

	TestEqual(TEXT("json arg"), FCourierSocketIO::ArgsFromText(TEXT("{\"a\": 1}")), FString(TEXT("[{\"a\":1}]")));
	TestEqual(TEXT("text arg"), FCourierSocketIO::ArgsFromText(TEXT("hello there")), FString(TEXT("[\"hello there\"]")));
	TestEqual(TEXT("number arg"), FCourierSocketIO::ArgsFromText(TEXT("42")), FString(TEXT("[42]")));
	TestEqual(TEXT("no arg"), FCourierSocketIO::ArgsFromText(TEXT("  ")), FString(TEXT("[]")));
	return true;
}

#endif
