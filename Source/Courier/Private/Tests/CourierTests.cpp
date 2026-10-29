#include "CourierEventSource.h"
#include "CourierHttp.h"
#include "CourierLibrary.h"
#include "CourierPostman.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace CourierTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString BodyText(const FCourierPreparedRequest& Prepared)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Prepared.Body.GetData()), Prepared.Body.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	const FString* FindHeader(const FCourierPreparedRequest& Prepared, const FString& Name)
	{
		const TPair<FString, FString>* Found = Prepared.Headers.FindByPredicate([&Name](const TPair<FString, FString>& Header) { return Header.Key == Name; });
		return Found ? &Found->Value : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierVariablesTest, "Courier.Variables", CourierTests::Flags)
bool FCourierVariablesTest::RunTest(const FString& Parameters)
{
	const TMap<FString, FString> Vars = { { TEXT("host"), TEXT("api.example.com") }, { TEXT("base"), TEXT("https://{{host}}/v1") }, { TEXT("id"), TEXT("42") } };
	TestEqual(TEXT("simple"), FCourierHttp::ResolveVariables(TEXT("/users/{{id}}"), Vars), FString(TEXT("/users/42")));
	TestEqual(TEXT("nested"), FCourierHttp::ResolveVariables(TEXT("{{base}}/users/{{ id }}"), Vars), FString(TEXT("https://api.example.com/v1/users/42")));
	TestEqual(TEXT("unknown kept"), FCourierHttp::ResolveVariables(TEXT("{{missing}}/x"), Vars), FString(TEXT("{{missing}}/x")));
	TestEqual(TEXT("unclosed kept"), FCourierHttp::ResolveVariables(TEXT("a {{id"), Vars), FString(TEXT("a {{id")));
	const FString Guid = FCourierHttp::ResolveVariables(TEXT("{{$guid}}"), Vars);
	TestEqual(TEXT("$guid length"), Guid.Len(), 36);
	TestTrue(TEXT("$timestamp numeric"), FCourierHttp::ResolveVariables(TEXT("{{$timestamp}}"), Vars).IsNumeric());

	FCourierRequestSpec Spec;
	Spec.Url = TEXT("{{base}}/users/{{id}}");
	Spec.Headers.Emplace(TEXT("X-Token"), TEXT("{{token}}"));
	Spec.Headers.Emplace(TEXT("X-Off"), TEXT("{{disabled}}"), false);
	Spec.BodyType = ECourierBodyType::Json;
	Spec.Body = TEXT("{\"when\": \"{{$timestamp}}\", \"who\": \"{{user}}\"}");
	const TArray<FString> Names = FCourierHttp::FindVariableNames(Spec);
	TestEqual(TEXT("names"), Names, TArray<FString>({ TEXT("base"), TEXT("id"), TEXT("token"), TEXT("user") }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierPrepareTest, "Courier.Prepare", CourierTests::Flags)
bool FCourierPrepareTest::RunTest(const FString& Parameters)
{
	FCourierRequestSpec Spec;
	Spec.Verb = ECourierVerb::Post;
	Spec.Url = TEXT("example.com/search");
	Spec.QueryParams.Emplace(TEXT("q"), TEXT("a b&c"));
	Spec.QueryParams.Emplace(TEXT("off"), TEXT("1"), false);
	Spec.Auth.Type = ECourierAuthType::Basic;
	Spec.Auth.Username = TEXT("user");
	Spec.Auth.Password = TEXT("pass");
	Spec.BodyType = ECourierBodyType::FormUrlEncoded;
	FCourierFormField& Field = Spec.FormFields.AddDefaulted_GetRef();
	Field.Key = TEXT("name");
	Field.Value = TEXT("Ünï code");

	FCourierPreparedRequest Prepared;
	FString Error;
	TestTrue(TEXT("prepared"), FCourierHttp::Prepare(Spec, {}, Prepared, Error));
	TestEqual(TEXT("verb"), Prepared.Verb, FString(TEXT("POST")));
	TestEqual(TEXT("url"), Prepared.Url, FString(TEXT("http://example.com/search?q=a%20b%26c")));
	const FString* Auth = CourierTests::FindHeader(Prepared, TEXT("Authorization"));
	TestTrue(TEXT("basic auth"), Auth && *Auth == TEXT("Basic dXNlcjpwYXNz"));
	const FString* ContentType = CourierTests::FindHeader(Prepared, TEXT("Content-Type"));
	TestTrue(TEXT("form content type"), ContentType && *ContentType == TEXT("application/x-www-form-urlencoded"));
	TestEqual(TEXT("form body"), CourierTests::BodyText(Prepared), FString(TEXT("name=%C3%9Cn%C3%AF%20code")));

	FCourierRequestSpec GraphQL;
	GraphQL.Verb = ECourierVerb::Post;
	GraphQL.Url = TEXT("https://api.example.com/graphql");
	GraphQL.BodyType = ECourierBodyType::GraphQL;
	GraphQL.GraphQLQuery = TEXT("query($id: ID!) { user(id: $id) { name } }");
	GraphQL.GraphQLVariables = TEXT("{\"id\": \"{{id}}\"}");
	TestTrue(TEXT("graphql prepared"), FCourierHttp::Prepare(GraphQL, { { TEXT("id"), TEXT("7") } }, Prepared, Error));
	FString Id;
	TestTrue(TEXT("graphql variables"), UCourierLibrary::GetJsonString(CourierTests::BodyText(Prepared), TEXT("variables.id"), Id) && Id == TEXT("7"));

	GraphQL.GraphQLVariables = TEXT("not json");
	TestFalse(TEXT("bad graphql variables rejected"), FCourierHttp::Prepare(GraphQL, {}, Prepared, Error));

	FCourierRequestSpec ApiKey;
	ApiKey.Url = TEXT("https://x.test/a?z=1");
	ApiKey.Auth.Type = ECourierAuthType::ApiKey;
	ApiKey.Auth.ApiKeyName = TEXT("key");
	ApiKey.Auth.ApiKeyValue = TEXT("{{k}}");
	ApiKey.Auth.bApiKeyInQuery = true;
	TestEqual(TEXT("api key in query"), FCourierHttp::BuildUrl(ApiKey, { { TEXT("k"), TEXT("s3cret") } }), FString(TEXT("https://x.test/a?z=1&key=s3cret")));

	FCourierRequestSpec Multipart;
	Multipart.Verb = ECourierVerb::Post;
	Multipart.Url = TEXT("https://x.test/upload");
	Multipart.BodyType = ECourierBodyType::Multipart;
	FCourierFormField& Text = Multipart.FormFields.AddDefaulted_GetRef();
	Text.Key = TEXT("title");
	Text.Value = TEXT("hello");
	TestTrue(TEXT("multipart prepared"), FCourierHttp::Prepare(Multipart, {}, Prepared, Error));
	const FString* MultipartType = CourierTests::FindHeader(Prepared, TEXT("Content-Type"));
	TestTrue(TEXT("multipart boundary"), MultipartType && MultipartType->StartsWith(TEXT("multipart/form-data; boundary=")));
	TestTrue(TEXT("multipart part"), CourierTests::BodyText(Prepared).Contains(TEXT("Content-Disposition: form-data; name=\"title\"\r\n\r\nhello\r\n")));

	FCourierFormField& Missing = Multipart.FormFields.AddDefaulted_GetRef();
	Missing.Key = TEXT("file");
	Missing.bIsFile = true;
	Missing.Value = TEXT("Z:/does/not/exist.bin");
	TestFalse(TEXT("missing file rejected"), FCourierHttp::Prepare(Multipart, {}, Prepared, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierCurlTest, "Courier.Curl", CourierTests::Flags)
bool FCourierCurlTest::RunTest(const FString& Parameters)
{
	FCourierRequestSpec Spec;
	Spec.Verb = ECourierVerb::Put;
	Spec.Url = TEXT("https://x.test/items/1");
	Spec.Headers.Emplace(TEXT("Accept"), TEXT("application/json"));
	Spec.BodyType = ECourierBodyType::Json;
	Spec.Body = TEXT("{\"name\":\"it's\"}");
	const FString Curl = FCourierHttp::ToCurl(Spec, {});
	TestTrue(TEXT("verb"), Curl.StartsWith(TEXT("curl -X PUT 'https://x.test/items/1'")));
	TestTrue(TEXT("header"), Curl.Contains(TEXT("-H 'Accept: application/json'")));
	TestTrue(TEXT("json content type"), Curl.Contains(TEXT("-H 'Content-Type: application/json'")));
	TestTrue(TEXT("quoted body"), Curl.Contains(TEXT("--data-raw '{\"name\":\"it'\\''s\"}'")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierJsonTest, "Courier.Json", CourierTests::Flags)
bool FCourierJsonTest::RunTest(const FString& Parameters)
{
	const FString Json = TEXT("{\"data\":{\"users\":[{\"name\":\"Ada\",\"age\":36,\"admin\":true},{\"name\":\"Bob\"}]},\"top\":[1,2]}");
	FString Name;
	TestTrue(TEXT("string path"), UCourierLibrary::GetJsonString(Json, TEXT("data.users[1].name"), Name) && Name == TEXT("Bob"));
	double Age = 0.0;
	TestTrue(TEXT("number path"), UCourierLibrary::GetJsonNumber(Json, TEXT("data.users[0].age"), Age) && Age == 36.0);
	bool bAdmin = false;
	TestTrue(TEXT("bool path"), UCourierLibrary::GetJsonBool(Json, TEXT("data.users[0].admin"), bAdmin) && bAdmin);
	TestEqual(TEXT("array length"), UCourierLibrary::GetJsonArrayLength(Json, TEXT("data.users")), 2);
	TestEqual(TEXT("missing path"), UCourierLibrary::GetJsonArrayLength(Json, TEXT("data.nope")), -1);
	FString Object;
	TestTrue(TEXT("object as json"), UCourierLibrary::GetJsonString(Json, TEXT("data.users[1]"), Object) && Object == TEXT("{\"name\":\"Bob\"}"));
	TestEqual(TEXT("top-level array"), UCourierLibrary::GetJsonArrayLength(TEXT("[[1,2,3]]"), TEXT("[0]")), 3);
	TestEqual(TEXT("keys"), UCourierLibrary::GetJsonKeys(Json, FString()).Num(), 2);
	TestFalse(TEXT("invalid json"), UCourierLibrary::IsValidJson(TEXT("{nope")));
	const FString Made = UCourierLibrary::MakeJsonObject({ { TEXT("n"), TEXT("5") }, { TEXT("s"), TEXT("hi") } }, true);
	double N = 0.0;
	TestTrue(TEXT("made number"), UCourierLibrary::GetJsonNumber(Made, TEXT("n"), N) && N == 5.0);
	TestTrue(TEXT("made string"), UCourierLibrary::GetJsonString(Made, TEXT("s"), Name) && Name == TEXT("hi"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierPostmanTest, "Courier.Postman", CourierTests::Flags)
bool FCourierPostmanTest::RunTest(const FString& Parameters)
{
	const FString Collection = TEXT(R"JSON({
		"info": { "name": "Demo API", "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json" },
		"auth": { "type": "bearer", "bearer": [ { "key": "token", "value": "{{token}}", "type": "string" } ] },
		"variable": [ { "key": "baseUrl", "value": "https://api.example.com" }, { "key": "n", "value": 3 } ],
		"item": [
			{ "name": "Users", "item": [
				{ "name": "List users", "request": { "method": "GET", "header": [],
					"url": { "raw": "{{baseUrl}}/users?page=1&limit=10", "host": ["{{baseUrl}}"], "path": ["users"],
						"query": [ { "key": "page", "value": "1" }, { "key": "limit", "value": "10", "disabled": true } ] } } },
				{ "name": "Create user", "request": { "method": "POST",
					"header": [ { "key": "Content-Type", "value": "application/json" } ],
					"body": { "mode": "raw", "raw": "{\"name\":\"Ada\"}", "options": { "raw": { "language": "json" } } },
					"auth": { "type": "noauth" },
					"url": "{{baseUrl}}/users" } },
				{ "name": "Empty", "item": [] }
			] },
			{ "name": "Upload", "request": { "method": "POST",
				"body": { "mode": "formdata", "formdata": [ { "key": "title", "value": "x", "type": "text" }, { "key": "file", "type": "file", "src": "C:/a.png" } ] },
				"url": "{{baseUrl}}/upload" } },
			{ "name": "Graph", "request": { "method": "POST",
				"body": { "mode": "graphql", "graphql": { "query": "{ me { id } }", "variables": "{}" } },
				"url": "{{baseUrl}}/graphql" } },
			{ "name": "Legacy auth", "request": { "method": "GET",
				"auth": { "type": "basic", "basic": { "username": "u", "password": "p" } },
				"url": "{{baseUrl}}/legacy" } }
		]
	})JSON");

	FCourierCollection Imported;
	FString Error;
	if (!TestTrue(TEXT("imported"), FCourierPostman::ImportCollection(Collection, Imported, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("name"), Imported.Name, FString(TEXT("Demo API")));
	TestEqual(TEXT("variables"), Imported.Variables.Num(), 2);
	TestEqual(TEXT("numeric variable"), Imported.Variables[1].Value, FString(TEXT("3")));
	TestEqual(TEXT("request count"), Imported.Requests.Num(), 5);
	TestEqual(TEXT("empty folder kept"), Imported.Folders, TArray<FString>({ TEXT("Users/Empty") }));

	const FCourierRequestSpec& List = Imported.Requests[0];
	TestEqual(TEXT("folder"), List.Folder, FString(TEXT("Users")));
	TestEqual(TEXT("url without query"), List.Url, FString(TEXT("{{baseUrl}}/users")));
	TestEqual(TEXT("query rows"), List.QueryParams.Num(), 2);
	TestFalse(TEXT("disabled query row"), List.QueryParams[1].bEnabled);
	TestEqual(TEXT("inherited bearer"), List.Auth.Type, ECourierAuthType::Bearer);
	TestEqual(TEXT("inherited token"), List.Auth.Token, FString(TEXT("{{token}}")));

	const FCourierRequestSpec& Create = Imported.Requests[1];
	TestEqual(TEXT("post"), Create.Verb, ECourierVerb::Post);
	TestEqual(TEXT("json body"), Create.BodyType, ECourierBodyType::Json);
	TestEqual(TEXT("noauth overrides"), Create.Auth.Type, ECourierAuthType::None);

	const FCourierRequestSpec& Upload = Imported.Requests[2];
	TestEqual(TEXT("multipart"), Upload.BodyType, ECourierBodyType::Multipart);
	TestTrue(TEXT("file field"), Upload.FormFields.Num() == 2 && Upload.FormFields[1].bIsFile && Upload.FormFields[1].Value == TEXT("C:/a.png"));
	TestEqual(TEXT("graphql"), Imported.Requests[3].BodyType, ECourierBodyType::GraphQL);
	TestEqual(TEXT("v2.0 basic auth"), Imported.Requests[4].Auth.Username, FString(TEXT("u")));

	// Round trip through our own export.
	FCourierRequestSpec& Socket = Imported.Requests.AddDefaulted_GetRef();
	Socket.Name = TEXT("Live feed");
	Socket.Protocol = ECourierProtocol::WebSocket;
	Socket.Url = TEXT("wss://echo.example.com");
	Socket.WebSocketProtocols.Add(TEXT("chat"));
	const FString Exported = FCourierPostman::ExportCollection(Imported);
	FCourierCollection Again;
	TestTrue(TEXT("re-imported"), FCourierPostman::ImportCollection(Exported, Again, Error));
	TestEqual(TEXT("round trip count"), Again.Requests.Num(), Imported.Requests.Num());
	TestEqual(TEXT("round trip folders"), Again.Folders, Imported.Folders);
	for (int32 Index = 0; Index < FMath::Min(Again.Requests.Num(), Imported.Requests.Num()); ++Index)
	{
		const FCourierRequestSpec& A = Imported.Requests[Index];
		const FCourierRequestSpec& B = Again.Requests[Index];
		const FString What = FString::Printf(TEXT("round trip %s"), *A.Name);
		TestEqual(What + TEXT(" name"), B.Name, A.Name);
		TestEqual(What + TEXT(" folder"), B.Folder, A.Folder);
		TestEqual(What + TEXT(" url"), B.Url, A.Url);
		TestEqual(What + TEXT(" verb"), B.Verb, A.Verb);
		TestEqual(What + TEXT(" body type"), B.BodyType, A.BodyType);
		TestTrue(What + TEXT(" query"), B.QueryParams == A.QueryParams);
		TestEqual(What + TEXT(" auth"), B.Auth.Type, A.Auth.Type);
		TestEqual(What + TEXT(" protocol"), B.Protocol, A.Protocol);
	}

	FCourierEnvironment Environment;
	Environment.Name = TEXT("Staging");
	Environment.Variables.Emplace(TEXT("baseUrl"), TEXT("https://staging.example.com"));
	Environment.Variables.Emplace(TEXT("off"), TEXT("x"), false);
	const FString EnvironmentJson = FCourierPostman::ExportEnvironment(Environment);
	TestTrue(TEXT("is environment"), FCourierPostman::IsEnvironmentJson(EnvironmentJson));
	TestFalse(TEXT("collection is not environment"), FCourierPostman::IsEnvironmentJson(Collection));
	FCourierEnvironment EnvironmentAgain;
	TestTrue(TEXT("environment imported"), FCourierPostman::ImportEnvironment(EnvironmentJson, EnvironmentAgain, Error));
	TestTrue(TEXT("environment rows"), EnvironmentAgain.Variables == Environment.Variables);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCourierSseParserTest, "Courier.SseParser", CourierTests::Flags)
bool FCourierSseParserTest::RunTest(const FString& Parameters)
{
	const FString Stream = TEXT(": keep-alive\r\nevent: score\r\ndata: {\"home\":1}\r\nid: 7\r\n\r\ndata: line one\ndata: line two\n\nretry: 5000\ndata:no-space\r\r");
	const FTCHARToUTF8 Utf8(*Stream);
	const uint8* Bytes = reinterpret_cast<const uint8*>(Utf8.Get());

	// Byte by byte, so every split point (including inside \r\n) is exercised.
	FCourierSseParser Parser;
	TArray<FCourierSseEvent> Events;
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		Parser.Feed(Bytes + Index, 1, Events);
	}
	if (!TestEqual(TEXT("event count"), Events.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("named event"), Events[0].EventName, FString(TEXT("score")));
	TestEqual(TEXT("data"), Events[0].Data, FString(TEXT("{\"home\":1}")));
	TestEqual(TEXT("id"), Events[0].Id, FString(TEXT("7")));
	TestEqual(TEXT("default name"), Events[1].EventName, FString(TEXT("message")));
	TestEqual(TEXT("multi-line data"), Events[1].Data, FString(TEXT("line one\nline two")));
	TestEqual(TEXT("id persists"), Events[1].Id, FString(TEXT("7")));
	TestEqual(TEXT("retry"), Events[2].Retry, 5000);
	TestEqual(TEXT("no space after colon"), Events[2].Data, FString(TEXT("no-space")));
	return true;
}

#endif
