#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CourierLibrary.generated.h"

class UCourierRequestAsset;
class UCourierSendRequest;

UCLASS()
class COURIER_API UCourierLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ---- Sending with a delegate (usable inside functions, where latent nodes are not) ----

	/** Sends the request and calls OnComplete once, on the game thread, whatever the outcome. */
	UFUNCTION(BlueprintCallable, Category = "Courier|HTTP", meta = (WorldContext = "WorldContextObject", AutoCreateRefTerm = "Variables"))
	static UCourierSendRequest* SendCourierRequestWithCallback(UObject* WorldContextObject, const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables, FCourierResponseDelegate OnComplete);

	/** Sends a saved request asset and calls OnComplete once. */
	UFUNCTION(BlueprintCallable, Category = "Courier|HTTP", meta = (WorldContext = "WorldContextObject", AutoCreateRefTerm = "Variables"))
	static UCourierSendRequest* SendCourierRequestAssetWithCallback(UObject* WorldContextObject, UCourierRequestAsset* RequestAsset, const TMap<FString, FString>& Variables, FCourierResponseDelegate OnComplete);

	/**
	 * The request task without sending it: bind On Success / On Failure, then call Activate. For Python and
	 * editor scripts (the async nodes are Blueprint-only).
	 */
	UFUNCTION(BlueprintCallable, Category = "Courier|HTTP", meta = (WorldContext = "WorldContextObject", AutoCreateRefTerm = "Variables"))
	static UCourierSendRequest* CreateCourierRequest(UObject* WorldContextObject, const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables);

	// ---- Requests ----

	UFUNCTION(BlueprintPure, Category = "Courier|Request", meta = (AutoCreateRefTerm = "Variables"))
	static FString ResolveCourierVariables(const FString& Text, const TMap<FString, FString>& Variables);

	/** The request as a cURL command line. */
	UFUNCTION(BlueprintPure, Category = "Courier|Request", meta = (AutoCreateRefTerm = "Variables"))
	static FString CourierRequestToCurl(const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables);

	UFUNCTION(BlueprintPure, Category = "Courier|Request")
	static TArray<FString> GetCourierRequestVariableNames(const FCourierRequestSpec& Request);

	/** A request from a collection, by name (and folder, when given). */
	UFUNCTION(BlueprintPure, Category = "Courier|Request")
	static bool FindCourierRequest(const FCourierCollection& Collection, const FString& Name, const FString& Folder, FCourierRequestSpec& OutRequest);

	/** A header of the response; names ignore case. */
	UFUNCTION(BlueprintPure, Category = "Courier|Response")
	static bool GetCourierResponseHeader(const FCourierResponse& Response, const FString& Name, FString& OutValue);

	// ---- JSON (paths look like "data.users[0].name"; "[2]" indexes a top-level array) ----

	UFUNCTION(BlueprintPure, Category = "Courier|JSON")
	static bool IsValidJson(const FString& Json);

	UFUNCTION(BlueprintPure, Category = "Courier|JSON")
	static FString PrettyPrintJson(const FString& Json);

	UFUNCTION(BlueprintPure, Category = "Courier|JSON")
	static FString CondenseJson(const FString& Json);

	/** Strings as is, numbers and bools as text, objects and arrays as JSON text. */
	UFUNCTION(BlueprintPure, Category = "Courier|JSON")
	static bool GetJsonString(const FString& Json, const FString& Path, FString& OutValue);

	UFUNCTION(BlueprintPure, Category = "Courier|JSON")
	static bool GetJsonNumber(const FString& Json, const FString& Path, double& OutValue);

	UFUNCTION(BlueprintPure, Category = "Courier|JSON")
	static bool GetJsonBool(const FString& Json, const FString& Path, bool& OutValue);

	/** Element count of the array at Path, or -1. */
	UFUNCTION(BlueprintPure, Category = "Courier|JSON")
	static int32 GetJsonArrayLength(const FString& Json, const FString& Path);

	/** The object at Path's field names. */
	UFUNCTION(BlueprintPure, Category = "Courier|JSON")
	static TArray<FString> GetJsonKeys(const FString& Json, const FString& Path);

	/** {"key": "value", ...}; values that are valid JSON (numbers, objects, true) are kept as JSON. */
	UFUNCTION(BlueprintPure, Category = "Courier|JSON")
	static FString MakeJsonObject(const TMap<FString, FString>& Fields, bool bParseValues = true);

	// ---- Postman ----

	/** Reads a Postman collection (v2.0 / v2.1 JSON). */
	UFUNCTION(BlueprintCallable, Category = "Courier|Postman")
	static bool ImportPostmanCollection(const FString& Json, FCourierCollection& OutCollection, FString& OutError);

	UFUNCTION(BlueprintPure, Category = "Courier|Postman")
	static FString ExportPostmanCollection(const FCourierCollection& Collection);

	UFUNCTION(BlueprintCallable, Category = "Courier|Postman")
	static bool ImportPostmanEnvironment(const FString& Json, FCourierEnvironment& OutEnvironment, FString& OutError);

	UFUNCTION(BlueprintPure, Category = "Courier|Postman")
	static FString ExportPostmanEnvironment(const FCourierEnvironment& Environment);

	// ---- Cookies (one jar for every Courier request in the process) ----

	/** Cookies that would be sent to Url; every cookie when Url is empty. */
	UFUNCTION(BlueprintPure, Category = "Courier|Cookies")
	static TArray<FCourierCookie> GetCourierCookies(const FString& Url);

	/** The Cookie header Courier would send to Url. */
	UFUNCTION(BlueprintPure, Category = "Courier|Cookies")
	static FString GetCourierCookieHeader(const FString& Url);

	/** Adds or replaces a cookie (same domain, path and name). */
	UFUNCTION(BlueprintCallable, Category = "Courier|Cookies")
	static void SetCourierCookie(const FCourierCookie& Cookie);

	/** Removes the cookies of Domain, or just the one called Name. Returns how many were removed. */
	UFUNCTION(BlueprintCallable, Category = "Courier|Cookies")
	static int32 RemoveCourierCookies(const FString& Domain, const FString& Name);

	UFUNCTION(BlueprintCallable, Category = "Courier|Cookies")
	static void ClearCourierCookies();

	/** Writes the jar (minus expired cookies) as JSON, e.g. to keep a login across game sessions. */
	UFUNCTION(BlueprintCallable, Category = "Courier|Cookies")
	static bool SaveCourierCookies(const FString& FilePath);

	/** Adds cookies saved by Save Courier Cookies. */
	UFUNCTION(BlueprintCallable, Category = "Courier|Cookies")
	static bool LoadCourierCookies(const FString& FilePath);

	// ---- OAuth 2.0 ----

	/** The cached token for these settings, if Courier has one (valid or expired). */
	UFUNCTION(BlueprintCallable, Category = "Courier|OAuth2", meta = (AutoCreateRefTerm = "Variables"))
	static bool GetCachedCourierOAuth2Token(const FCourierOAuth2& Settings, const TMap<FString, FString>& Variables, FCourierOAuth2Token& OutToken);

	/** Forgets every cached OAuth 2.0 token (for example on sign-out). */
	UFUNCTION(BlueprintCallable, Category = "Courier|OAuth2")
	static void ClearCourierOAuth2Tokens();

	// ---- Used by the "Courier: <asset>" Blueprint node ----

	UFUNCTION(BlueprintCallable, Category = "Courier|Internal", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Variables"))
	static UCourierSendRequest* SendCourierRequestAsset(UObject* WorldContextObject, UCourierRequestAsset* Request, const TMap<FString, FString>& Variables);

	UFUNCTION(BlueprintPure, Category = "Courier|Internal", meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "Variables"))
	static TMap<FString, FString> AddCourierVariable(const TMap<FString, FString>& Variables, const FString& Name, const FString& Value);
};
