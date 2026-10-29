#include "CourierPostman.h"

#include "CourierHttp.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace CourierPostman
{
	const TCHAR* SchemaV21 = TEXT("https://schema.getpostman.com/json/collection/v2.1.0/collection.json");

	FString GetString(const TSharedPtr<FJsonObject>& Object, const FString& Field)
	{
		if (!Object.IsValid())
		{
			return FString();
		}
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		if (!Value.IsValid() || Value->IsNull())
		{
			return FString();
		}
		FString Out;
		if (Value->TryGetString(Out))
		{
			return Out;
		}
		// Postman sometimes writes {"content": "...", "type": "text/markdown"} for descriptions.
		const TSharedPtr<FJsonObject>* Inner = nullptr;
		if (Value->TryGetObject(Inner) && Inner)
		{
			return GetString(*Inner, TEXT("content"));
		}
		return Out;
	}

	bool GetBool(const TSharedPtr<FJsonObject>& Object, const FString& Field, bool bDefault)
	{
		bool bOut = bDefault;
		return Object.IsValid() && Object->TryGetBoolField(Field, bOut) ? bOut : bDefault;
	}

	const TArray<TSharedPtr<FJsonValue>>* GetArray(const TSharedPtr<FJsonObject>& Object, const FString& Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
		return Object.IsValid() && Object->TryGetArrayField(Field, Out) ? Out : nullptr;
	}

	TSharedPtr<FJsonObject> GetObject(const TSharedPtr<FJsonObject>& Object, const FString& Field)
	{
		const TSharedPtr<FJsonObject>* Out = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Field, Out) && Out ? *Out : nullptr;
	}

	/** A value Postman stored as a string or as another JSON type (numbers in variables). */
	FString ValueAsString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return FString();
		}
		FString Out;
		if (Value->TryGetString(Out))
		{
			return Out;
		}
		double Number = 0.0;
		if (Value->TryGetNumber(Number))
		{
			return FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)) ? FString::Printf(TEXT("%lld"), static_cast<int64>(Number)) : FString::SanitizeFloat(Number);
		}
		bool bBool = false;
		if (Value->TryGetBool(bBool))
		{
			return bBool ? TEXT("true") : TEXT("false");
		}
		return Out;
	}

	/** Reads [{key, value, disabled}] (or {enabled} in environments). */
	TArray<FCourierKeyValue> ReadKeyValues(const TArray<TSharedPtr<FJsonValue>>* Array)
	{
		TArray<FCourierKeyValue> Out;
		if (!Array)
		{
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject> Row = Entry.IsValid() ? Entry->AsObject() : nullptr;
			if (!Row.IsValid())
			{
				continue;
			}
			const bool bEnabled = !GetBool(Row, TEXT("disabled"), false) && GetBool(Row, TEXT("enabled"), true);
			Out.Emplace(GetString(Row, TEXT("key")), ValueAsString(Row->TryGetField(TEXT("value"))), bEnabled);
		}
		return Out;
	}

	/** Auth params: v2.1 is [{key, value}], v2.0 is {key: value}. */
	FString AuthParam(const TSharedPtr<FJsonObject>& Auth, const FString& Type, const FString& Key)
	{
		if (const TArray<TSharedPtr<FJsonValue>>* Params = GetArray(Auth, Type))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Params)
			{
				const TSharedPtr<FJsonObject> Row = Entry.IsValid() ? Entry->AsObject() : nullptr;
				if (Row.IsValid() && GetString(Row, TEXT("key")) == Key)
				{
					return ValueAsString(Row->TryGetField(TEXT("value")));
				}
			}
			return FString();
		}
		return GetString(GetObject(Auth, Type), Key);
	}

	/** Returns false when the object is not an auth block; noauth gives a None auth. */
	bool ReadAuth(const TSharedPtr<FJsonObject>& Auth, FCourierAuth& Out)
	{
		if (!Auth.IsValid())
		{
			return false;
		}
		const FString Type = GetString(Auth, TEXT("type")).ToLower();
		Out = FCourierAuth();
		if (Type == TEXT("bearer"))
		{
			Out.Type = ECourierAuthType::Bearer;
			Out.Token = AuthParam(Auth, TEXT("bearer"), TEXT("token"));
		}
		else if (Type == TEXT("basic"))
		{
			Out.Type = ECourierAuthType::Basic;
			Out.Username = AuthParam(Auth, TEXT("basic"), TEXT("username"));
			Out.Password = AuthParam(Auth, TEXT("basic"), TEXT("password"));
		}
		else if (Type == TEXT("apikey"))
		{
			Out.Type = ECourierAuthType::ApiKey;
			Out.ApiKeyName = AuthParam(Auth, TEXT("apikey"), TEXT("key"));
			Out.ApiKeyValue = AuthParam(Auth, TEXT("apikey"), TEXT("value"));
			Out.bApiKeyInQuery = AuthParam(Auth, TEXT("apikey"), TEXT("in")).Equals(TEXT("query"), ESearchCase::IgnoreCase);
		}
		else if (Type != TEXT("noauth"))
		{
			UE_LOG(LogCourier, Warning, TEXT("Courier: Postman auth type '%s' is not supported; the request is imported without auth."), *Type);
		}
		return true;
	}

	FString ContentTypeForLanguage(const FString& Language)
	{
		if (Language == TEXT("javascript"))	return TEXT("application/javascript");
		if (Language == TEXT("html"))		return TEXT("text/html");
		if (Language == TEXT("xml"))		return TEXT("application/xml");
		return TEXT("text/plain");
	}

	void ReadUrl(const TSharedPtr<FJsonValue>& UrlValue, FCourierRequestSpec& Spec)
	{
		if (!UrlValue.IsValid())
		{
			return;
		}
		FString Raw;
		if (UrlValue->TryGetString(Raw))
		{
			FCourierHttp::SplitUrl(Raw, Spec.Url, Spec.QueryParams);
			return;
		}
		const TSharedPtr<FJsonObject> Url = UrlValue->AsObject();
		Raw = GetString(Url, TEXT("raw"));
		if (const TArray<TSharedPtr<FJsonValue>>* Query = GetArray(Url, TEXT("query")))
		{
			// The structured query keeps disabled rows, which raw drops.
			Raw.Split(TEXT("?"), &Spec.Url, nullptr);
			if (Spec.Url.IsEmpty())
			{
				Spec.Url = Raw;
			}
			Spec.QueryParams = ReadKeyValues(Query);
		}
		else
		{
			FCourierHttp::SplitUrl(Raw, Spec.Url, Spec.QueryParams);
		}
	}

	void ReadBody(const TSharedPtr<FJsonObject>& Body, FCourierRequestSpec& Spec)
	{
		if (!Body.IsValid() || GetBool(Body, TEXT("disabled"), false))
		{
			return;
		}
		const FString Mode = GetString(Body, TEXT("mode"));
		if (Mode == TEXT("raw"))
		{
			Spec.Body = GetString(Body, TEXT("raw"));
			const FString Language = GetString(GetObject(GetObject(Body, TEXT("options")), TEXT("raw")), TEXT("language")).ToLower();
			const FCourierKeyValue* TypeHeader = Spec.Headers.FindByPredicate([](const FCourierKeyValue& Row)
			{
				return Row.bEnabled && Row.Key.Equals(TEXT("Content-Type"), ESearchCase::IgnoreCase);
			});
			const bool bJsonHeader = TypeHeader && TypeHeader->Value.Contains(TEXT("json"));
			if (Language == TEXT("json") || bJsonHeader)
			{
				Spec.BodyType = ECourierBodyType::Json;
			}
			else
			{
				Spec.BodyType = ECourierBodyType::Raw;
				Spec.RawContentType = TypeHeader ? TypeHeader->Value : ContentTypeForLanguage(Language);
			}
		}
		else if (Mode == TEXT("urlencoded") || Mode == TEXT("formdata"))
		{
			Spec.BodyType = Mode == TEXT("urlencoded") ? ECourierBodyType::FormUrlEncoded : ECourierBodyType::Multipart;
			if (const TArray<TSharedPtr<FJsonValue>>* Fields = GetArray(Body, Mode))
			{
				for (const TSharedPtr<FJsonValue>& Entry : *Fields)
				{
					const TSharedPtr<FJsonObject> Row = Entry.IsValid() ? Entry->AsObject() : nullptr;
					if (!Row.IsValid())
					{
						continue;
					}
					FCourierFormField& Field = Spec.FormFields.AddDefaulted_GetRef();
					Field.Key = GetString(Row, TEXT("key"));
					Field.bEnabled = !GetBool(Row, TEXT("disabled"), false);
					Field.ContentType = GetString(Row, TEXT("contentType"));
					Field.bIsFile = GetString(Row, TEXT("type")) == TEXT("file");
					if (Field.bIsFile)
					{
						const TSharedPtr<FJsonValue> Src = Row->TryGetField(TEXT("src"));
						const TArray<TSharedPtr<FJsonValue>>* SrcArray = nullptr;
						if (Src.IsValid() && Src->TryGetArray(SrcArray) && SrcArray && SrcArray->Num() > 0)
						{
							Field.Value = ValueAsString((*SrcArray)[0]);
						}
						else
						{
							Field.Value = ValueAsString(Src);
						}
					}
					else
					{
						Field.Value = ValueAsString(Row->TryGetField(TEXT("value")));
					}
				}
			}
		}
		else if (Mode == TEXT("file"))
		{
			Spec.BodyType = ECourierBodyType::Binary;
			Spec.Body = GetString(GetObject(Body, TEXT("file")), TEXT("src"));
		}
		else if (Mode == TEXT("graphql"))
		{
			Spec.BodyType = ECourierBodyType::GraphQL;
			const TSharedPtr<FJsonObject> GraphQL = GetObject(Body, TEXT("graphql"));
			Spec.GraphQLQuery = GetString(GraphQL, TEXT("query"));
			Spec.GraphQLVariables = GetString(GraphQL, TEXT("variables"));
		}
	}

	void ReadRequest(const TSharedPtr<FJsonObject>& Item, const FString& Folder, const FCourierAuth& InheritedAuth, FCourierCollection& Out)
	{
		FCourierRequestSpec& Spec = Out.Requests.AddDefaulted_GetRef();
		Spec.Id = FGuid::NewGuid();
		Spec.Name = GetString(Item, TEXT("name"));
		Spec.Folder = Folder;
		Spec.Auth = InheritedAuth;

		const TSharedPtr<FJsonValue> RequestValue = Item->TryGetField(TEXT("request"));
		FString RequestUrl;
		if (RequestValue.IsValid() && RequestValue->TryGetString(RequestUrl))
		{
			FCourierHttp::SplitUrl(RequestUrl, Spec.Url, Spec.QueryParams);
		}
		else
		{
			const TSharedPtr<FJsonObject> Request = RequestValue.IsValid() ? RequestValue->AsObject() : nullptr;
			Spec.Verb = FCourierHttp::VerbFromString(GetString(Request, TEXT("method")), Spec.CustomVerb);
			Spec.Description = GetString(Request, TEXT("description"));
			Spec.Headers = ReadKeyValues(GetArray(Request, TEXT("header")));
			ReadUrl(Request.IsValid() ? Request->TryGetField(TEXT("url")) : nullptr, Spec);
			ReadBody(GetObject(Request, TEXT("body")), Spec);
			FCourierAuth OwnAuth;
			if (ReadAuth(GetObject(Request, TEXT("auth")), OwnAuth))
			{
				Spec.Auth = OwnAuth;
			}
		}
		if (Spec.Name.IsEmpty())
		{
			Spec.Name = Spec.Url;
		}

		const TSharedPtr<FJsonObject> Extension = GetObject(Item, TEXT("courier"));
		if (Extension.IsValid())
		{
			const FString Protocol = GetString(Extension, TEXT("protocol"));
			if (Protocol == TEXT("WebSocket"))
			{
				Spec.Protocol = ECourierProtocol::WebSocket;
			}
			else if (Protocol == TEXT("ServerSentEvents"))
			{
				Spec.Protocol = ECourierProtocol::ServerSentEvents;
			}
			if (const TArray<TSharedPtr<FJsonValue>>* Protocols = GetArray(Extension, TEXT("protocols")))
			{
				for (const TSharedPtr<FJsonValue>& Entry : *Protocols)
				{
					Spec.WebSocketProtocols.Add(ValueAsString(Entry));
				}
			}
			double Timeout = 0.0;
			if (Extension->TryGetNumberField(TEXT("timeout"), Timeout))
			{
				Spec.TimeoutSeconds = static_cast<float>(Timeout);
			}
		}
		else if (Spec.Url.StartsWith(TEXT("ws://")) || Spec.Url.StartsWith(TEXT("wss://")))
		{
			Spec.Protocol = ECourierProtocol::WebSocket;
		}
	}

	void ReadItems(const TArray<TSharedPtr<FJsonValue>>* Items, const FString& Folder, const FCourierAuth& InheritedAuth, FCourierCollection& Out)
	{
		if (!Items)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Items)
		{
			const TSharedPtr<FJsonObject> Item = Entry.IsValid() ? Entry->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				continue;
			}
			if (const TArray<TSharedPtr<FJsonValue>>* Children = GetArray(Item, TEXT("item")))
			{
				FString Name = GetString(Item, TEXT("name")).Replace(TEXT("/"), TEXT("-"));
				const FString SubFolder = Folder.IsEmpty() ? Name : Folder + TEXT("/") + Name;
				FCourierAuth FolderAuth = InheritedAuth;
				ReadAuth(GetObject(Item, TEXT("auth")), FolderAuth);
				if (Children->Num() == 0)
				{
					Out.Folders.AddUnique(SubFolder);
				}
				ReadItems(Children, SubFolder, FolderAuth, Out);
			}
			else
			{
				ReadRequest(Item, Folder, InheritedAuth, Out);
			}
		}
	}

	// ---- Export ----

	TArray<TSharedPtr<FJsonValue>> WriteKeyValues(const TArray<FCourierKeyValue>& Rows)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FCourierKeyValue& Row : Rows)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("key"), Row.Key);
			Object->SetStringField(TEXT("value"), Row.Value);
			if (!Row.bEnabled)
			{
				Object->SetBoolField(TEXT("disabled"), true);
			}
			Out.Add(MakeShared<FJsonValueObject>(Object));
		}
		return Out;
	}

	TSharedPtr<FJsonValue> AuthEntry(const FString& Key, const FString& Value)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("key"), Key);
		Object->SetStringField(TEXT("value"), Value);
		Object->SetStringField(TEXT("type"), TEXT("string"));
		return MakeShared<FJsonValueObject>(Object);
	}

	TSharedPtr<FJsonObject> WriteAuth(const FCourierAuth& Auth)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		switch (Auth.Type)
		{
		case ECourierAuthType::Bearer:
			Object->SetStringField(TEXT("type"), TEXT("bearer"));
			Object->SetArrayField(TEXT("bearer"), { AuthEntry(TEXT("token"), Auth.Token) });
			break;
		case ECourierAuthType::Basic:
			Object->SetStringField(TEXT("type"), TEXT("basic"));
			Object->SetArrayField(TEXT("basic"), { AuthEntry(TEXT("username"), Auth.Username), AuthEntry(TEXT("password"), Auth.Password) });
			break;
		case ECourierAuthType::ApiKey:
			Object->SetStringField(TEXT("type"), TEXT("apikey"));
			Object->SetArrayField(TEXT("apikey"), { AuthEntry(TEXT("key"), Auth.ApiKeyName), AuthEntry(TEXT("value"), Auth.ApiKeyValue), AuthEntry(TEXT("in"), Auth.bApiKeyInQuery ? TEXT("query") : TEXT("header")) });
			break;
		default:
			Object->SetStringField(TEXT("type"), TEXT("noauth"));
			break;
		}
		return Object;
	}

	TSharedPtr<FJsonObject> WriteBody(const FCourierRequestSpec& Spec)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		switch (Spec.BodyType)
		{
		case ECourierBodyType::Raw:
		case ECourierBodyType::Json:
		{
			Body->SetStringField(TEXT("mode"), TEXT("raw"));
			Body->SetStringField(TEXT("raw"), Spec.Body);
			TSharedRef<FJsonObject> Raw = MakeShared<FJsonObject>();
			const FString Language = Spec.BodyType == ECourierBodyType::Json ? TEXT("json")
				: Spec.RawContentType.Contains(TEXT("xml")) ? TEXT("xml")
				: Spec.RawContentType.Contains(TEXT("html")) ? TEXT("html")
				: Spec.RawContentType.Contains(TEXT("javascript")) ? TEXT("javascript") : TEXT("text");
			Raw->SetStringField(TEXT("language"), Language);
			TSharedRef<FJsonObject> Options = MakeShared<FJsonObject>();
			Options->SetObjectField(TEXT("raw"), Raw);
			Body->SetObjectField(TEXT("options"), Options);
			break;
		}
		case ECourierBodyType::FormUrlEncoded:
		case ECourierBodyType::Multipart:
		{
			const bool bMultipart = Spec.BodyType == ECourierBodyType::Multipart;
			const FString Mode = bMultipart ? TEXT("formdata") : TEXT("urlencoded");
			Body->SetStringField(TEXT("mode"), Mode);
			TArray<TSharedPtr<FJsonValue>> Fields;
			for (const FCourierFormField& Field : Spec.FormFields)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("key"), Field.Key);
				if (bMultipart && Field.bIsFile)
				{
					Row->SetStringField(TEXT("type"), TEXT("file"));
					Row->SetStringField(TEXT("src"), Field.Value);
				}
				else
				{
					Row->SetStringField(TEXT("value"), Field.Value);
					if (bMultipart)
					{
						Row->SetStringField(TEXT("type"), TEXT("text"));
					}
				}
				if (!Field.ContentType.IsEmpty())
				{
					Row->SetStringField(TEXT("contentType"), Field.ContentType);
				}
				if (!Field.bEnabled)
				{
					Row->SetBoolField(TEXT("disabled"), true);
				}
				Fields.Add(MakeShared<FJsonValueObject>(Row));
			}
			Body->SetArrayField(Mode, Fields);
			break;
		}
		case ECourierBodyType::Binary:
		{
			Body->SetStringField(TEXT("mode"), TEXT("file"));
			TSharedRef<FJsonObject> File = MakeShared<FJsonObject>();
			File->SetStringField(TEXT("src"), Spec.Body);
			Body->SetObjectField(TEXT("file"), File);
			break;
		}
		case ECourierBodyType::GraphQL:
		{
			Body->SetStringField(TEXT("mode"), TEXT("graphql"));
			TSharedRef<FJsonObject> GraphQL = MakeShared<FJsonObject>();
			GraphQL->SetStringField(TEXT("query"), Spec.GraphQLQuery);
			GraphQL->SetStringField(TEXT("variables"), Spec.GraphQLVariables);
			Body->SetObjectField(TEXT("graphql"), GraphQL);
			break;
		}
		default:
			return nullptr;
		}
		return Body;
	}

	TSharedPtr<FJsonObject> WriteUrl(const FCourierRequestSpec& Spec)
	{
		TSharedRef<FJsonObject> Url = MakeShared<FJsonObject>();
		TArray<FString> Parts;
		for (const FCourierKeyValue& Row : Spec.QueryParams)
		{
			if (Row.bEnabled)
			{
				Parts.Add(Row.Key + TEXT("=") + Row.Value);
			}
		}
		const FString Raw = Parts.Num() > 0 ? Spec.Url + (Spec.Url.Contains(TEXT("?")) ? TEXT("&") : TEXT("?")) + FString::Join(Parts, TEXT("&")) : Spec.Url;
		Url->SetStringField(TEXT("raw"), Raw);

		// host and path too: Postman's importer rebuilds the URL from them when present.
		FString Rest = Spec.Url;
		FString Protocol;
		if (Rest.Split(TEXT("://"), &Protocol, &Rest))
		{
			Url->SetStringField(TEXT("protocol"), Protocol);
		}
		Rest.Split(TEXT("?"), &Rest, nullptr);
		FString Host = Rest;
		FString Path;
		Rest.Split(TEXT("/"), &Host, &Path);
		TArray<FString> HostParts;
		Host.ParseIntoArray(HostParts, TEXT("."));
		TArray<TSharedPtr<FJsonValue>> HostValues;
		for (const FString& Part : HostParts)
		{
			HostValues.Add(MakeShared<FJsonValueString>(Part));
		}
		Url->SetArrayField(TEXT("host"), HostValues);
		TArray<FString> PathParts;
		Path.ParseIntoArray(PathParts, TEXT("/"));
		TArray<TSharedPtr<FJsonValue>> PathValues;
		for (const FString& Part : PathParts)
		{
			PathValues.Add(MakeShared<FJsonValueString>(Part));
		}
		Url->SetArrayField(TEXT("path"), PathValues);
		if (Spec.QueryParams.Num() > 0)
		{
			Url->SetArrayField(TEXT("query"), WriteKeyValues(Spec.QueryParams));
		}
		return Url;
	}

	TSharedPtr<FJsonObject> WriteRequestItem(const FCourierRequestSpec& Spec)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Spec.Name);

		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("method"), Spec.Protocol == ECourierProtocol::Http ? FCourierHttp::VerbToString(Spec.Verb, Spec.CustomVerb) : FString(TEXT("GET")));
		Request->SetArrayField(TEXT("header"), WriteKeyValues(Spec.Headers));
		if (const TSharedPtr<FJsonObject> Body = WriteBody(Spec))
		{
			Request->SetObjectField(TEXT("body"), Body);
		}
		Request->SetObjectField(TEXT("url"), WriteUrl(Spec));
		if (Spec.Auth.Type != ECourierAuthType::None)
		{
			Request->SetObjectField(TEXT("auth"), WriteAuth(Spec.Auth));
		}
		if (!Spec.Description.IsEmpty())
		{
			Request->SetStringField(TEXT("description"), Spec.Description);
		}
		Item->SetObjectField(TEXT("request"), Request);
		Item->SetArrayField(TEXT("response"), TArray<TSharedPtr<FJsonValue>>());

		TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
		Extension->SetStringField(TEXT("protocol"),
			Spec.Protocol == ECourierProtocol::WebSocket ? TEXT("WebSocket") : Spec.Protocol == ECourierProtocol::ServerSentEvents ? TEXT("ServerSentEvents") : TEXT("Http"));
		if (Spec.WebSocketProtocols.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Protocols;
			for (const FString& Protocol : Spec.WebSocketProtocols)
			{
				Protocols.Add(MakeShared<FJsonValueString>(Protocol));
			}
			Extension->SetArrayField(TEXT("protocols"), Protocols);
		}
		Extension->SetNumberField(TEXT("timeout"), Spec.TimeoutSeconds);
		Item->SetObjectField(TEXT("courier"), Extension);
		return Item;
	}

	/** A folder node while the tree is built; children keep insertion order. */
	struct FFolderNode
	{
		FString Name;
		TArray<TSharedPtr<FFolderNode>> Folders;
		TArray<const FCourierRequestSpec*> Requests;

		TSharedPtr<FFolderNode> FindOrAdd(const FString& Child)
		{
			for (const TSharedPtr<FFolderNode>& Folder : Folders)
			{
				if (Folder->Name == Child)
				{
					return Folder;
				}
			}
			TSharedPtr<FFolderNode> Folder = MakeShared<FFolderNode>();
			Folder->Name = Child;
			Folders.Add(Folder);
			return Folder;
		}

		FFolderNode* Resolve(const FString& Path)
		{
			TArray<FString> Parts;
			Path.ParseIntoArray(Parts, TEXT("/"));
			FFolderNode* Node = this;
			for (const FString& Part : Parts)
			{
				Node = Node->FindOrAdd(Part).Get();
			}
			return Node;
		}

		TArray<TSharedPtr<FJsonValue>> ToItems() const
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const TSharedPtr<FFolderNode>& Folder : Folders)
			{
				TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("name"), Folder->Name);
				Object->SetArrayField(TEXT("item"), Folder->ToItems());
				Items.Add(MakeShared<FJsonValueObject>(Object));
			}
			for (const FCourierRequestSpec* Spec : Requests)
			{
				Items.Add(MakeShared<FJsonValueObject>(WriteRequestItem(*Spec)));
			}
			return Items;
		}
	};

	bool Parse(const FString& Json, TSharedPtr<FJsonObject>& OutRoot, FString& OutError)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
		{
			OutError = FString::Printf(TEXT("Not valid JSON: %s"), *Reader->GetErrorMessage());
			return false;
		}
		return true;
	}

	FString Write(const TSharedRef<FJsonObject>& Root)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}
}

bool FCourierPostman::ImportCollection(const FString& Json, FCourierCollection& OutCollection, FString& OutError)
{
	using namespace CourierPostman;
	OutCollection = FCourierCollection();
	TSharedPtr<FJsonObject> Root;
	if (!Parse(Json, Root, OutError))
	{
		return false;
	}
	// Exports from the Postman API wrap the collection in {"collection": {...}}.
	if (const TSharedPtr<FJsonObject> Wrapped = GetObject(Root, TEXT("collection")))
	{
		Root = Wrapped;
	}
	const TSharedPtr<FJsonObject> Info = GetObject(Root, TEXT("info"));
	if (!Info.IsValid() || !GetArray(Root, TEXT("item")))
	{
		OutError = TEXT("This is not a Postman collection (no \"info\" and \"item\"). Postman v1 collections are not supported; re-export as v2.1.");
		return false;
	}
	OutCollection.Id = FGuid::NewGuid();
	OutCollection.Name = GetString(Info, TEXT("name"));
	OutCollection.Description = GetString(Info, TEXT("description"));
	OutCollection.Variables = ReadKeyValues(GetArray(Root, TEXT("variable")));

	FCourierAuth CollectionAuth;
	ReadAuth(GetObject(Root, TEXT("auth")), CollectionAuth);
	ReadItems(GetArray(Root, TEXT("item")), FString(), CollectionAuth, OutCollection);
	return true;
}

FString FCourierPostman::ExportCollection(const FCourierCollection& Collection)
{
	using namespace CourierPostman;
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
	Info->SetStringField(TEXT("_postman_id"), (Collection.Id.IsValid() ? Collection.Id : FGuid::NewGuid()).ToString(EGuidFormats::DigitsWithHyphensLower));
	Info->SetStringField(TEXT("name"), Collection.Name);
	if (!Collection.Description.IsEmpty())
	{
		Info->SetStringField(TEXT("description"), Collection.Description);
	}
	Info->SetStringField(TEXT("schema"), SchemaV21);
	Root->SetObjectField(TEXT("info"), Info);

	FFolderNode Tree;
	for (const FString& Folder : Collection.Folders)
	{
		Tree.Resolve(Folder);
	}
	for (const FCourierRequestSpec& Spec : Collection.Requests)
	{
		Tree.Resolve(Spec.Folder)->Requests.Add(&Spec);
	}
	Root->SetArrayField(TEXT("item"), Tree.ToItems());
	if (Collection.Variables.Num() > 0)
	{
		Root->SetArrayField(TEXT("variable"), WriteKeyValues(Collection.Variables));
	}
	return Write(Root);
}

bool FCourierPostman::ImportEnvironment(const FString& Json, FCourierEnvironment& OutEnvironment, FString& OutError)
{
	using namespace CourierPostman;
	OutEnvironment = FCourierEnvironment();
	TSharedPtr<FJsonObject> Root;
	if (!Parse(Json, Root, OutError))
	{
		return false;
	}
	if (const TSharedPtr<FJsonObject> Wrapped = GetObject(Root, TEXT("environment")))
	{
		Root = Wrapped;
	}
	const TArray<TSharedPtr<FJsonValue>>* Values = GetArray(Root, TEXT("values"));
	if (!Values)
	{
		OutError = TEXT("This is not a Postman environment (no \"values\").");
		return false;
	}
	OutEnvironment.Id = FGuid::NewGuid();
	OutEnvironment.Name = GetString(Root, TEXT("name"));
	OutEnvironment.Variables = ReadKeyValues(Values);
	return true;
}

FString FCourierPostman::ExportEnvironment(const FCourierEnvironment& Environment)
{
	using namespace CourierPostman;
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("id"), (Environment.Id.IsValid() ? Environment.Id : FGuid::NewGuid()).ToString(EGuidFormats::DigitsWithHyphensLower));
	Root->SetStringField(TEXT("name"), Environment.Name);
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FCourierKeyValue& Row : Environment.Variables)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("key"), Row.Key);
		Object->SetStringField(TEXT("value"), Row.Value);
		Object->SetStringField(TEXT("type"), TEXT("default"));
		Object->SetBoolField(TEXT("enabled"), Row.bEnabled);
		Values.Add(MakeShared<FJsonValueObject>(Object));
	}
	Root->SetArrayField(TEXT("values"), Values);
	Root->SetStringField(TEXT("_postman_variable_scope"), TEXT("environment"));
	return Write(Root);
}

bool FCourierPostman::IsEnvironmentJson(const FString& Json)
{
	using namespace CourierPostman;
	TSharedPtr<FJsonObject> Root;
	FString Error;
	if (!Parse(Json, Root, Error))
	{
		return false;
	}
	if (const TSharedPtr<FJsonObject> Wrapped = GetObject(Root, TEXT("environment")))
	{
		Root = Wrapped;
	}
	return GetArray(Root, TEXT("values")) != nullptr && !GetObject(Root, TEXT("info")).IsValid();
}
