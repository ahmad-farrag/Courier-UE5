#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"
#include "CourierWorkspace.generated.h"

USTRUCT()
struct FCourierHistoryEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FCourierRequestSpec Request;

	UPROPERTY()
	int32 StatusCode = 0;

	UPROPERTY()
	float ElapsedSeconds = 0.f;

	UPROPERTY()
	FString Error;

	UPROPERTY()
	FDateTime Time;
};

/** Everything the Courier window keeps between sessions. */
USTRUCT()
struct FCourierWorkspaceData
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FCourierCollection> Collections;

	UPROPERTY()
	TArray<FCourierEnvironment> Environments;

	/** Variables every request sees, below collection and environment variables. */
	UPROPERTY()
	TArray<FCourierKeyValue> Globals;

	UPROPERTY()
	FGuid ActiveEnvironment;

	/** Newest first. */
	UPROPERTY()
	TArray<FCourierHistoryEntry> History;
};

/**
 * The editor's collections, environments and history, stored as JSON in Saved/Courier/Workspace.json.
 * Share collections with your team by exporting them (Postman v2.1), not through this file.
 */
class FCourierWorkspace
{
public:
	static FCourierWorkspace& Get();

	FCourierWorkspaceData Data;

	/** Fires after any change made through this class (and after Save()). */
	FSimpleMulticastDelegate OnChanged;

	void Load();
	void Save();
	FString GetFilePath() const;

	FCourierCollection* FindCollection(const FGuid& Id);
	FCourierEnvironment* FindEnvironment(const FGuid& Id);
	FCourierEnvironment* GetActiveEnvironment() { return FindEnvironment(Data.ActiveEnvironment); }
	/** The request and the collection that owns it. */
	FCourierRequestSpec* FindRequest(const FGuid& RequestId, FCourierCollection** OutCollection = nullptr);

	/** Globals, then the collection's variables, then the active environment's: later wins, as in Postman. */
	TMap<FString, FString> GetVariables(const FGuid& CollectionId);

	FCourierCollection& AddCollection(const FString& Name);
	/** Adds or replaces (by Id) the request in the collection. Returns the stored copy. */
	FCourierRequestSpec* StoreRequest(const FGuid& CollectionId, const FCourierRequestSpec& Spec);
	void RemoveRequest(const FGuid& RequestId);
	void AddHistory(const FCourierRequestSpec& Spec, const FCourierResponse& Response);

	static constexpr int32 MaxHistory = 200;

private:
	bool bLoaded = false;
};
