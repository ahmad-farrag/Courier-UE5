#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"
#include "Interfaces/IHttpRequest.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STreeView.h"

class FCourierSocketIOConnection;
class FCourierSseConnection;
class IWebSocket;
class SBox;
class SMultiLineEditableTextBox;
class SVerticalBox;
class UCourierRequestAsset;
struct FCourierCookie;
struct FCourierHistoryEntry;
template <typename ItemType> class SListView;

/** The Courier tab: a Postman-style client for HTTP, GraphQL, WebSocket and Server-Sent Events. */
class SCourierWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCourierWindow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCourierWindow() override;

	/** Loads a request asset into the editor; Save then writes back to the asset. */
	void OpenRequestAsset(UCourierRequestAsset* Asset);
	/** Send for HTTP, Connect / Disconnect for WebSocket and SSE. */
	void SendOrConnect() { Working->Protocol == ECourierProtocol::Http ? SendHttp() : ToggleStream(); }

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	struct FTreeItem
	{
		enum class EKind : uint8 { Collection, Folder, Request };
		EKind Kind = EKind::Request;
		FGuid CollectionId;
		FGuid RequestId;
		FString FolderPath;
		FString Label;
		FCourierRequestSpec Spec;
		int32 RequestCount = 0;
		TArray<TSharedPtr<FTreeItem>> Children;
		FString Key() const;
	};
	using FTreeItemPtr = TSharedPtr<FTreeItem>;

	struct FLogLine
	{
		enum class EKind : uint8 { Sent, Received, System, Error };
		EKind Kind = EKind::System;
		FString Text;
		FString Tag;
		FDateTime Time;
	};
	using FLogLinePtr = TSharedPtr<FLogLine>;

	// ---- Layout ----
	TSharedRef<SWidget> BuildToolbar();
	TSharedRef<SWidget> BuildSidebar();
	TSharedRef<SWidget> BuildRequestEditor();
	TSharedRef<SWidget> BuildRequestTabs();
	TSharedRef<SWidget> BuildResponsePanel();
	TSharedRef<SWidget> BuildStreamPanel();
	TSharedRef<SWidget> BuildCollectionEditor();
	TSharedRef<SWidget> MakeNewMenu();
	TSharedRef<SWidget> MakeEnvironmentMenu();
	TSharedRef<SWidget> MakeVerbMenu();
	TSharedRef<SWidget> MakeProtocolMenu();
	TSharedRef<SWidget> MakeBodyTypeMenu();
	TSharedRef<SWidget> MakeAuthTypeMenu();
	TSharedRef<SWidget> MakeRequestActionsMenu();
	void ShowRequestEditor();
	void ShowCollectionEditor(const FGuid& CollectionId);
	void RebuildBody();
	void RebuildAuth();
	void BuildOAuth2Panel(const TSharedRef<SVerticalBox>& Box);
	FText GetOAuth2StatusText() const;
	void FetchOAuth2Token();
	void RebuildResponseHeaders();
	void RebuildEnvironments();

	// ---- Tree ----
	void RefreshTree();
	TSharedRef<ITableRow> OnGenerateTreeRow(FTreeItemPtr Item, const TSharedRef<STableViewBase>& Owner);
	void OnGetTreeChildren(FTreeItemPtr Item, TArray<FTreeItemPtr>& OutChildren);
	void OnTreeSelectionChanged(FTreeItemPtr Item, ESelectInfo::Type SelectInfo);
	void OnTreeExpansionChanged(FTreeItemPtr Item, bool bExpanded);
	TSharedPtr<SWidget> OnTreeContextMenu();

	// ---- Cookies ----
	TSharedRef<SWidget> BuildCookiesPanel();
	void RefreshCookies();
	TSharedRef<ITableRow> OnGenerateCookieRow(TSharedPtr<FCourierCookie> Cookie, const TSharedRef<STableViewBase>& Owner);

	// ---- History ----
	void RefreshHistory();
	TSharedRef<ITableRow> OnGenerateHistoryRow(TSharedPtr<FCourierHistoryEntry> Entry, const TSharedRef<STableViewBase>& Owner);

	// ---- Requests ----
	void NewRequest(ECourierProtocol Protocol, ECourierBodyType BodyType = ECourierBodyType::None, const FGuid& CollectionId = FGuid(), const FString& Folder = FString());
	void LoadRequest(const FCourierRequestSpec& Spec, const FGuid& CollectionId, UCourierRequestAsset* Asset);
	/** Asks about unsaved edits. False when the user cancelled. */
	bool ResolveUnsavedChanges();
	void SaveRequest();
	void SaveRequestAsAsset();
	void CopyCurl();
	void MarkDirty() { bDirty = true; }
	TMap<FString, FString> GetVariables() const;
	FString GetResolvedUrlPreview() const;
	FText GetUnresolvedVariablesText() const;

	// ---- HTTP ----
	void SendHttp();
	void OnHttpComplete(const FCourierRequestSpec& Sent, const FCourierResponse& Response);
	FText GetSendButtonText() const;

	// ---- WebSocket / SSE ----
	void ToggleStream();
	void ConnectWebSocket();
	void ConnectSse();
	void ConnectSocketIO();
	void EmitSocketIOEvent();
	void CloseStreams();
	bool IsStreamOpen() const;
	void SendWebSocketMessage();
	void AddLog(FLogLine::EKind Kind, const FString& Text, const FString& InTag = FString());
	TSharedRef<ITableRow> OnGenerateLogRow(FLogLinePtr Line, const TSharedRef<STableViewBase>& Owner);

	// ---- Collections and environments ----
	void PickCollectionThen(TFunction<void(const FGuid&)> Then);
	void ImportFile();
	void ExportCollection(const FGuid& CollectionId);
	void OnWorkspaceChanged();

	// Request being edited.
	TSharedPtr<FCourierRequestSpec> Working;
	FGuid WorkingCollectionId;
	TWeakObjectPtr<UCourierRequestAsset> SourceAsset;
	bool bDirty = false;

	// HTTP state.
	FHttpRequestPtr InFlight;
	bool bSending = false;
	bool bCancelled = false;
	TOptional<FCourierResponse> LastResponse;
	FText ResponseBodyText;
	bool bPrettyResponse = true;
	int32 ResponseTab = 0;

	// Stream state.
	TSharedPtr<IWebSocket> Socket;
	bool bSocketConnecting = false;
	TSharedPtr<FCourierSseConnection, ESPMode::ThreadSafe> Sse;
	TArray<FLogLinePtr> LogLines;
	TSharedPtr<SListView<FLogLinePtr>> LogList;
	TSharedPtr<SMultiLineEditableTextBox> Composer;
	TArray<uint8> PendingBinary;
	TSharedPtr<FCourierSocketIOConnection> SocketIO;
	FString ComposerEvent = TEXT("message");
	bool bComposerAck = false;

	// OAuth 2.0 and cookies.
	bool bFetchingToken = false;
	TArray<TSharedPtr<FCourierCookie>> CookieItems;
	TSharedPtr<SListView<TSharedPtr<FCourierCookie>>> CookieList;
	FDelegateHandle CookiesChangedHandle;

	// Widgets.
	TSharedPtr<SBox> RightHost;
	TSharedPtr<SBox> BodyHost;
	TSharedPtr<SBox> AuthHost;
	TSharedPtr<SBox> EnvironmentsHost;
	TSharedPtr<SVerticalBox> ResponseHeadersBox;
	TSharedPtr<STreeView<FTreeItemPtr>> Tree;
	TArray<FTreeItemPtr> RootItems;
	TSet<FString> ExpandedKeys;
	FString TreeFilter;
	TSharedPtr<SListView<TSharedPtr<FCourierHistoryEntry>>> HistoryList;
	TArray<TSharedPtr<FCourierHistoryEntry>> HistoryItems;
	int32 SidebarTab = 0;
	int32 RequestTab = 0;

	// Collection editor (edits a copy; Save writes it back).
	TSharedPtr<FCourierCollection> EditingCollection;
	bool bShowingCollection = false;

	// Environment editor (a copy too). An invalid id edits the globals.
	FGuid EditingEnvironmentId;
	TSharedPtr<FCourierEnvironment> EditingEnvironment;
	FString EnvironmentSignature;

	FDelegateHandle WorkspaceChangedHandle;
};
