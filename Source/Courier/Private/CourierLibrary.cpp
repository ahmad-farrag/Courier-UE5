#include "CourierLibrary.h"

#include "CourierHttp.h"
#include "CourierPostman.h"
#include "CourierRequestAsset.h"
#include "CourierSendRequest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace CourierJson
{
	TSharedPtr<FJsonValue> Parse(const FString& Json)
	{
		TSharedPtr<FJsonValue> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, Root) ? Root : nullptr;
	}

	/** Walks "a.b[2].c". An empty path is the root. */
	TSharedPtr<FJsonValue> Walk(const TSharedPtr<FJsonValue>& Root, const FString& Path)
	{
		TSharedPtr<FJsonValue> Current = Root;
		int32 Index = 0;
		const int32 Len = Path.Len();
		while (Current.IsValid() && Index < Len)
		{
			const TCHAR C = Path[Index];
			if (C == TEXT('.'))
			{
				++Index;
				continue;
			}
			if (C == TEXT('['))
			{
				const int32 Close = Path.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index);
				if (Close == INDEX_NONE)
				{
					return nullptr;
				}
				const int32 ArrayIndex = FCString::Atoi(*Path.Mid(Index + 1, Close - Index - 1));
				const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
				if (!Current->TryGetArray(Array) || !Array || !Array->IsValidIndex(ArrayIndex))
				{
					return nullptr;
				}
				Current = (*Array)[ArrayIndex];
				Index = Close + 1;
				continue;
			}
			int32 End = Index;
			while (End < Len && Path[End] != TEXT('.') && Path[End] != TEXT('['))
			{
				++End;
			}
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Current->TryGetObject(Object) || !Object || !Object->IsValid())
			{
				return nullptr;
			}
			Current = (*Object)->TryGetField(Path.Mid(Index, End - Index));
			Index = End;
		}
		return Current;
	}

	FString Serialize(const TSharedPtr<FJsonValue>& Value, bool bPretty)
	{
		FString Out;
		if (!Value.IsValid())
		{
			return Out;
		}
		if (bPretty)
		{
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Value, FString(), Writer);
		}
		else
		{
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Value, FString(), Writer);
		}
		return Out;
	}
}

UCourierSendRequest* UCourierLibrary::SendCourierRequestWithCallback(UObject* WorldContextObject, const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables, FCourierResponseDelegate OnComplete)
{
	UCourierSendRequest* Action = UCourierSendRequest::SendCourierRequest(WorldContextObject, Request, Variables);
	Action->SetCallback(OnComplete);
	Action->Activate();
	return Action;
}

UCourierSendRequest* UCourierLibrary::SendCourierRequestAssetWithCallback(UObject* WorldContextObject, UCourierRequestAsset* RequestAsset, const TMap<FString, FString>& Variables, FCourierResponseDelegate OnComplete)
{
	UCourierSendRequest* Action = UCourierSendRequest::SendRequestAsset(WorldContextObject, RequestAsset, Variables);
	Action->SetCallback(OnComplete);
	Action->Activate();
	return Action;
}

UCourierSendRequest* UCourierLibrary::CreateCourierRequest(UObject* WorldContextObject, const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables)
{
	return UCourierSendRequest::SendCourierRequest(WorldContextObject, Request, Variables);
}

FString UCourierLibrary::ResolveCourierVariables(const FString& Text, const TMap<FString, FString>& Variables)
{
	return FCourierHttp::ResolveVariables(Text, Variables);
}

FString UCourierLibrary::CourierRequestToCurl(const FCourierRequestSpec& Request, const TMap<FString, FString>& Variables)
{
	return FCourierHttp::ToCurl(Request, Variables);
}

TArray<FString> UCourierLibrary::GetCourierRequestVariableNames(const FCourierRequestSpec& Request)
{
	return FCourierHttp::FindVariableNames(Request);
}

bool UCourierLibrary::FindCourierRequest(const FCourierCollection& Collection, const FString& Name, const FString& Folder, FCourierRequestSpec& OutRequest)
{
	const FCourierRequestSpec* Found = Collection.Requests.FindByPredicate([&](const FCourierRequestSpec& Spec)
	{
		return Spec.Name == Name && (Folder.IsEmpty() || Spec.Folder == Folder);
	});
	if (Found)
	{
		OutRequest = *Found;
	}
	return Found != nullptr;
}

bool UCourierLibrary::GetCourierResponseHeader(const FCourierResponse& Response, const FString& Name, FString& OutValue)
{
	for (const TPair<FString, FString>& Header : Response.Headers)
	{
		if (Header.Key.Equals(Name, ESearchCase::IgnoreCase))
		{
			OutValue = Header.Value;
			return true;
		}
	}
	OutValue.Reset();
	return false;
}

bool UCourierLibrary::IsValidJson(const FString& Json)
{
	return CourierJson::Parse(Json).IsValid();
}

FString UCourierLibrary::PrettyPrintJson(const FString& Json)
{
	const TSharedPtr<FJsonValue> Root = CourierJson::Parse(Json);
	return Root.IsValid() ? CourierJson::Serialize(Root, true) : Json;
}

FString UCourierLibrary::CondenseJson(const FString& Json)
{
	const TSharedPtr<FJsonValue> Root = CourierJson::Parse(Json);
	return Root.IsValid() ? CourierJson::Serialize(Root, false) : Json;
}

bool UCourierLibrary::GetJsonString(const FString& Json, const FString& Path, FString& OutValue)
{
	OutValue.Reset();
	const TSharedPtr<FJsonValue> Value = CourierJson::Walk(CourierJson::Parse(Json), Path);
	if (!Value.IsValid())
	{
		return false;
	}
	switch (Value->Type)
	{
	case EJson::String:
	case EJson::Number:
	case EJson::Boolean:
		OutValue = Value->AsString();
		return true;
	case EJson::Null:
		return true;
	default:
		OutValue = CourierJson::Serialize(Value, false);
		return true;
	}
}

bool UCourierLibrary::GetJsonNumber(const FString& Json, const FString& Path, double& OutValue)
{
	OutValue = 0.0;
	const TSharedPtr<FJsonValue> Value = CourierJson::Walk(CourierJson::Parse(Json), Path);
	return Value.IsValid() && Value->TryGetNumber(OutValue);
}

bool UCourierLibrary::GetJsonBool(const FString& Json, const FString& Path, bool& OutValue)
{
	OutValue = false;
	const TSharedPtr<FJsonValue> Value = CourierJson::Walk(CourierJson::Parse(Json), Path);
	return Value.IsValid() && Value->TryGetBool(OutValue);
}

int32 UCourierLibrary::GetJsonArrayLength(const FString& Json, const FString& Path)
{
	const TSharedPtr<FJsonValue> Value = CourierJson::Walk(CourierJson::Parse(Json), Path);
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	return Value.IsValid() && Value->TryGetArray(Array) && Array ? Array->Num() : -1;
}

TArray<FString> UCourierLibrary::GetJsonKeys(const FString& Json, const FString& Path)
{
	TArray<FString> Keys;
	const TSharedPtr<FJsonValue> Value = CourierJson::Walk(CourierJson::Parse(Json), Path);
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (Value.IsValid() && Value->TryGetObject(Object) && Object && Object->IsValid())
	{
		(*Object)->Values.GetKeys(Keys);
	}
	return Keys;
}

FString UCourierLibrary::MakeJsonObject(const TMap<FString, FString>& Fields, bool bParseValues)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Field : Fields)
	{
		TSharedPtr<FJsonValue> Parsed = bParseValues ? CourierJson::Parse(Field.Value) : nullptr;
		Object->SetField(Field.Key, Parsed.IsValid() ? Parsed : MakeShared<FJsonValueString>(Field.Value));
	}
	return CourierJson::Serialize(MakeShared<FJsonValueObject>(Object), false);
}

bool UCourierLibrary::ImportPostmanCollection(const FString& Json, FCourierCollection& OutCollection, FString& OutError)
{
	return FCourierPostman::ImportCollection(Json, OutCollection, OutError);
}

FString UCourierLibrary::ExportPostmanCollection(const FCourierCollection& Collection)
{
	return FCourierPostman::ExportCollection(Collection);
}

bool UCourierLibrary::ImportPostmanEnvironment(const FString& Json, FCourierEnvironment& OutEnvironment, FString& OutError)
{
	return FCourierPostman::ImportEnvironment(Json, OutEnvironment, OutError);
}

FString UCourierLibrary::ExportPostmanEnvironment(const FCourierEnvironment& Environment)
{
	return FCourierPostman::ExportEnvironment(Environment);
}

UCourierSendRequest* UCourierLibrary::SendCourierRequestAsset(UObject* WorldContextObject, UCourierRequestAsset* Request, const TMap<FString, FString>& Variables)
{
	return UCourierSendRequest::SendRequestAsset(WorldContextObject, Request, Variables);
}

TMap<FString, FString> UCourierLibrary::AddCourierVariable(const TMap<FString, FString>& Variables, const FString& Name, const FString& Value)
{
	TMap<FString, FString> Out = Variables;
	Out.Add(Name, Value);
	return Out;
}
