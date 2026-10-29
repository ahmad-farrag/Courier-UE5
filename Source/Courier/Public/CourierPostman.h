#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"

/**
 * Postman collection (v2.0 and v2.1) and environment JSON. Folders become FCourierRequestSpec::Folder
 * paths; collection-level auth is copied onto requests that inherit it. WebSocket and SSE requests
 * export as GET items with a "courier" extension field, which Postman ignores and Courier reads back.
 */
class COURIER_API FCourierPostman
{
public:
	static bool ImportCollection(const FString& Json, FCourierCollection& OutCollection, FString& OutError);
	static FString ExportCollection(const FCourierCollection& Collection);

	static bool ImportEnvironment(const FString& Json, FCourierEnvironment& OutEnvironment, FString& OutError);
	static FString ExportEnvironment(const FCourierEnvironment& Environment);

	/** True when the JSON looks like a Postman environment rather than a collection. */
	static bool IsEnvironmentJson(const FString& Json);
};
