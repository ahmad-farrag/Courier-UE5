#include "CourierEventSource.h"
#include "CourierLibrary.h"
#include "CourierLiveTestListener.h"
#include "CourierSendRequest.h"
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

#endif
