#include "CourierWorkspace.h"

#include "CourierHttp.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FCourierWorkspace& FCourierWorkspace::Get()
{
	static FCourierWorkspace Instance;
	if (!Instance.bLoaded)
	{
		Instance.Load();
	}
	return Instance;
}

FString FCourierWorkspace::GetFilePath() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Courier") / TEXT("Workspace.json"));
}

void FCourierWorkspace::Load()
{
	bLoaded = true;
	Data = FCourierWorkspaceData();
	FString Json;
	if (FFileHelper::LoadFileToString(Json, *GetFilePath()))
	{
		if (!FJsonObjectConverter::JsonObjectStringToUStruct(Json, &Data, 0, 0))
		{
			UE_LOG(LogCourier, Warning, TEXT("Courier: could not read %s; starting with an empty workspace (the file is kept as .bak)."), *GetFilePath());
			IFileManager::Get().Copy(*(GetFilePath() + TEXT(".bak")), *GetFilePath());
			Data = FCourierWorkspaceData();
		}
	}

	// Ids are how the window finds things; old or hand-written files may lack them.
	for (FCourierCollection& Collection : Data.Collections)
	{
		if (!Collection.Id.IsValid()) { Collection.Id = FGuid::NewGuid(); }
		for (FCourierRequestSpec& Spec : Collection.Requests)
		{
			if (!Spec.Id.IsValid()) { Spec.Id = FGuid::NewGuid(); }
		}
	}
	for (FCourierEnvironment& Environment : Data.Environments)
	{
		if (!Environment.Id.IsValid()) { Environment.Id = FGuid::NewGuid(); }
	}
}

void FCourierWorkspace::Save()
{
	FString Json;
	if (FJsonObjectConverter::UStructToJsonObjectString(Data, Json))
	{
		FFileHelper::SaveStringToFile(Json, *GetFilePath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	OnChanged.Broadcast();
}

FCourierCollection* FCourierWorkspace::FindCollection(const FGuid& Id)
{
	return Id.IsValid() ? Data.Collections.FindByPredicate([&Id](const FCourierCollection& Collection) { return Collection.Id == Id; }) : nullptr;
}

FCourierEnvironment* FCourierWorkspace::FindEnvironment(const FGuid& Id)
{
	return Id.IsValid() ? Data.Environments.FindByPredicate([&Id](const FCourierEnvironment& Environment) { return Environment.Id == Id; }) : nullptr;
}

FCourierRequestSpec* FCourierWorkspace::FindRequest(const FGuid& RequestId, FCourierCollection** OutCollection)
{
	if (!RequestId.IsValid())
	{
		return nullptr;
	}
	for (FCourierCollection& Collection : Data.Collections)
	{
		if (FCourierRequestSpec* Spec = Collection.Requests.FindByPredicate([&RequestId](const FCourierRequestSpec& Item) { return Item.Id == RequestId; }))
		{
			if (OutCollection)
			{
				*OutCollection = &Collection;
			}
			return Spec;
		}
	}
	return nullptr;
}

TMap<FString, FString> FCourierWorkspace::GetVariables(const FGuid& CollectionId)
{
	TMap<FString, FString> Variables = FCourierHttp::ToVariableMap(Data.Globals);
	if (const FCourierCollection* Collection = FindCollection(CollectionId))
	{
		Variables.Append(FCourierHttp::ToVariableMap(Collection->Variables));
	}
	if (const FCourierEnvironment* Environment = GetActiveEnvironment())
	{
		Variables.Append(FCourierHttp::ToVariableMap(Environment->Variables));
	}
	return Variables;
}

FCourierCollection& FCourierWorkspace::AddCollection(const FString& Name)
{
	FCourierCollection& Collection = Data.Collections.AddDefaulted_GetRef();
	Collection.Id = FGuid::NewGuid();
	Collection.Name = Name;
	return Collection;
}

FCourierRequestSpec* FCourierWorkspace::StoreRequest(const FGuid& CollectionId, const FCourierRequestSpec& Spec)
{
	FCourierCollection* Collection = FindCollection(CollectionId);
	if (!Collection)
	{
		return nullptr;
	}
	// A request lives in one collection: moving it removes the old copy.
	FCourierCollection* Owner = nullptr;
	if (FCourierRequestSpec* Existing = FindRequest(Spec.Id, &Owner))
	{
		if (Owner == Collection)
		{
			*Existing = Spec;
			return Existing;
		}
		RemoveRequest(Spec.Id);
	}
	FCourierRequestSpec& Stored = Collection->Requests.Add_GetRef(Spec);
	if (!Stored.Id.IsValid())
	{
		Stored.Id = FGuid::NewGuid();
	}
	return &Stored;
}

void FCourierWorkspace::RemoveRequest(const FGuid& RequestId)
{
	for (FCourierCollection& Collection : Data.Collections)
	{
		Collection.Requests.RemoveAll([&RequestId](const FCourierRequestSpec& Spec) { return Spec.Id == RequestId; });
	}
}

void FCourierWorkspace::AddHistory(const FCourierRequestSpec& Spec, const FCourierResponse& Response)
{
	FCourierHistoryEntry Entry;
	Entry.Request = Spec;
	Entry.StatusCode = Response.StatusCode;
	Entry.ElapsedSeconds = Response.ElapsedSeconds;
	Entry.Error = Response.bReceivedResponse ? FString() : Response.Error;
	Entry.Time = FDateTime::Now();
	Data.History.Insert(MoveTemp(Entry), 0);
	if (Data.History.Num() > MaxHistory)
	{
		Data.History.SetNum(MaxHistory);
	}
	Save();
}
