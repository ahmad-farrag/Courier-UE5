#include "CourierCookies.h"
#include "CourierEventSource.h"
#include "CourierOAuth2.h"
#include "CourierSocketIO.h"
#include "CourierLibrary.h"
#include "CourierLiveTestListener.h"
#include "CourierSendRequest.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// Courier.Live.* talk to public servers (postman-echo.com, stream.wikimedia.org), so they are not in the
// default filters: run them by name, e.g. "Automation RunTests Courier.Live".
namespace CourierLiveTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter;
	constexpr double TimeoutSeconds = 30.0;
}

DEFINE_LATENT_AUTOMATION_COMMAND_FOUR_PARAMETER(FCourierWaitFor, FAutomationTestBase*, Test, TFunction<bool()>, Done, FString, What, double, StartTime);
bool FCourierWaitFor::Update()
{
	if (Done())
	{
		return true;
	}
	if (FPlatformTime::Seconds() - StartTime > CourierLiveTests::TimeoutSeconds)
	{
		Test->AddError(FString::Printf(TEXT("Timed out waiting for %s"), *What));
		return true;
	}
	return false;
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FCourierRun, TFunction<void()>, Body);
bool FCourierRun::Update()
{
	Body();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierLiveHttpTest, "Courier.Live.Http", CourierLiveTests::Flags)
bool FCourierLiveHttpTest::RunTest(const FString& Parameters)
{
	TSharedRef<TStrongObjectPtr<UCourierLiveTestListener>> Listener = MakeShared<TStrongObjectPtr<UCourierLiveTestListener>>(NewObject<UCourierLiveTestListener>());
	UCourierLiveTestListener* L = Listener->Get();

	auto Bind = [L](UCourierSendRequest* Action)
	{
		Action->OnSuccess.AddDynamic(L, &UCourierLiveTestListener::HandleSuccess);
		Action->OnFailure.AddDynamic(L, &UCourierLiveTestListener::HandleFailure);
		Action->Send();
	};

	// GET with variables and a query, POST JSON, Basic auth: three On Success.
	FCourierRequestSpec Get;
	Get.Url = TEXT("{{base}}/get");
	Get.QueryParams.Emplace(TEXT("who"), TEXT("{{player}}"));
	Bind(UCourierLibrary::CreateCourierRequest(nullptr, Get, { { TEXT("base"), TEXT("https://postman-echo.com") }, { TEXT("player"), TEXT("Ada Lovelace") } }));
	Bind(UCourierSendRequest::CourierPostJson(nullptr, TEXT("https://postman-echo.com/post"), TEXT("{\"score\":42}"), {}));
	FCourierRequestSpec Auth;
	Auth.Url = TEXT("https://postman-echo.com/basic-auth");
	Auth.Auth.Type = ECourierAuthType::Basic;
	Auth.Auth.Username = TEXT("postman");
	Auth.Auth.Password = TEXT("password");
	Bind(UCourierLibrary::CreateCourierRequest(nullptr, Auth, {}));

	// A 404 and a DNS failure: two On Failure.
	Bind(UCourierSendRequest::CourierGet(nullptr, TEXT("https://postman-echo.com/status/404"), {}));
	Bind(UCourierSendRequest::CourierGet(nullptr, TEXT("https://no-such-host.invalid/"), {}));

	// The single-cast callback variant.
	FCourierResponseDelegate Callback;
	Callback.BindUFunction(L, GET_FUNCTION_NAME_CHECKED(UCourierLiveTestListener, HandleCallback));
	UCourierLibrary::SendCourierRequestWithCallback(nullptr, Get, { { TEXT("base"), TEXT("https://postman-echo.com") }, { TEXT("player"), TEXT("x") } }, Callback);

	ADD_LATENT_AUTOMATION_COMMAND(FCourierWaitFor(this, [L]() { return L->Successes.Num() + L->Failures.Num() >= 5 && L->Callbacks.Num() >= 1; }, TEXT("6 responses"), FPlatformTime::Seconds()));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierRun([this, Listener]()
	{
		const UCourierLiveTestListener* L = Listener->Get();
		TestEqual(TEXT("successes"), L->Successes.Num(), 3);
		TestEqual(TEXT("failures"), L->Failures.Num(), 2);
		TestEqual(TEXT("callbacks"), L->Callbacks.Num(), 1);
		for (const FCourierResponse& Response : L->Successes)
		{
			TestEqual(FString::Printf(TEXT("%s status"), *Response.Url), Response.StatusCode, 200);
		}
		const FCourierResponse* Echo = L->Successes.FindByPredicate([](const FCourierResponse& R) { return R.Url.Contains(TEXT("/get?")); });
		FString Who;
		TestTrue(TEXT("GET echoed the resolved query"), Echo && UCourierLibrary::GetJsonString(Echo->Body, TEXT("args.who"), Who) && Who == TEXT("Ada Lovelace"));
		const FCourierResponse* Post = L->Successes.FindByPredicate([](const FCourierResponse& R) { return R.Url.EndsWith(TEXT("/post")); });
		double Score = 0.0;
		TestTrue(TEXT("POST echoed the JSON body"), Post && UCourierLibrary::GetJsonNumber(Post->Body, TEXT("json.score"), Score) && Score == 42.0);
		const FCourierResponse* NotFound = L->Failures.FindByPredicate([](const FCourierResponse& R) { return R.StatusCode == 404; });
		TestTrue(TEXT("404 is a failure with a response"), NotFound && NotFound->bReceivedResponse);
		const FCourierResponse* Dns = L->Failures.FindByPredicate([](const FCourierResponse& R) { return !R.bReceivedResponse; });
		TestTrue(TEXT("DNS failure has no response and an error"), Dns && !Dns->Error.IsEmpty());
		TestTrue(TEXT("callback got a 200"), L->Callbacks.Num() == 1 && L->Callbacks[0].StatusCode == 200);
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierLiveSseTest, "Courier.Live.ServerSentEvents", CourierLiveTests::Flags)
bool FCourierLiveSseTest::RunTest(const FString& Parameters)
{
	TSharedRef<TStrongObjectPtr<UCourierLiveTestListener>> Listener = MakeShared<TStrongObjectPtr<UCourierLiveTestListener>>(NewObject<UCourierLiveTestListener>());
	TSharedRef<TStrongObjectPtr<UCourierEventSource>> Source = MakeShared<TStrongObjectPtr<UCourierEventSource>>(
		UCourierEventSource::CreateCourierEventSource(TEXT("https://stream.wikimedia.org/v2/stream/recentchange"), { { TEXT("User-Agent"), TEXT("CourierTest/1.0") } }));
	UCourierLiveTestListener* L = Listener->Get();
	UCourierEventSource* S = Source->Get();
	S->OnOpen.AddDynamic(L, &UCourierLiveTestListener::HandleSseOpen);
	S->OnEvent.AddDynamic(L, &UCourierLiveTestListener::HandleSseEvent);
	S->OnClosed.AddDynamic(L, &UCourierLiveTestListener::HandleSseClosed);
	TestTrue(TEXT("opened"), S->Open());

	ADD_LATENT_AUTOMATION_COMMAND(FCourierWaitFor(this, [L]() { return L->SseEvents.Num() >= 3 || L->bSseClosed; }, TEXT("3 SSE events"), FPlatformTime::Seconds()));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierRun([this, Listener, Source]()
	{
		const UCourierLiveTestListener* L = Listener->Get();
		Source->Get()->Close();
		TestEqual(TEXT("open status"), L->SseOpenStatus, 200);
		TestTrue(TEXT("3 events"), L->SseEvents.Num() >= 3);
		if (L->SseEvents.Num() > 0)
		{
			TestTrue(TEXT("event data is JSON"), UCourierLibrary::IsValidJson(L->SseEvents[0].Data));
			TestFalse(TEXT("event has an id"), L->SseEvents[0].Id.IsEmpty());
		}
		TestFalse(TEXT("not closed with an error"), L->bSseClosed && !L->SseClosedError.IsEmpty());
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierLiveCookiesTest, "Courier.Live.Cookies", CourierLiveTests::Flags)
bool FCourierLiveCookiesTest::RunTest(const FString& Parameters)
{
	TSharedRef<TStrongObjectPtr<UCourierLiveTestListener>> Listener = MakeShared<TStrongObjectPtr<UCourierLiveTestListener>>(NewObject<UCourierLiveTestListener>());
	UCourierLiveTestListener* L = Listener->Get();
	FCourierCookieJar::Get().Remove(TEXT("postman-echo.com"));

	// /response-headers answers 200 with the Set-Cookie header we ask for. (The engine's HTTP layer drops the
	// headers of redirect responses, so cookies set on a 3xx never reach any jar; /cookies/set redirects.)
	FCourierRequestSpec Set;
	Set.Url = TEXT("https://postman-echo.com/response-headers");
	Set.QueryParams.Emplace(TEXT("Set-Cookie"), TEXT("courier=jar; Path=/"));
	UCourierSendRequest* First = UCourierLibrary::CreateCourierRequest(nullptr, Set, {});
	First->OnSuccess.AddDynamic(L, &UCourierLiveTestListener::HandleSuccess);
	First->OnFailure.AddDynamic(L, &UCourierLiveTestListener::HandleFailure);
	First->Send();

	ADD_LATENT_AUTOMATION_COMMAND(FCourierWaitFor(this, [L]() { return L->Successes.Num() + L->Failures.Num() >= 1; }, TEXT("the cookie to be set"), FPlatformTime::Seconds()));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierRun([this, L]()
	{
		TestTrue(TEXT("jar holds the cookie"), FCourierCookieJar::Get().GetCookieHeader(TEXT("https://postman-echo.com/cookies")).Contains(TEXT("courier=jar")));
		FCourierRequestSpec Read;
		Read.Url = TEXT("https://postman-echo.com/cookies");
		UCourierSendRequest* Second = UCourierLibrary::CreateCourierRequest(nullptr, Read, {});
		Second->OnSuccess.AddDynamic(L, &UCourierLiveTestListener::HandleSuccess);
		Second->OnFailure.AddDynamic(L, &UCourierLiveTestListener::HandleFailure);
		Second->Send();
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierWaitFor(this, [L]() { return L->Successes.Num() + L->Failures.Num() >= 2; }, TEXT("the cookie echo"), FPlatformTime::Seconds()));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierRun([this, Listener]()
	{
		const UCourierLiveTestListener* L = Listener->Get();
		const FCourierResponse* Echo = L->Successes.FindByPredicate([](const FCourierResponse& R) { return R.Url.EndsWith(TEXT("/cookies")); });
		FString Value;
		TestTrue(TEXT("server saw the cookie"), Echo && UCourierLibrary::GetJsonString(Echo->Body, TEXT("cookies.courier"), Value) && Value == TEXT("jar"));
		FCourierCookieJar::Get().Remove(TEXT("postman-echo.com"));
	}));
	return true;
}

// Courier.Live.OAuth2 and Courier.Live.SocketIO need test servers: set COURIER_TEST_OAUTH_URL (a mock token server,
// see docs) and COURIER_TEST_SOCKETIO_URL. Without them the tests report that they skipped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierLiveOAuth2Test, "Courier.Live.OAuth2", CourierLiveTests::Flags)
bool FCourierLiveOAuth2Test::RunTest(const FString& Parameters)
{
	const FString Base = FPlatformMisc::GetEnvironmentVariable(TEXT("COURIER_TEST_OAUTH_URL"));
	if (Base.IsEmpty())
	{
		AddInfo(TEXT("Skipped: COURIER_TEST_OAUTH_URL is not set."));
		return true;
	}
	FCourierOAuth2Client::ClearCache();
	TSharedRef<TStrongObjectPtr<UCourierLiveTestListener>> Listener = MakeShared<TStrongObjectPtr<UCourierLiveTestListener>>(NewObject<UCourierLiveTestListener>());
	UCourierLiveTestListener* L = Listener->Get();
	auto Send = [L](const FCourierRequestSpec& Spec)
	{
		UCourierSendRequest* Action = UCourierLibrary::CreateCourierRequest(nullptr, Spec, {});
		Action->OnSuccess.AddDynamic(L, &UCourierLiveTestListener::HandleSuccess);
		Action->OnFailure.AddDynamic(L, &UCourierLiveTestListener::HandleFailure);
		Action->Send();
	};
	auto Me = [Base](const FString& Client, const FString& Secret)
	{
		FCourierRequestSpec Spec;
		Spec.Url = Base + TEXT("/me?as=") + Client;
		Spec.Auth.Type = ECourierAuthType::OAuth2;
		Spec.Auth.OAuth2.AccessTokenUrl = Base + TEXT("/token");
		Spec.Auth.OAuth2.ClientId = Client;
		Spec.Auth.OAuth2.ClientSecret = Secret;
		Spec.Auth.OAuth2.Scope = TEXT("scores");
		return Spec;
	};

	// Client credentials: Send fetches a token, then calls /me with it. A wrong secret fails before /me.
	Send(Me(TEXT("game"), TEXT("secret")));
	Send(Me(TEXT("game-bad"), TEXT("wrong")));

	// Password grant through the Blueprint async node.
	FCourierOAuth2 Password;
	Password.GrantType = ECourierOAuth2Grant::Password;
	Password.AccessTokenUrl = Base + TEXT("/token");
	Password.ClientId = TEXT("game");
	Password.ClientSecret = TEXT("secret");
	Password.ClientAuthentication = ECourierOAuth2ClientAuth::Body;
	Password.Username = TEXT("ada");
	Password.Password = TEXT("hunter2");
	UCourierOAuth2Task* Task = UCourierOAuth2Task::GetCourierOAuth2Token(nullptr, Password, {});
	Task->OnSuccess.AddDynamic(L, &UCourierLiveTestListener::HandleToken);
	Task->OnFailure.AddDynamic(L, &UCourierLiveTestListener::HandleToken);
	Task->Activate();

	ADD_LATENT_AUTOMATION_COMMAND(FCourierWaitFor(this, [L]() { return L->Successes.Num() + L->Failures.Num() >= 2 && L->Tokens.Num() >= 1; }, TEXT("token grants"), FPlatformTime::Seconds()));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierRun([this, L, Send, Me]()
	{
		const FCourierResponse* Ok = L->Successes.FindByPredicate([](const FCourierResponse& R) { return R.Url.Contains(TEXT("as=game")); });
		FString Client;
		TestTrue(TEXT("client credentials reached /me"), Ok && UCourierLibrary::GetJsonString(Ok->Body, TEXT("client"), Client) && Client == TEXT("game"));
		const FCourierResponse* Bad = L->Failures.FindByPredicate([](const FCourierResponse& R) { return R.Url.Contains(TEXT("as=game-bad")); });
		TestTrue(TEXT("bad secret fails with the OAuth error"), Bad && Bad->Error.Contains(TEXT("invalid_client")) && !Bad->bReceivedResponse);
		TestTrue(TEXT("password grant token"), L->Tokens.Num() == 1 && L->Tokens[0].bSuccess && !L->Tokens[0].RefreshToken.IsEmpty());

		// "short" tokens live 1 second: the second Send must use the refresh token.
		Send(Me(TEXT("short"), TEXT("secret")));
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierWaitFor(this, [L]() { return L->Successes.Num() >= 2; }, TEXT("the short-lived token"), FPlatformTime::Seconds()));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierRun([L, Send, Me]() { Send(Me(TEXT("short"), TEXT("secret"))); }));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierWaitFor(this, [L]() { return L->Successes.Num() + L->Failures.Num() >= 4; }, TEXT("the refreshed request"), FPlatformTime::Seconds()));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierRun([this, Listener]()
	{
		const UCourierLiveTestListener* L = Listener->Get();
		const int32 ShortOk = L->Successes.FilterByPredicate([](const FCourierResponse& R) { return R.Url.Contains(TEXT("as=short")); }).Num();
		TestEqual(TEXT("both short requests succeeded"), ShortOk, 2);
		const FCourierResponse* Last = L->Successes.Num() > 0 ? &L->Successes.Last() : nullptr;
		FString Grant;
		TestTrue(TEXT("second token came from refresh_token"), Last && UCourierLibrary::GetJsonString(Last->Body, TEXT("grant"), Grant) && Grant == TEXT("refresh_token"));
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierLiveSocketIOTest, "Courier.Live.SocketIO", CourierLiveTests::Flags)
bool FCourierLiveSocketIOTest::RunTest(const FString& Parameters)
{
	const FString Url = FPlatformMisc::GetEnvironmentVariable(TEXT("COURIER_TEST_SOCKETIO_URL"));
	if (Url.IsEmpty())
	{
		AddInfo(TEXT("Skipped: COURIER_TEST_SOCKETIO_URL is not set."));
		return true;
	}
	TSharedRef<TStrongObjectPtr<UCourierLiveTestListener>> Listener = MakeShared<TStrongObjectPtr<UCourierLiveTestListener>>(NewObject<UCourierLiveTestListener>());
	UCourierLiveTestListener* L = Listener->Get();
	auto Make = [L](const FString& InUrl, const FString& Namespace, const FString& Auth)
	{
		UCourierSocketIO* Client = UCourierSocketIO::CreateCourierSocketIO(InUrl, Namespace, Auth, {});
		Client->OnConnected.AddDynamic(L, &UCourierLiveTestListener::HandleSioConnected);
		Client->OnConnectError.AddDynamic(L, &UCourierLiveTestListener::HandleSioError);
		Client->OnDisconnected.AddDynamic(L, &UCourierLiveTestListener::HandleSioDisconnected);
		Client->OnEvent.AddDynamic(L, &UCourierLiveTestListener::HandleSioEvent);
		Client->OnAck.AddDynamic(L, &UCourierLiveTestListener::HandleSioAck);
		return MakeShared<TStrongObjectPtr<UCourierSocketIO>>(Client);
	};
	const auto Main = Make(Url, TEXT("/"), FString());
	const auto Refused = Make(Url, TEXT("/match"), TEXT("{\"token\":\"wrong\"}"));
	Main->Get()->Connect();
	Refused->Get()->Connect();

	ADD_LATENT_AUTOMATION_COMMAND(FCourierWaitFor(this, [L]() { return !L->SioSocketId.IsEmpty() && L->SioErrors.Num() >= 1; }, TEXT("connect and connect error"), FPlatformTime::Seconds()));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierRun([this, L, Main]()
	{
		TestTrue(TEXT("refused namespace reports the server's message"), L->SioErrors.Num() >= 1 && L->SioErrors[0].Contains(TEXT("unauthorized")));
		TestTrue(TEXT("emit"), Main->Get()->Emit(TEXT("echo"), TEXT("{\"x\":1}")));
		TestEqual(TEXT("ack id"), Main->Get()->EmitWithAck(TEXT("add"), TEXT("{\"a\":2,\"b\":3}")), 0);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierWaitFor(this, [L]() { return L->SioAcks.Num() >= 1 && L->SioEvents.ContainsByPredicate([](const FString& E) { return E.StartsWith(TEXT("echo ")); }); }, TEXT("echo and ack"), FPlatformTime::Seconds()));
	ADD_LATENT_AUTOMATION_COMMAND(FCourierRun([this, Listener, Main, Refused]()
	{
		const UCourierLiveTestListener* L = Listener->Get();
		TestTrue(TEXT("welcome event"), L->SioEvents.ContainsByPredicate([](const FString& E) { return E.StartsWith(TEXT("welcome [")); }));
		TestTrue(TEXT("echo came back"), L->SioEvents.Contains(TEXT("echo [{\"x\":1}]")));
		TestEqual(TEXT("ack"), L->SioAcks.Num() > 0 ? L->SioAcks[0] : FString(), FString(TEXT("0 [{\"sum\":5}]")));
		Main->Get()->Disconnect();
		Refused->Get()->Disconnect();
		TestTrue(TEXT("disconnect reported"), L->SioDisconnects.Contains(TEXT("io client disconnect")));
	}));
	return true;
}

#endif
