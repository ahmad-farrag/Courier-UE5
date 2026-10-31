#include "SCourierWindow.h"

#include "AssetToolsModule.h"
#include "Async/Async.h"
#include "BlueprintActionDatabase.h"
#include "CourierCookies.h"
#include "CourierEventSource.h"
#include "CourierHttp.h"
#include "CourierLibrary.h"
#include "CourierOAuth2.h"
#include "CourierPostman.h"
#include "CourierRequestAsset.h"
#include "CourierRequestAssetFactory.h"
#include "CourierSocketIO.h"
#include "CourierWebSocket.h"
#include "CourierWorkspace.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IAssetTools.h"
#include "Interfaces/IHttpResponse.h"
#include "IWebSocket.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "SCourierWidgets.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "Courier"

namespace CourierWindow
{
	/** Slate gets slow on multi-megabyte text; the full body stays available through Copy and Save. */
	constexpr int32 MaxDisplayChars = 1 << 20;

	const FSlateColor Subdued = FSlateColor::UseSubduedForeground();

	FText ProtocolName(ECourierProtocol Protocol)
	{
		switch (Protocol)
		{
		case ECourierProtocol::WebSocket:			return LOCTEXT("ProtoWs", "WebSocket");
		case ECourierProtocol::ServerSentEvents:	return LOCTEXT("ProtoSse", "Server-Sent Events");
		case ECourierProtocol::SocketIO:			return LOCTEXT("ProtoSio", "Socket.IO");
		default:									return LOCTEXT("ProtoHttp", "HTTP");
		}
	}

	FText BodyTypeName(ECourierBodyType Type)
	{
		return StaticEnum<ECourierBodyType>()->GetDisplayNameTextByValue(static_cast<int64>(Type));
	}

	FText AuthTypeName(ECourierAuthType Type)
	{
		return StaticEnum<ECourierAuthType>()->GetDisplayNameTextByValue(static_cast<int64>(Type));
	}

	bool LooksLikeText(const FCourierResponse& Response)
	{
		const FString Type = Response.ContentType.ToLower();
		if (Type.IsEmpty())
		{
			// No type: call it text when the first bytes have no NULs.
			const int32 Probe = FMath::Min(Response.Content.Num(), 512);
			for (int32 Index = 0; Index < Probe; ++Index)
			{
				if (Response.Content[Index] == 0)
				{
					return false;
				}
			}
			return true;
		}
		return Type.StartsWith(TEXT("text/")) || Type.Contains(TEXT("json")) || Type.Contains(TEXT("xml"))
			|| Type.Contains(TEXT("javascript")) || Type.Contains(TEXT("html")) || Type.Contains(TEXT("x-www-form-urlencoded"))
			|| Type.Contains(TEXT("graphql")) || Type.Contains(TEXT("yaml")) || Type.Contains(TEXT("csv"));
	}

	TSharedRef<SWidget> SectionLabel(const FText& Text)
	{
		return SNew(STextBlock).Text(Text).ColorAndOpacity(Subdued).Font(FAppStyle::GetFontStyle("SmallFontBold"));
	}

	/** Protocols with no request body: messages go through the console instead. */
	bool HasNoBody(ECourierProtocol Protocol)
	{
		return Protocol == ECourierProtocol::WebSocket || Protocol == ECourierProtocol::SocketIO;
	}

	FText DescribeExpiry(const FDateTime& ExpiresAt)
	{
		if (ExpiresAt.GetTicks() == 0)
		{
			return LOCTEXT("NoExpiry", "no expiry given");
		}
		const FTimespan Left = ExpiresAt - FDateTime::UtcNow();
		if (Left.GetTotalSeconds() <= 0)
		{
			return LOCTEXT("Expired", "expired");
		}
		if (Left.GetTotalDays() >= 2.0)
		{
			return FText::Format(LOCTEXT("ExpiresDays", "expires in {0} days"), FText::AsNumber(FMath::FloorToInt(Left.GetTotalDays())));
		}
		return Left.GetTotalMinutes() >= 1.0
			? FText::Format(LOCTEXT("ExpiresMinutes", "expires in {0} min"), FText::AsNumber(FMath::FloorToInt(Left.GetTotalMinutes())))
			: FText::Format(LOCTEXT("ExpiresSeconds", "expires in {0} s"), FText::AsNumber(FMath::FloorToInt(Left.GetTotalSeconds())));
	}

	/** A folder path is inside (or is) another. */
	bool IsInFolder(const FString& Folder, const FString& Parent)
	{
		return Folder == Parent || Folder.StartsWith(Parent + TEXT("/"));
	}
}

FString SCourierWindow::FTreeItem::Key() const
{
	switch (Kind)
	{
	case EKind::Collection:	return TEXT("C:") + CollectionId.ToString();
	case EKind::Folder:		return TEXT("F:") + CollectionId.ToString() + TEXT(":") + FolderPath;
	default:				return TEXT("R:") + RequestId.ToString();
	}
}

void SCourierWindow::Construct(const FArguments& InArgs)
{
	FCourierWorkspace& Workspace = FCourierWorkspace::Get();
	WorkspaceChangedHandle = Workspace.OnChanged.AddSP(this, &SCourierWindow::OnWorkspaceChanged);
	// The jar can change on the HTTP thread; refresh the list on the game thread.
	TWeakPtr<SCourierWindow> WeakWindow = SharedThis(this);
	CookiesChangedHandle = FCourierCookieJar::Get().OnChanged.AddLambda([WeakWindow]()
	{
		AsyncTask(ENamedThreads::GameThread, [WeakWindow]()
		{
			if (const TSharedPtr<SCourierWindow> Self = WeakWindow.Pin())
			{
				Self->RefreshCookies();
			}
		});
	});
	for (const FCourierCollection& Collection : Workspace.Data.Collections)
	{
		ExpandedKeys.Add(TEXT("C:") + Collection.Id.ToString());
	}

	Working = MakeShared<FCourierRequestSpec>();
	Working->Id = FGuid::NewGuid();

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(0)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				BuildToolbar()
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SSeparator).Thickness(1.f)
			]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SNew(SSplitter)
				.PhysicalSplitterHandleSize(2.f)
				+ SSplitter::Slot().Value(0.26f)
				[
					BuildSidebar()
				]
				+ SSplitter::Slot().Value(0.74f)
				[
					SAssignNew(RightHost, SBox)
				]
			]
		]
	];

	RefreshTree();
	RefreshHistory();
	RefreshCookies();
	RebuildEnvironments();
	ShowRequestEditor();
}

SCourierWindow::~SCourierWindow()
{
	FCourierWorkspace::Get().OnChanged.Remove(WorkspaceChangedHandle);
	FCourierCookieJar::Get().OnChanged.Remove(CookiesChangedHandle);
	if (InFlight.IsValid())
	{
		InFlight->OnProcessRequestComplete().Unbind();
		InFlight->CancelRequest();
	}
	CloseStreams();
}

FReply SCourierWindow::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.IsControlDown() && InKeyEvent.GetKey() == EKeys::Enter)
	{
		SendOrConnect();
		return FReply::Handled();
	}
	if (InKeyEvent.IsControlDown() && InKeyEvent.GetKey() == EKeys::S)
	{
		SaveRequest();
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

// ------------------------------------------------------------------------------------------------
// Toolbar and sidebar

TSharedRef<SWidget> SCourierWindow::BuildToolbar()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(6, 4, 2, 4)
		[
			SNew(SComboButton)
			.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
			.OnGetMenuContent(this, &SCourierWindow::MakeNewMenu)
			.ButtonContent()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
				[
					SNew(SImage).Image(FAppStyle::GetBrush("Icons.Plus")).ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("New", "New"))
				]
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2, 4)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(LOCTEXT("ImportTip", "Import a Postman collection or environment (v2.0 / v2.1 JSON)"))
			.OnClicked_Lambda([this]() { ImportFile(); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(LOCTEXT("Import", "Import"))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			SNullWidget::NullWidget
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
		[
			SNew(STextBlock).Text(LOCTEXT("EnvironmentLabel", "Environment")).ColorAndOpacity(CourierWindow::Subdued)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(2, 4, 6, 4)
		[
			SNew(SBox)
			.MinDesiredWidth(180.f)
			[
				SNew(SComboButton)
				.OnGetMenuContent(this, &SCourierWindow::MakeEnvironmentMenu)
				.ButtonContent()
				[
					SNew(STextBlock).Text_Lambda([]()
					{
						const FCourierEnvironment* Environment = FCourierWorkspace::Get().GetActiveEnvironment();
						return Environment ? FText::FromString(Environment->Name) : LOCTEXT("NoEnvironment", "No environment");
					})
				]
			]
		];
}

TSharedRef<SWidget> SCourierWindow::MakeNewMenu()
{
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("NewHttp", "HTTP Request"), LOCTEXT("NewHttpTip", "REST: GET, POST, PUT, PATCH, DELETE..."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { NewRequest(ECourierProtocol::Http); })));
	Menu.AddMenuEntry(LOCTEXT("NewGraphQL", "GraphQL Request"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { NewRequest(ECourierProtocol::Http, ECourierBodyType::GraphQL); })));
	Menu.AddMenuEntry(LOCTEXT("NewWs", "WebSocket"), LOCTEXT("NewWsTip", "A ws:// or wss:// connection with a message console"), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { NewRequest(ECourierProtocol::WebSocket); })));
	Menu.AddMenuEntry(LOCTEXT("NewSse", "Server-Sent Events"), LOCTEXT("NewSseTip", "A text/event-stream subscription"), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { NewRequest(ECourierProtocol::ServerSentEvents); })));
	Menu.AddMenuEntry(LOCTEXT("NewSio", "Socket.IO"), LOCTEXT("NewSioTip", "A Socket.IO v3/v4 connection: emit and receive named events"), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { NewRequest(ECourierProtocol::SocketIO); })));
	Menu.AddSeparator();
	Menu.AddMenuEntry(LOCTEXT("NewCollection", "Collection"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			const TOptional<FString> Name = CourierUI::PromptForText(LOCTEXT("NewCollectionTitle", "New Collection"), LOCTEXT("CollectionName", "Name"), TEXT("New Collection"));
			if (Name.IsSet() && !Name->IsEmpty())
			{
				FCourierCollection& Collection = FCourierWorkspace::Get().AddCollection(*Name);
				ExpandedKeys.Add(TEXT("C:") + Collection.Id.ToString());
				FCourierWorkspace::Get().Save();
			}
		})));
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SCourierWindow::MakeEnvironmentMenu()
{
	FMenuBuilder Menu(true, nullptr);
	FCourierWorkspace& Workspace = FCourierWorkspace::Get();
	auto AddEntry = [&Menu, &Workspace](const FText& Label, const FGuid& Id)
	{
		Menu.AddMenuEntry(Label, FText::GetEmpty(), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Id]() { FCourierWorkspace::Get().Data.ActiveEnvironment = Id; FCourierWorkspace::Get().Save(); }),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([Id]() { return FCourierWorkspace::Get().Data.ActiveEnvironment == Id; })),
			NAME_None, EUserInterfaceActionType::RadioButton);
	};
	AddEntry(LOCTEXT("NoEnvironment", "No environment"), FGuid());
	for (const FCourierEnvironment& Environment : Workspace.Data.Environments)
	{
		AddEntry(FText::FromString(Environment.Name), Environment.Id);
	}
	Menu.AddSeparator();
	Menu.AddMenuEntry(LOCTEXT("ManageEnvironments", "Manage environments..."), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { SidebarTab = 1; })));
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SCourierWindow::BuildSidebar()
{
	TSharedRef<SWidget> Collections = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(4)
		[
			SNew(SSearchBox)
			.HintText(LOCTEXT("FilterHint", "Filter requests"))
			.OnTextChanged_Lambda([this](const FText& Text) { TreeFilter = Text.ToString(); RefreshTree(); })
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SAssignNew(Tree, STreeView<FTreeItemPtr>)
			.TreeItemsSource(&RootItems)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SCourierWindow::OnGenerateTreeRow)
			.OnGetChildren(this, &SCourierWindow::OnGetTreeChildren)
			.OnSelectionChanged(this, &SCourierWindow::OnTreeSelectionChanged)
			.OnExpansionChanged(this, &SCourierWindow::OnTreeExpansionChanged)
			.OnContextMenuOpening(this, &SCourierWindow::OnTreeContextMenu)
		];

	TSharedRef<SWidget> History = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(4).HAlign(HAlign_Right)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.Text(LOCTEXT("ClearHistory", "Clear"))
			.OnClicked_Lambda([]()
			{
				FCourierWorkspace::Get().Data.History.Reset();
				FCourierWorkspace::Get().Save();
				return FReply::Handled();
			})
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SAssignNew(HistoryList, SListView<TSharedPtr<FCourierHistoryEntry>>)
			.ListItemsSource(&HistoryItems)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SCourierWindow::OnGenerateHistoryRow)
			.OnMouseButtonDoubleClick_Lambda([this](TSharedPtr<FCourierHistoryEntry> Entry)
			{
				if (Entry.IsValid() && ResolveUnsavedChanges())
				{
					FCourierRequestSpec Copy = Entry->Request;
					Copy.Id = FGuid::NewGuid();
					LoadRequest(Copy, FGuid(), nullptr);
				}
			})
		];

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(4)
		[
			SNew(SSegmentedControl<int32>)
			.Value_Lambda([this]() { return SidebarTab; })
			.OnValueChanged_Lambda([this](int32 Value) { SidebarTab = Value; })
			+ SSegmentedControl<int32>::Slot(0).Text(LOCTEXT("TabCollections", "Collections"))
			+ SSegmentedControl<int32>::Slot(1).Text(LOCTEXT("TabEnvironments", "Environments"))
			+ SSegmentedControl<int32>::Slot(2).Text(LOCTEXT("TabHistory", "History"))
			+ SSegmentedControl<int32>::Slot(3).Text(LOCTEXT("TabCookies", "Cookies"))
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex_Lambda([this]() { return SidebarTab; })
			+ SWidgetSwitcher::Slot()[Collections]
			+ SWidgetSwitcher::Slot()[SAssignNew(EnvironmentsHost, SBox)]
			+ SWidgetSwitcher::Slot()[History]
			+ SWidgetSwitcher::Slot()[BuildCookiesPanel()]
		];
}

// ------------------------------------------------------------------------------------------------
// Collections tree

void SCourierWindow::RefreshTree()
{
	RootItems.Reset();
	const FString Filter = TreeFilter.TrimStartAndEnd();
	for (const FCourierCollection& Collection : FCourierWorkspace::Get().Data.Collections)
	{
		FTreeItemPtr Root = MakeShared<FTreeItem>();
		Root->Kind = FTreeItem::EKind::Collection;
		Root->CollectionId = Collection.Id;
		Root->Label = Collection.Name;
		Root->RequestCount = Collection.Requests.Num();

		TMap<FString, FTreeItemPtr> Folders;
		TFunction<FTreeItemPtr(const FString&)> GetFolder = [&](const FString& Path) -> FTreeItemPtr
		{
			if (Path.IsEmpty())
			{
				return Root;
			}
			if (const FTreeItemPtr* Found = Folders.Find(Path))
			{
				return *Found;
			}
			FString Parent, Name;
			if (!Path.Split(TEXT("/"), &Parent, &Name, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				Parent.Reset();
				Name = Path;
			}
			FTreeItemPtr ParentItem = GetFolder(Parent);
			FTreeItemPtr Folder = MakeShared<FTreeItem>();
			Folder->Kind = FTreeItem::EKind::Folder;
			Folder->CollectionId = Collection.Id;
			Folder->FolderPath = Path;
			Folder->Label = Name;
			// Folders before requests at each level.
			const int32 FirstRequest = ParentItem->Children.IndexOfByPredicate([](const FTreeItemPtr& Child) { return Child->Kind == FTreeItem::EKind::Request; });
			ParentItem->Children.Insert(Folder, FirstRequest == INDEX_NONE ? ParentItem->Children.Num() : FirstRequest);
			Folders.Add(Path, Folder);
			return Folder;
		};

		if (Filter.IsEmpty())
		{
			for (const FString& Folder : Collection.Folders)
			{
				GetFolder(Folder);
			}
		}
		for (const FCourierRequestSpec& Spec : Collection.Requests)
		{
			if (!Filter.IsEmpty() && !Spec.Name.Contains(Filter) && !Spec.Url.Contains(Filter))
			{
				continue;
			}
			FTreeItemPtr Item = MakeShared<FTreeItem>();
			Item->Kind = FTreeItem::EKind::Request;
			Item->CollectionId = Collection.Id;
			Item->RequestId = Spec.Id;
			Item->FolderPath = Spec.Folder;
			Item->Label = Spec.Name;
			Item->Spec = Spec;
			GetFolder(Spec.Folder)->Children.Add(Item);
		}
		if (Filter.IsEmpty() || Root->Children.Num() > 0)
		{
			RootItems.Add(Root);
		}
	}

	if (Tree.IsValid())
	{
		Tree->RequestTreeRefresh();
		TFunction<void(const FTreeItemPtr&)> Expand = [this, &Expand, bFiltering = !Filter.IsEmpty()](const FTreeItemPtr& Item)
		{
			if (Item->Kind != FTreeItem::EKind::Request)
			{
				Tree->SetItemExpansion(Item, bFiltering || ExpandedKeys.Contains(Item->Key()));
				for (const FTreeItemPtr& Child : Item->Children)
				{
					Expand(Child);
				}
			}
			else if (Item->RequestId == Working->Id)
			{
				Tree->SetItemSelection(Item, true, ESelectInfo::Direct);
			}
		};
		for (const FTreeItemPtr& Root : RootItems)
		{
			Expand(Root);
		}
	}
}

TSharedRef<ITableRow> SCourierWindow::OnGenerateTreeRow(FTreeItemPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	switch (Item->Kind)
	{
	case FTreeItem::EKind::Collection:
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
		[
			SNew(SImage).Image(FAppStyle::GetBrush("Icons.FolderClosed")).ColorAndOpacity(FLinearColor(1.f, 0.55f, 0.25f))
		];
		Row->AddSlot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(FText::FromString(Item->Label)).Font(FAppStyle::GetFontStyle("BoldFont"))
		];
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
		[
			SNew(STextBlock).Text(FText::AsNumber(Item->RequestCount)).ColorAndOpacity(CourierWindow::Subdued)
		];
		break;
	case FTreeItem::EKind::Folder:
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
		[
			SNew(SImage).Image(FAppStyle::GetBrush("Icons.FolderClosed")).ColorAndOpacity(FSlateColor::UseForeground())
		];
		Row->AddSlot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(FText::FromString(Item->Label))
		];
		break;
	default:
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
		[
			SNew(SBox).WidthOverride(34.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(CourierUI::BadgeText(Item->Spec)))
				.ColorAndOpacity(CourierUI::BadgeColor(Item->Spec))
				.Font(FAppStyle::GetFontStyle("SmallFontBold"))
			]
		];
		Row->AddSlot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(FText::FromString(Item->Label)).ToolTipText(FText::FromString(Item->Spec.Url))
		];
		break;
	}
	return SNew(STableRow<FTreeItemPtr>, Owner).Padding(FMargin(2, 3))[Row];
}

void SCourierWindow::OnGetTreeChildren(FTreeItemPtr Item, TArray<FTreeItemPtr>& OutChildren)
{
	OutChildren = Item->Children;
}

void SCourierWindow::OnTreeExpansionChanged(FTreeItemPtr Item, bool bExpanded)
{
	if (TreeFilter.IsEmpty())
	{
		bExpanded ? (void)ExpandedKeys.Add(Item->Key()) : (void)ExpandedKeys.Remove(Item->Key());
	}
}

void SCourierWindow::OnTreeSelectionChanged(FTreeItemPtr Item, ESelectInfo::Type SelectInfo)
{
	if (!Item.IsValid() || SelectInfo == ESelectInfo::Direct)
	{
		return;
	}
	if (Item->Kind == FTreeItem::EKind::Request)
	{
		if (Item->RequestId == Working->Id && !bShowingCollection)
		{
			return;
		}
		if (!ResolveUnsavedChanges())
		{
			RefreshTree();
			return;
		}
		FCourierCollection* Owner = nullptr;
		if (const FCourierRequestSpec* Spec = FCourierWorkspace::Get().FindRequest(Item->RequestId, &Owner))
		{
			LoadRequest(*Spec, Owner ? Owner->Id : FGuid(), nullptr);
		}
	}
	else if (Item->Kind == FTreeItem::EKind::Collection)
	{
		ShowCollectionEditor(Item->CollectionId);
	}
	else
	{
		Tree->SetItemExpansion(Item, !Tree->IsItemExpanded(Item));
	}
}

TSharedPtr<SWidget> SCourierWindow::OnTreeContextMenu()
{
	const TArray<FTreeItemPtr> Selected = Tree->GetSelectedItems();
	if (Selected.Num() == 0)
	{
		return nullptr;
	}
	const FTreeItemPtr Item = Selected[0];
	const FGuid CollectionId = Item->CollectionId;
	FMenuBuilder Menu(true, nullptr);

	if (Item->Kind != FTreeItem::EKind::Request)
	{
		const FString Folder = Item->FolderPath;
		Menu.AddMenuEntry(LOCTEXT("CtxNewRequest", "New HTTP Request"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, CollectionId, Folder]() { NewRequest(ECourierProtocol::Http, ECourierBodyType::None, CollectionId, Folder); })));
		Menu.AddMenuEntry(LOCTEXT("CtxNewWs", "New WebSocket"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, CollectionId, Folder]() { NewRequest(ECourierProtocol::WebSocket, ECourierBodyType::None, CollectionId, Folder); })));
		Menu.AddMenuEntry(LOCTEXT("CtxNewSse", "New Server-Sent Events"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, CollectionId, Folder]() { NewRequest(ECourierProtocol::ServerSentEvents, ECourierBodyType::None, CollectionId, Folder); })));
		Menu.AddMenuEntry(LOCTEXT("CtxNewSio", "New Socket.IO"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, CollectionId, Folder]() { NewRequest(ECourierProtocol::SocketIO, ECourierBodyType::None, CollectionId, Folder); })));
		Menu.AddMenuEntry(LOCTEXT("CtxNewFolder", "New Folder"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, CollectionId, Folder]()
			{
				const TOptional<FString> Name = CourierUI::PromptForText(LOCTEXT("NewFolderTitle", "New Folder"), LOCTEXT("FolderName", "Name"), TEXT("New Folder"));
				FCourierCollection* Collection = FCourierWorkspace::Get().FindCollection(CollectionId);
				if (Collection && Name.IsSet() && !Name->IsEmpty())
				{
					const FString Path = Folder.IsEmpty() ? Name->Replace(TEXT("/"), TEXT("-")) : Folder + TEXT("/") + Name->Replace(TEXT("/"), TEXT("-"));
					Collection->Folders.AddUnique(Path);
					ExpandedKeys.Add(TEXT("F:") + CollectionId.ToString() + TEXT(":") + Path);
					FCourierWorkspace::Get().Save();
				}
			})));
		Menu.AddSeparator();
	}

	switch (Item->Kind)
	{
	case FTreeItem::EKind::Collection:
		Menu.AddMenuEntry(LOCTEXT("CtxRenameCollection", "Rename"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([CollectionId]()
			{
				FCourierCollection* Collection = FCourierWorkspace::Get().FindCollection(CollectionId);
				const TOptional<FString> Name = Collection ? CourierUI::PromptForText(LOCTEXT("RenameTitle", "Rename"), LOCTEXT("NewName", "Name"), Collection->Name) : TOptional<FString>();
				if (Collection && Name.IsSet() && !Name->IsEmpty())
				{
					Collection->Name = *Name;
					FCourierWorkspace::Get().Save();
				}
			})));
		Menu.AddMenuEntry(LOCTEXT("CtxExport", "Export to Postman..."), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, CollectionId]() { ExportCollection(CollectionId); })));
		Menu.AddMenuEntry(LOCTEXT("CtxDeleteCollection", "Delete"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, CollectionId]()
			{
				FCourierWorkspace& Workspace = FCourierWorkspace::Get();
				const FCourierCollection* Collection = Workspace.FindCollection(CollectionId);
				if (Collection && CourierUI::Confirm(FText::Format(LOCTEXT("ConfirmDeleteCollection", "Delete the collection '{0}' and its {1} requests?"), FText::FromString(Collection->Name), Collection->Requests.Num())))
				{
					Workspace.Data.Collections.RemoveAll([&CollectionId](const FCourierCollection& Item) { return Item.Id == CollectionId; });
					if (WorkingCollectionId == CollectionId)
					{
						WorkingCollectionId.Invalidate();
						bDirty = true;
					}
					if (bShowingCollection && EditingCollection.IsValid() && EditingCollection->Id == CollectionId)
					{
						ShowRequestEditor();
					}
					Workspace.Save();
				}
			})));
		break;

	case FTreeItem::EKind::Folder:
	{
		const FString Folder = Item->FolderPath;
		Menu.AddMenuEntry(LOCTEXT("CtxRenameFolder", "Rename"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, CollectionId, Folder]()
			{
				FCourierCollection* Collection = FCourierWorkspace::Get().FindCollection(CollectionId);
				FString Parent, Leaf;
				if (!Folder.Split(TEXT("/"), &Parent, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
				{
					Leaf = Folder;
				}
				const TOptional<FString> Name = Collection ? CourierUI::PromptForText(LOCTEXT("RenameFolderTitle", "Rename Folder"), LOCTEXT("NewName", "Name"), Leaf) : TOptional<FString>();
				if (!Collection || !Name.IsSet() || Name->IsEmpty())
				{
					return;
				}
				const FString NewPath = Parent.IsEmpty() ? Name->Replace(TEXT("/"), TEXT("-")) : Parent + TEXT("/") + Name->Replace(TEXT("/"), TEXT("-"));
				auto Rename = [&Folder, &NewPath](FString& Path)
				{
					if (CourierWindow::IsInFolder(Path, Folder))
					{
						Path = NewPath + Path.Mid(Folder.Len());
					}
				};
				for (FCourierRequestSpec& Spec : Collection->Requests) { Rename(Spec.Folder); }
				for (FString& Path : Collection->Folders) { Rename(Path); }
				if (WorkingCollectionId == CollectionId) { Rename(Working->Folder); }
				ExpandedKeys.Add(TEXT("F:") + CollectionId.ToString() + TEXT(":") + NewPath);
				FCourierWorkspace::Get().Save();
			})));
		Menu.AddMenuEntry(LOCTEXT("CtxDeleteFolder", "Delete"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([CollectionId, Folder]()
			{
				FCourierCollection* Collection = FCourierWorkspace::Get().FindCollection(CollectionId);
				if (!Collection)
				{
					return;
				}
				const int32 Count = Collection->Requests.FilterByPredicate([&Folder](const FCourierRequestSpec& Spec) { return CourierWindow::IsInFolder(Spec.Folder, Folder); }).Num();
				if (CourierUI::Confirm(FText::Format(LOCTEXT("ConfirmDeleteFolder", "Delete the folder '{0}' and its {1} requests?"), FText::FromString(Folder), Count)))
				{
					Collection->Requests.RemoveAll([&Folder](const FCourierRequestSpec& Spec) { return CourierWindow::IsInFolder(Spec.Folder, Folder); });
					Collection->Folders.RemoveAll([&Folder](const FString& Path) { return CourierWindow::IsInFolder(Path, Folder); });
					FCourierWorkspace::Get().Save();
				}
			})));
		break;
	}

	default:
	{
		const FGuid RequestId = Item->RequestId;
		Menu.AddMenuEntry(LOCTEXT("CtxDuplicate", "Duplicate"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([CollectionId, RequestId]()
			{
				FCourierWorkspace& Workspace = FCourierWorkspace::Get();
				if (const FCourierRequestSpec* Spec = Workspace.FindRequest(RequestId))
				{
					FCourierRequestSpec Copy = *Spec;
					Copy.Id = FGuid::NewGuid();
					Copy.Name += TEXT(" Copy");
					Workspace.StoreRequest(CollectionId, Copy);
					Workspace.Save();
				}
			})));
		Menu.AddMenuEntry(LOCTEXT("CtxRename", "Rename"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, RequestId]()
			{
				FCourierRequestSpec* Spec = FCourierWorkspace::Get().FindRequest(RequestId);
				const TOptional<FString> Name = Spec ? CourierUI::PromptForText(LOCTEXT("RenameTitle", "Rename"), LOCTEXT("NewName", "Name"), Spec->Name) : TOptional<FString>();
				if (Spec && Name.IsSet() && !Name->IsEmpty())
				{
					Spec->Name = *Name;
					if (Working->Id == RequestId)
					{
						Working->Name = *Name;
					}
					FCourierWorkspace::Get().Save();
				}
			})));
		Menu.AddMenuEntry(LOCTEXT("CtxMove", "Move to Folder..."), LOCTEXT("CtxMoveTip", "Type a folder path such as Users/Admin; empty moves it to the collection root"), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, RequestId]()
			{
				FCourierRequestSpec* Spec = FCourierWorkspace::Get().FindRequest(RequestId);
				const TOptional<FString> Path = Spec ? CourierUI::PromptForText(LOCTEXT("MoveTitle", "Move to Folder"), LOCTEXT("FolderPath", "Folder path"), Spec->Folder) : TOptional<FString>();
				if (Spec && Path.IsSet())
				{
					Spec->Folder = *Path;
					if (Working->Id == RequestId)
					{
						Working->Folder = *Path;
					}
					FCourierWorkspace::Get().Save();
				}
			})));
		Menu.AddMenuEntry(LOCTEXT("CtxCurl", "Copy as cURL"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([CollectionId, RequestId]()
			{
				if (const FCourierRequestSpec* Spec = FCourierWorkspace::Get().FindRequest(RequestId))
				{
					FPlatformApplicationMisc::ClipboardCopy(*FCourierHttp::ToCurl(*Spec, FCourierWorkspace::Get().GetVariables(CollectionId)));
					CourierUI::Notify(LOCTEXT("CurlCopied", "cURL command copied"));
				}
			})));
		Menu.AddMenuEntry(LOCTEXT("CtxDelete", "Delete"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, RequestId]()
			{
				FCourierWorkspace& Workspace = FCourierWorkspace::Get();
				const FCourierRequestSpec* Spec = Workspace.FindRequest(RequestId);
				if (Spec && CourierUI::Confirm(FText::Format(LOCTEXT("ConfirmDelete", "Delete the request '{0}'?"), FText::FromString(Spec->Name))))
				{
					Workspace.RemoveRequest(RequestId);
					if (Working->Id == RequestId)
					{
						WorkingCollectionId.Invalidate();
						bDirty = true;
					}
					Workspace.Save();
				}
			})));
		break;
	}
	}
	return Menu.MakeWidget();
}

// ------------------------------------------------------------------------------------------------
// History

void SCourierWindow::RefreshHistory()
{
	HistoryItems.Reset();
	for (const FCourierHistoryEntry& Entry : FCourierWorkspace::Get().Data.History)
	{
		HistoryItems.Add(MakeShared<FCourierHistoryEntry>(Entry));
	}
	if (HistoryList.IsValid())
	{
		HistoryList->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SCourierWindow::OnGenerateHistoryRow(TSharedPtr<FCourierHistoryEntry> Entry, const TSharedRef<STableViewBase>& Owner)
{
	const FString Status = Entry->StatusCode > 0 ? FString::FromInt(Entry->StatusCode) : FString(TEXT("ERR"));
	return SNew(STableRow<TSharedPtr<FCourierHistoryEntry>>, Owner)
		.Padding(FMargin(2, 3))
		.ToolTipText(FText::Format(LOCTEXT("HistoryTip", "{0}\n{1}\nDouble-click to open a copy."), FText::FromString(Entry->Request.Url), FText::AsDateTime(Entry->Time)))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
			[
				SNew(SBox).WidthOverride(34.f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(CourierUI::BadgeText(Entry->Request)))
					.ColorAndOpacity(CourierUI::BadgeColor(Entry->Request))
					.Font(FAppStyle::GetFontStyle("SmallFontBold"))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(FText::FromString(Entry->Request.Url)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
			[
				SNew(STextBlock).Text(FText::FromString(Status)).ColorAndOpacity(CourierUI::StatusColor(Entry->StatusCode)).Font(FAppStyle::GetFontStyle("SmallFontBold"))
			]
		];
}

// ------------------------------------------------------------------------------------------------
// Environments

void SCourierWindow::RebuildEnvironments()
{
	if (!EnvironmentsHost.IsValid())
	{
		return;
	}
	FCourierWorkspace& Workspace = FCourierWorkspace::Get();
	if (EditingEnvironmentId.IsValid() && !Workspace.FindEnvironment(EditingEnvironmentId))
	{
		EditingEnvironmentId.Invalidate();
	}
	EditingEnvironment = MakeShared<FCourierEnvironment>();
	if (const FCourierEnvironment* Environment = Workspace.FindEnvironment(EditingEnvironmentId))
	{
		*EditingEnvironment = *Environment;
	}
	else
	{
		EditingEnvironment->Name = TEXT("Globals");
		EditingEnvironment->Variables = Workspace.Data.Globals;
	}

	TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
	auto AddRow = [this, &List](const FText& Label, const FGuid& Id, bool bActive)
	{
		List->AddSlot().AutoHeight()
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), EditingEnvironmentId == Id ? "PrimaryButton" : "SimpleButton")
			.OnClicked_Lambda([this, Id]() { EditingEnvironmentId = Id; RebuildEnvironments(); return FReply::Handled(); })
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(STextBlock).Text(Label)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock).Text(bActive ? LOCTEXT("ActiveEnv", "active") : FText::GetEmpty()).ColorAndOpacity(FLinearColor(0.42f, 0.83f, 0.47f))
				]
			]
		];
	};
	AddRow(LOCTEXT("Globals", "Globals"), FGuid(), false);
	for (const FCourierEnvironment& Environment : Workspace.Data.Environments)
	{
		AddRow(FText::FromString(Environment.Name), Environment.Id, Workspace.Data.ActiveEnvironment == Environment.Id);
	}

	const bool bGlobals = !EditingEnvironmentId.IsValid();
	EnvironmentsHost->SetContent(
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(6)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[List]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("NewEnvironment", "New"))
					.OnClicked_Lambda([this]()
					{
						const TOptional<FString> Name = CourierUI::PromptForText(LOCTEXT("NewEnvTitle", "New Environment"), LOCTEXT("EnvName", "Name"), TEXT("Development"));
						if (Name.IsSet() && !Name->IsEmpty())
						{
							FCourierEnvironment& Environment = FCourierWorkspace::Get().Data.Environments.AddDefaulted_GetRef();
							Environment.Id = FGuid::NewGuid();
							Environment.Name = *Name;
							EditingEnvironmentId = Environment.Id;
							FCourierWorkspace::Get().Save();
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("ImportEnvironment", "Import"))
					.OnClicked_Lambda([this]() { ImportFile(); return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				bGlobals
				? StaticCastSharedRef<SWidget>(SNew(STextBlock).Text(LOCTEXT("GlobalsHelp", "Globals: every request sees these, below collection and environment variables.")).AutoWrapText(true).ColorAndOpacity(CourierWindow::Subdued))
				: StaticCastSharedRef<SWidget>(SNew(SEditableTextBox)
					.Text(FText::FromString(EditingEnvironment->Name))
					.OnTextChanged_Lambda([this](const FText& Text) { EditingEnvironment->Name = Text.ToString(); }))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SCourierKeyValueList)
				.Rows(&EditingEnvironment->Variables)
				.KeyHint(LOCTEXT("VarHint", "Variable"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Text(LOCTEXT("SaveEnvironment", "Save"))
					.OnClicked_Lambda([this]()
					{
						FCourierWorkspace& Workspace = FCourierWorkspace::Get();
						if (FCourierEnvironment* Environment = Workspace.FindEnvironment(EditingEnvironmentId))
						{
							*Environment = *EditingEnvironment;
						}
						else
						{
							Workspace.Data.Globals = EditingEnvironment->Variables;
						}
						Workspace.Save();
						CourierUI::Notify(LOCTEXT("EnvironmentSaved", "Variables saved"));
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Visibility(bGlobals ? EVisibility::Collapsed : EVisibility::Visible)
					.Text(LOCTEXT("UseEnvironment", "Use"))
					.ToolTipText(LOCTEXT("UseEnvironmentTip", "Make this the active environment"))
					.OnClicked_Lambda([this]()
					{
						FCourierWorkspace::Get().Data.ActiveEnvironment = EditingEnvironmentId;
						FCourierWorkspace::Get().Save();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Visibility(bGlobals ? EVisibility::Collapsed : EVisibility::Visible)
					.Text(LOCTEXT("ExportEnvironment", "Export"))
					.OnClicked_Lambda([this]()
					{
						const FString File = CourierUI::SaveFileDialog(TEXT("Export Postman environment"), EditingEnvironment->Name + TEXT(".postman_environment.json"), TEXT("Postman environment (*.json)|*.json"));
						if (!File.IsEmpty())
						{
							FFileHelper::SaveStringToFile(FCourierPostman::ExportEnvironment(*EditingEnvironment), *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
							CourierUI::Notify(FText::Format(LOCTEXT("Exported", "Exported to {0}"), FText::FromString(File)));
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Visibility(bGlobals ? EVisibility::Collapsed : EVisibility::Visible)
					.Text(LOCTEXT("DeleteEnvironment", "Delete"))
					.OnClicked_Lambda([this]()
					{
						FCourierWorkspace& Workspace = FCourierWorkspace::Get();
						if (CourierUI::Confirm(FText::Format(LOCTEXT("ConfirmDeleteEnv", "Delete the environment '{0}'?"), FText::FromString(EditingEnvironment->Name))))
						{
							const FGuid Id = EditingEnvironmentId;
							Workspace.Data.Environments.RemoveAll([&Id](const FCourierEnvironment& Environment) { return Environment.Id == Id; });
							if (Workspace.Data.ActiveEnvironment == Id)
							{
								Workspace.Data.ActiveEnvironment.Invalidate();
							}
							EditingEnvironmentId.Invalidate();
							Workspace.Save();
						}
						return FReply::Handled();
					})
				]
			]
		]);
}

// ------------------------------------------------------------------------------------------------
// Request editor

void SCourierWindow::ShowRequestEditor()
{
	bShowingCollection = false;
	EditingCollection.Reset();
	RightHost->SetContent(BuildRequestEditor());
	RebuildBody();
	RebuildAuth();
	RebuildResponseHeaders();
}

TSharedRef<SWidget> SCourierWindow::BuildRequestEditor()
{
	const bool bHttp = Working->Protocol == ECourierProtocol::Http;
	const bool bShowVerb = Working->Protocol == ECourierProtocol::Http || Working->Protocol == ECourierProtocol::ServerSentEvents;

	TSharedRef<SHorizontalBox> UrlRow = SNew(SHorizontalBox);
	if (bShowVerb)
	{
		UrlRow->AddSlot().AutoWidth().Padding(0, 0, 4, 0)
		[
			SNew(SBox).MinDesiredWidth(96.f)
			[
				SNew(SComboButton)
				.OnGetMenuContent(this, &SCourierWindow::MakeVerbMenu)
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(FCourierHttp::VerbToString(Working->Verb, Working->CustomVerb)); })
					.ColorAndOpacity_Lambda([this]() { return CourierUI::VerbColor(Working->Verb); })
					.Font(FAppStyle::GetFontStyle("BoldFont"))
				]
			]
		];
	}
	UrlRow->AddSlot().FillWidth(1.f).Padding(0, 0, 4, 0)
	[
		SNew(SEditableTextBox)
		.Font(CourierUI::MonoFont(10))
		.HintText(Working->Protocol == ECourierProtocol::WebSocket ? LOCTEXT("WsUrlHint", "wss://example.com/socket")
			: Working->Protocol == ECourierProtocol::SocketIO ? LOCTEXT("SioUrlHint", "https://example.com  (the Socket.IO server; /socket.io/ is added)")
			: LOCTEXT("UrlHint", "https://api.example.com/users/{{id}}"))
		.Text(FText::FromString(Working->Url))
		.OnTextChanged_Lambda([this](const FText& Text) { Working->Url = Text.ToString(); MarkDirty(); })
		.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type Commit)
		{
			if (Commit != ETextCommit::OnEnter)
			{
				return;
			}
			// A pasted URL with a query string becomes Params rows, as in Postman.
			FString Base;
			TArray<FCourierKeyValue> Query;
			FCourierHttp::SplitUrl(Text.ToString(), Base, Query);
			if (Query.Num() > 0)
			{
				Working->Url = Base;
				Working->QueryParams.Append(Query);
				ShowRequestEditor();
			}
			SendOrConnect();
		})
	];
	UrlRow->AddSlot().AutoWidth()
	[
		SNew(SBox).MinDesiredWidth(96.f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
			.HAlign(HAlign_Center)
			.Text_Lambda([this]() { return GetSendButtonText(); })
			.ToolTipText(LOCTEXT("SendTip", "Ctrl+Enter"))
			.OnClicked_Lambda([this]()
			{
				SendOrConnect();
				return FReply::Handled();
			})
		]
	];

	return SNew(SVerticalBox)
		// Name, protocol, save, actions.
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0, 0, 6, 0)
			[
				SNew(SEditableTextBox)
				.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
				.Text(FText::FromString(Working->Name))
				.OnTextChanged_Lambda([this](const FText& Text) { Working->Name = Text.ToString(); MarkDirty(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
			[
				SNew(STextBlock)
				.ColorAndOpacity(CourierWindow::Subdued)
				.Text_Lambda([this]()
				{
					if (SourceAsset.IsValid())
					{
						return FText::Format(LOCTEXT("FromAsset", "asset {0}"), FText::FromString(SourceAsset->GetName()));
					}
					const FCourierCollection* Collection = FCourierWorkspace::Get().FindCollection(WorkingCollectionId);
					const FText Where = Collection
						? FText::FromString(Working->Folder.IsEmpty() ? Collection->Name : Collection->Name + TEXT(" / ") + Working->Folder)
						: LOCTEXT("Unsaved", "not saved");
					return bDirty ? FText::Format(LOCTEXT("DirtyWhere", "{0} (edited)"), Where) : Where;
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
			[
				SNew(SComboButton)
				.OnGetMenuContent(this, &SCourierWindow::MakeProtocolMenu)
				.ButtonContent()
				[
					SNew(STextBlock).Text_Lambda([this]() { return CourierWindow::ProtocolName(Working->Protocol); })
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("Save", "Save"))
				.ToolTipText(LOCTEXT("SaveTip", "Save to its collection or asset (Ctrl+S)"))
				.OnClicked_Lambda([this]() { SaveRequest(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SComboButton)
				.OnGetMenuContent(this, &SCourierWindow::MakeRequestActionsMenu)
				.ButtonContent()
				[
					SNew(STextBlock).Text(LOCTEXT("More", "More"))
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 4)
		[
			UrlRow
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(12, 0, 8, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(STextBlock)
				.Font(CourierUI::MonoFont(8))
				.ColorAndOpacity(CourierWindow::Subdued)
				.Text_Lambda([this]() { return FText::FromString(GetResolvedUrlPreview()); })
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(STextBlock)
				.Font(CourierUI::MonoFont(8))
				.ColorAndOpacity(FLinearColor(1.f, 0.7f, 0.3f))
				.Text_Lambda([this]() { return GetUnresolvedVariablesText(); })
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(8, 4, 8, 8)
		[
			SNew(SSplitter)
			.Orientation(Orient_Vertical)
			.PhysicalSplitterHandleSize(2.f)
			+ SSplitter::Slot().Value(0.45f)
			[
				BuildRequestTabs()
			]
			+ SSplitter::Slot().Value(0.55f)
			[
				bHttp ? BuildResponsePanel() : BuildStreamPanel()
			]
		];
}

TSharedRef<SWidget> SCourierWindow::BuildRequestTabs()
{
	const bool bWebSocket = Working->Protocol == ECourierProtocol::WebSocket;
	const bool bSocketIO = Working->Protocol == ECourierProtocol::SocketIO;
	const FSimpleDelegate Dirty = FSimpleDelegate::CreateSP(this, &SCourierWindow::MarkDirty);

	TSharedRef<SWidget> Params = SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SCourierKeyValueList).Rows(&Working->QueryParams).KeyHint(LOCTEXT("ParamHint", "Parameter")).OnChanged(Dirty)
		];

	TSharedRef<SWidget> Headers = SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SCourierKeyValueList).Rows(&Working->Headers).KeyHint(LOCTEXT("HeaderHint", "Header")).OnChanged(Dirty)
		]
		+ SScrollBox::Slot().Padding(0, 6)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(CourierWindow::Subdued)
			.Text(LOCTEXT("AutoHeaders", "Content-Type comes from the body type and Authorization from the Auth tab unless you set them here."))
		];

	TSharedRef<SWidget> Body = CourierWindow::HasNoBody(Working->Protocol)
		? StaticCastSharedRef<SWidget>(SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(CourierWindow::Subdued).Text(bSocketIO
			? LOCTEXT("SioBody", "Socket.IO has no request body: emit events from the console below once connected. The CONNECT auth payload is in Settings.")
			: LOCTEXT("WsBody", "A WebSocket has no request body: write messages in the console below once connected.")))
		: StaticCastSharedRef<SWidget>(SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4).HAlign(HAlign_Left)
			[
				SNew(SComboButton)
				.OnGetMenuContent(this, &SCourierWindow::MakeBodyTypeMenu)
				.ButtonContent()
				[
					SNew(STextBlock).Text_Lambda([this]() { return CourierWindow::BodyTypeName(Working->BodyType); })
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SAssignNew(BodyHost, SBox)
			]);

	// OAuth 2.0 has many fields: scroll rather than spill over the response pane.
	TSharedRef<SWidget> Auth = SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(0, 0, 0, 6).HAlign(HAlign_Left)
		[
			SNew(SComboButton)
			.OnGetMenuContent(this, &SCourierWindow::MakeAuthTypeMenu)
			.ButtonContent()
			[
				SNew(STextBlock).Text_Lambda([this]() { return CourierWindow::AuthTypeName(Working->Auth.Type); })
			]
		]
		+ SScrollBox::Slot()
		[
			SAssignNew(AuthHost, SBox)
		];

	TSharedRef<SVerticalBox> Settings = SNew(SVerticalBox);
	if (Working->Protocol == ECourierProtocol::Http)
	{
		Settings->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Text(LOCTEXT("Timeout", "Timeout (seconds, 0 = engine default)"))
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(90.f)
				[
					SNew(SSpinBox<float>)
					.MinValue(0.f)
					.MaxValue(3600.f)
					.Value_Lambda([this]() { return Working->TimeoutSeconds; })
					.OnValueChanged_Lambda([this](float Value) { Working->TimeoutSeconds = Value; MarkDirty(); })
				]
			]
		];
	}
	if (bWebSocket)
	{
		Settings->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Text(LOCTEXT("Subprotocols", "Sub-protocols (comma separated)"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SEditableTextBox)
				.Text(FText::FromString(FString::Join(Working->WebSocketProtocols, TEXT(", "))))
				.OnTextChanged_Lambda([this](const FText& Text)
				{
					TArray<FString> Parts;
					Text.ToString().ParseIntoArray(Parts, TEXT(","));
					Working->WebSocketProtocols.Reset();
					for (FString& Part : Parts)
					{
						Part.TrimStartAndEndInline();
						if (!Part.IsEmpty())
						{
							Working->WebSocketProtocols.Add(Part);
						}
					}
					MarkDirty();
				})
			]
		];
	}
	if (bSocketIO)
	{
		Settings->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(SBox).WidthOverride(110.f)[SNew(STextBlock).Text(LOCTEXT("SioNamespace", "Namespace"))]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SEditableTextBox)
				.Font(CourierUI::MonoFont())
				.HintText(LOCTEXT("SioNamespaceHint", "/ (default) or /match"))
				.Text(FText::FromString(Working->SocketIONamespace))
				.OnTextChanged_Lambda([this](const FText& Text) { Working->SocketIONamespace = Text.ToString(); MarkDirty(); })
			]
		];
		Settings->AddSlot().AutoHeight().Padding(0, 0, 0, 2)
		[
			CourierWindow::SectionLabel(LOCTEXT("SioAuth", "AUTH PAYLOAD (JSON, sent with CONNECT; the server reads socket.handshake.auth)"))
		];
		Settings->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(SBox).MinDesiredHeight(48.f)
			[
				SNew(SMultiLineEditableTextBox)
				.Font(CourierUI::MonoFont(9))
				.HintText(LOCTEXT("SioAuthHint", "{ \"token\": \"{{token}}\" }"))
				.Text(FText::FromString(Working->SocketIOAuth))
				.OnTextChanged_Lambda([this](const FText& Text) { Working->SocketIOAuth = Text.ToString(); MarkDirty(); })
			]
		];
	}
	Settings->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(SCheckBox)
		.IsChecked_Lambda([this]() { return Working->bUseCookieJar ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Working->bUseCookieJar = State == ECheckBoxState::Checked; MarkDirty(); })
		.ToolTipText(LOCTEXT("CookieJarTip", "Send matching cookies from the jar (sidebar > Cookies) and keep the ones the server sets."))
		[
			SNew(STextBlock).Text(LOCTEXT("UseCookieJar", "Send and store cookies"))
		]
	];
	Settings->AddSlot().AutoHeight().Padding(0, 0, 0, 2)
	[
		CourierWindow::SectionLabel(LOCTEXT("Description", "DESCRIPTION"))
	];
	Settings->AddSlot().AutoHeight()
	[
		SNew(SBox).MinDesiredHeight(80.f)
		[
			SNew(SMultiLineEditableTextBox)
			.Text(FText::FromString(Working->Description))
			.AutoWrapText(true)
			.OnTextChanged_Lambda([this](const FText& Text) { Working->Description = Text.ToString(); MarkDirty(); })
		]
	];

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6).HAlign(HAlign_Left)
		[
			SNew(SSegmentedControl<int32>)
			.Value_Lambda([this]() { return RequestTab; })
			.OnValueChanged_Lambda([this](int32 Value) { RequestTab = Value; })
			+ SSegmentedControl<int32>::Slot(0).Text(LOCTEXT("TabParams", "Params"))
			+ SSegmentedControl<int32>::Slot(1).Text(LOCTEXT("TabHeaders", "Headers"))
			+ SSegmentedControl<int32>::Slot(2).Text(LOCTEXT("TabBody", "Body"))
			+ SSegmentedControl<int32>::Slot(3).Text(LOCTEXT("TabAuth", "Auth"))
			+ SSegmentedControl<int32>::Slot(4).Text(LOCTEXT("TabSettings", "Settings"))
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex_Lambda([this]() { return RequestTab; })
			+ SWidgetSwitcher::Slot()[Params]
			+ SWidgetSwitcher::Slot()[Headers]
			+ SWidgetSwitcher::Slot()[Body]
			+ SWidgetSwitcher::Slot()[Auth]
			+ SWidgetSwitcher::Slot()[SNew(SScrollBox) + SScrollBox::Slot()[Settings]]
		];
}

TSharedRef<SWidget> SCourierWindow::MakeVerbMenu()
{
	FMenuBuilder Menu(true, nullptr);
	for (ECourierVerb Verb : { ECourierVerb::Get, ECourierVerb::Post, ECourierVerb::Put, ECourierVerb::Patch, ECourierVerb::Delete, ECourierVerb::Head, ECourierVerb::Options })
	{
		Menu.AddMenuEntry(FText::FromString(FCourierHttp::VerbToString(Verb)), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, Verb]() { Working->Verb = Verb; MarkDirty(); })));
	}
	Menu.AddMenuEntry(LOCTEXT("CustomVerb", "Custom..."), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			const TOptional<FString> Verb = CourierUI::PromptForText(LOCTEXT("CustomVerbTitle", "Custom Method"), LOCTEXT("CustomVerbLabel", "HTTP method"), Working->CustomVerb.IsEmpty() ? TEXT("PURGE") : Working->CustomVerb);
			if (Verb.IsSet() && !Verb->IsEmpty())
			{
				Working->Verb = FCourierHttp::VerbFromString(*Verb, Working->CustomVerb);
				MarkDirty();
			}
		})));
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SCourierWindow::MakeProtocolMenu()
{
	FMenuBuilder Menu(true, nullptr);
	for (ECourierProtocol Protocol : { ECourierProtocol::Http, ECourierProtocol::WebSocket, ECourierProtocol::ServerSentEvents, ECourierProtocol::SocketIO })
	{
		Menu.AddMenuEntry(CourierWindow::ProtocolName(Protocol), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, Protocol]()
			{
				if (Working->Protocol != Protocol)
				{
					CloseStreams();
					Working->Protocol = Protocol;
					if (Protocol == ECourierProtocol::WebSocket)
					{
						Working->Url = Working->Url.Replace(TEXT("https://"), TEXT("wss://")).Replace(TEXT("http://"), TEXT("ws://"));
					}
					else
					{
						Working->Url = Working->Url.Replace(TEXT("wss://"), TEXT("https://")).Replace(TEXT("ws://"), TEXT("http://"));
					}
					MarkDirty();
					ShowRequestEditor();
				}
			})));
	}
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SCourierWindow::MakeBodyTypeMenu()
{
	FMenuBuilder Menu(true, nullptr);
	for (ECourierBodyType Type : { ECourierBodyType::None, ECourierBodyType::Json, ECourierBodyType::Raw, ECourierBodyType::FormUrlEncoded, ECourierBodyType::Multipart, ECourierBodyType::Binary, ECourierBodyType::GraphQL })
	{
		Menu.AddMenuEntry(CourierWindow::BodyTypeName(Type), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, Type]()
			{
				Working->BodyType = Type;
				if (Type == ECourierBodyType::GraphQL && Working->Verb == ECourierVerb::Get)
				{
					Working->Verb = ECourierVerb::Post;
				}
				MarkDirty();
				RebuildBody();
			})));
	}
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SCourierWindow::MakeAuthTypeMenu()
{
	FMenuBuilder Menu(true, nullptr);
	for (ECourierAuthType Type : { ECourierAuthType::None, ECourierAuthType::Bearer, ECourierAuthType::Basic, ECourierAuthType::ApiKey, ECourierAuthType::OAuth2 })
	{
		Menu.AddMenuEntry(CourierWindow::AuthTypeName(Type), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, Type]() { Working->Auth.Type = Type; MarkDirty(); RebuildAuth(); })));
	}
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SCourierWindow::MakeRequestActionsMenu()
{
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("SaveAs", "Save to Collection..."), LOCTEXT("SaveAsTip", "Save a copy into a collection of your choice"), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			PickCollectionThen([this](const FGuid& CollectionId)
			{
				Working->Id = FGuid::NewGuid();
				SourceAsset.Reset();
				WorkingCollectionId = CollectionId;
				SaveRequest();
			});
		})));
	Menu.AddMenuEntry(LOCTEXT("SaveAsAsset", "Save as Asset (Blueprint node)..."),
		LOCTEXT("SaveAsAssetTip", "Makes a Courier Request asset. Every such asset is a 'Courier: <name>' node in Blueprint graphs, with a pin per {{variable}}."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([this]() { SaveRequestAsAsset(); })));
	Menu.AddMenuEntry(LOCTEXT("CopyCurl", "Copy as cURL"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { CopyCurl(); })));
	Menu.AddMenuEntry(LOCTEXT("Duplicate", "Duplicate"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			FCourierRequestSpec Copy = *Working;
			Copy.Id = FGuid::NewGuid();
			Copy.Name += TEXT(" Copy");
			LoadRequest(Copy, WorkingCollectionId, nullptr);
			bDirty = true;
		})));
	return Menu.MakeWidget();
}

void SCourierWindow::RebuildBody()
{
	if (!BodyHost.IsValid())
	{
		return;
	}
	const FSimpleDelegate Dirty = FSimpleDelegate::CreateSP(this, &SCourierWindow::MarkDirty);
	auto TextEditor = [this](FString* Target, const FText& Hint) -> TSharedRef<SWidget>
	{
		return SNew(SMultiLineEditableTextBox)
			.Font(CourierUI::MonoFont(9))
			.HintText(Hint)
			.Text(FText::FromString(*Target))
			.OnTextChanged_Lambda([this, Target](const FText& Text) { *Target = Text.ToString(); MarkDirty(); });
	};

	TSharedRef<SWidget> Content = SNullWidget::NullWidget;
	switch (Working->BodyType)
	{
	case ECourierBodyType::None:
		Content = SNew(STextBlock).ColorAndOpacity(CourierWindow::Subdued).Text(LOCTEXT("NoBody", "This request has no body."));
		break;

	case ECourierBodyType::Json:
		Content = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 0, 0, 4)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.Text(LOCTEXT("Beautify", "Beautify"))
				.ToolTipText(LOCTEXT("BeautifyTip", "Re-indent the JSON (it must parse; {{variables}} inside strings are fine)"))
				.OnClicked_Lambda([this]()
				{
					if (UCourierLibrary::IsValidJson(Working->Body))
					{
						Working->Body = UCourierLibrary::PrettyPrintJson(Working->Body);
						MarkDirty();
						RebuildBody();
					}
					else
					{
						CourierUI::Notify(LOCTEXT("BadJson", "The body is not valid JSON"), false);
					}
					return FReply::Handled();
				})
			]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				TextEditor(&Working->Body, LOCTEXT("JsonHint", "{\n  \"name\": \"{{name}}\"\n}"))
			];
		break;

	case ECourierBodyType::Raw:
		Content = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
				[
					SNew(STextBlock).Text(LOCTEXT("ContentType", "Content-Type"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					SNew(SEditableTextBox)
					.Text(FText::FromString(Working->RawContentType))
					.OnTextChanged_Lambda([this](const FText& Text) { Working->RawContentType = Text.ToString(); MarkDirty(); })
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				TextEditor(&Working->Body, FText::GetEmpty())
			];
		break;

	case ECourierBodyType::FormUrlEncoded:
	case ECourierBodyType::Multipart:
		Content = SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SCourierFormFieldList)
				.Rows(&Working->FormFields)
				.AllowFiles(Working->BodyType == ECourierBodyType::Multipart)
				.OnChanged(Dirty)
			];
		break;

	case ECourierBodyType::Binary:
		Content = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Top).Padding(0, 0, 4, 0)
			[
				SNew(SEditableTextBox)
				.Font(CourierUI::MonoFont())
				.HintText(LOCTEXT("BinaryHint", "Path of the file to send"))
				.Text_Lambda([this]() { return FText::FromString(Working->Body); })
				.OnTextChanged_Lambda([this](const FText& Text) { Working->Body = Text.ToString(); MarkDirty(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
			[
				SNew(SButton)
				.Text(LOCTEXT("Browse", "..."))
				.OnClicked_Lambda([this]()
				{
					const FString File = CourierUI::OpenFileDialog(TEXT("Pick the body file"), TEXT("All files (*.*)|*.*"));
					if (!File.IsEmpty())
					{
						Working->Body = File;
						MarkDirty();
					}
					return FReply::Handled();
				})
			];
		break;

	case ECourierBodyType::GraphQL:
		Content = SNew(SSplitter)
			+ SSplitter::Slot().Value(0.6f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)[CourierWindow::SectionLabel(LOCTEXT("GqlQuery", "QUERY"))]
				+ SVerticalBox::Slot().FillHeight(1.f)[TextEditor(&Working->GraphQLQuery, LOCTEXT("GqlQueryHint", "query { me { id name } }"))]
			]
			+ SSplitter::Slot().Value(0.4f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 2)[CourierWindow::SectionLabel(LOCTEXT("GqlVariables", "GRAPHQL VARIABLES (JSON)"))]
				+ SVerticalBox::Slot().FillHeight(1.f)[TextEditor(&Working->GraphQLVariables, LOCTEXT("GqlVariablesHint", "{ \"id\": \"{{userId}}\" }"))]
			];
		break;
	}
	BodyHost->SetContent(Content);
}

void SCourierWindow::RebuildAuth()
{
	if (!AuthHost.IsValid())
	{
		return;
	}
	auto Field = [this](const FText& Label, FString* Target, bool bPassword) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(SBox).WidthOverride(110.f)[SNew(STextBlock).Text(Label)]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SEditableTextBox)
				.Font(CourierUI::MonoFont())
				.IsPassword(bPassword)
				.Text(FText::FromString(*Target))
				.OnTextChanged_Lambda([this, Target](const FText& Text) { *Target = Text.ToString(); MarkDirty(); })
			];
	};

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	FCourierAuth& Auth = Working->Auth;
	switch (Auth.Type)
	{
	case ECourierAuthType::Bearer:
		Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("Token", "Token"), &Auth.Token, false)];
		break;
	case ECourierAuthType::Basic:
		Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("Username", "Username"), &Auth.Username, false)];
		Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("Password", "Password"), &Auth.Password, true)];
		break;
	case ECourierAuthType::ApiKey:
		Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("KeyName", "Key"), &Auth.ApiKeyName, false)];
		Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("KeyValue", "Value"), &Auth.ApiKeyValue, false)];
		Box->AddSlot().AutoHeight().Padding(0, 4)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this]() { return Working->Auth.bApiKeyInQuery ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Working->Auth.bApiKeyInQuery = State == ECheckBoxState::Checked; MarkDirty(); })
			[
				SNew(STextBlock).Text(LOCTEXT("KeyInQuery", "Send in the query string instead of a header"))
			]
		];
		break;
	case ECourierAuthType::OAuth2:
		BuildOAuth2Panel(Box);
		break;
	default:
		Box->AddSlot().AutoHeight()
		[
			SNew(STextBlock).ColorAndOpacity(CourierWindow::Subdued).Text(LOCTEXT("NoAuth", "This request does not use authorization."))
		];
		break;
	}
	Box->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
	[
		SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(CourierWindow::Subdued)
		.Text(LOCTEXT("AuthVars", "Tip: keep secrets in an environment variable and type {{token}} here, so they stay out of exported collections."))
	];
	AuthHost->SetContent(Box);
}

// ------------------------------------------------------------------------------------------------
// Response

TSharedRef<SWidget> SCourierWindow::BuildResponsePanel()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
			[
				CourierWindow::SectionLabel(LOCTEXT("Response", "RESPONSE"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text_Lambda([this]()
				{
					if (bSending) return LOCTEXT("Sending", "Sending...");
					if (!LastResponse.IsSet()) return LOCTEXT("NoResponse", "Send a request to see the response.");
					if (!LastResponse->bReceivedResponse) return FText::FromString(LastResponse->Error);
					return FText::Format(LOCTEXT("StatusFmt", "{0} {1}"), FText::AsNumber(LastResponse->StatusCode, &FNumberFormattingOptions::DefaultNoGrouping()), EHttpResponseCodes::GetDescription(EHttpResponseCodes::Type(LastResponse->StatusCode)));
				})
				.ColorAndOpacity_Lambda([this]()
				{
					return LastResponse.IsSet() && !bSending ? CourierUI::StatusColor(LastResponse->bReceivedResponse ? LastResponse->StatusCode : 0) : CourierWindow::Subdued;
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
			[
				SNew(STextBlock)
				.ColorAndOpacity(CourierWindow::Subdued)
				.Text_Lambda([this]()
				{
					return LastResponse.IsSet() && !bSending
						? FText::Format(LOCTEXT("TimeSize", "{0} ms   {1}"), FText::AsNumber(FMath::RoundToInt(LastResponse->ElapsedSeconds * 1000.f)), FText::FromString(CourierUI::FormatBytes(LastResponse->SizeBytes)))
						: FText::GetEmpty();
				})
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)[SNullWidget::NullWidget]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
			[
				SNew(SSegmentedControl<int32>)
				.Value_Lambda([this]() { return ResponseTab; })
				.OnValueChanged_Lambda([this](int32 Value) { ResponseTab = Value; })
				+ SSegmentedControl<int32>::Slot(0).Text(LOCTEXT("RespBody", "Body"))
				+ SSegmentedControl<int32>::Slot(1).Text(LOCTEXT("RespHeaders", "Headers"))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.ToolTipText(LOCTEXT("PrettyTip", "Pretty-print JSON"))
				.IsChecked_Lambda([this]() { return bPrettyResponse ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
				{
					bPrettyResponse = State == ECheckBoxState::Checked;
					if (LastResponse.IsSet())
					{
						OnHttpComplete(*Working, *LastResponse);
					}
				})
				[
					SNew(STextBlock).Text(LOCTEXT("Pretty", "Pretty")).Margin(FMargin(6, 2))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.Text(LOCTEXT("CopyBody", "Copy"))
				.IsEnabled_Lambda([this]() { return LastResponse.IsSet(); })
				.OnClicked_Lambda([this]()
				{
					FPlatformApplicationMisc::ClipboardCopy(*LastResponse->Body);
					CourierUI::Notify(LOCTEXT("BodyCopied", "Response body copied"));
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.Text(LOCTEXT("SaveBody", "Save..."))
				.IsEnabled_Lambda([this]() { return LastResponse.IsSet() && LastResponse->Content.Num() > 0; })
				.OnClicked_Lambda([this]()
				{
					const FString File = CourierUI::SaveFileDialog(TEXT("Save response body"), TEXT("response"), TEXT("All files (*.*)|*.*"));
					if (!File.IsEmpty())
					{
						FFileHelper::SaveArrayToFile(LastResponse->Content, *File);
					}
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex_Lambda([this]() { return ResponseTab; })
			+ SWidgetSwitcher::Slot()
			[
				SNew(SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.Font(CourierUI::MonoFont(9))
				.AlwaysShowScrollbars(true)
				.Text_Lambda([this]() { return ResponseBodyText; })
			]
			+ SWidgetSwitcher::Slot()
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(ResponseHeadersBox, SVerticalBox)
				]
			]
		];
}

void SCourierWindow::RebuildResponseHeaders()
{
	if (!ResponseHeadersBox.IsValid())
	{
		return;
	}
	ResponseHeadersBox->ClearChildren();
	if (!LastResponse.IsSet())
	{
		return;
	}
	for (const TPair<FString, FString>& Header : LastResponse->Headers)
	{
		ResponseHeadersBox->AddSlot().AutoHeight().Padding(0, 1)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(0.35f)
			[
				SNew(STextBlock).Font(CourierUI::MonoFont()).Text(FText::FromString(Header.Key)).ColorAndOpacity(CourierWindow::Subdued)
			]
			+ SHorizontalBox::Slot().FillWidth(0.65f)
			[
				SNew(STextBlock).Font(CourierUI::MonoFont()).Text(FText::FromString(Header.Value)).AutoWrapText(true)
			]
		];
	}
}

// ------------------------------------------------------------------------------------------------
// Stream console (WebSocket and SSE)

TSharedRef<SWidget> SCourierWindow::BuildStreamPanel()
{
	const bool bWebSocket = Working->Protocol == ECourierProtocol::WebSocket;
	const bool bSocketIO = Working->Protocol == ECourierProtocol::SocketIO;
	TSharedRef<SVerticalBox> Panel = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
			[
				CourierWindow::SectionLabel(bWebSocket ? LOCTEXT("Messages", "MESSAGES") : LOCTEXT("Events", "EVENTS"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text_Lambda([this]()
				{
					if (IsStreamOpen()) return LOCTEXT("Connected", "Connected");
					if (bSocketConnecting) return LOCTEXT("Connecting", "Connecting...");
					return LOCTEXT("Disconnected", "Disconnected");
				})
				.ColorAndOpacity_Lambda([this]() { return IsStreamOpen() ? FSlateColor(FLinearColor(0.42f, 0.83f, 0.47f)) : CourierWindow::Subdued; })
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)[SNullWidget::NullWidget]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.Text(LOCTEXT("ClearLog", "Clear"))
				.OnClicked_Lambda([this]() { LogLines.Reset(); LogList->RequestListRefresh(); return FReply::Handled(); })
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(2)
			[
				SAssignNew(LogList, SListView<FLogLinePtr>)
				.ListItemsSource(&LogLines)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SCourierWindow::OnGenerateLogRow)
				.OnMouseButtonDoubleClick_Lambda([](FLogLinePtr Line)
				{
					if (Line.IsValid())
					{
						FPlatformApplicationMisc::ClipboardCopy(*Line->Text);
						CourierUI::Notify(LOCTEXT("LineCopied", "Message copied"));
					}
				})
			]
		];

	if (bSocketIO)
	{
		Panel->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0, 0, 4, 0)
			[
				SNew(SBox).WidthOverride(150.f)
				[
					SNew(SEditableTextBox)
					.Font(CourierUI::MonoFont(9))
					.HintText(LOCTEXT("SioEventHint", "event name"))
					.Text(FText::FromString(ComposerEvent))
					.OnTextChanged_Lambda([this](const FText& Text) { ComposerEvent = Text.ToString(); })
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
			[
				SNew(SBox).MaxDesiredHeight(120.f)
				[
					SAssignNew(Composer, SMultiLineEditableTextBox)
					.Font(CourierUI::MonoFont(9))
					.HintText(LOCTEXT("SioComposerHint", "Data: JSON (sent as is) or text (sent as a string). Ctrl+Enter emits; {{variables}} are filled."))
					.OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& Key)
					{
						if (Key.GetKey() == EKeys::Enter && Key.IsControlDown())
						{
							EmitSocketIOEvent();
							return FReply::Handled();
						}
						return FReply::Unhandled();
					})
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(0, 0, 6, 4)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]() { return bComposerAck ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bComposerAck = State == ECheckBoxState::Checked; })
				.ToolTipText(LOCTEXT("SioAckTip", "Ask the server to acknowledge; its reply shows as an ack line."))
				[
					SNew(STextBlock).Text(LOCTEXT("SioAck", "Ack"))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.Text(LOCTEXT("SioEmit", "Emit"))
				.IsEnabled_Lambda([this]() { return IsStreamOpen(); })
				.OnClicked_Lambda([this]() { EmitSocketIOEvent(); return FReply::Handled(); })
			]
		];
	}
	if (bWebSocket)
	{
		Panel->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 4, 0)
			[
				SNew(SBox).MaxDesiredHeight(120.f)
				[
					SAssignNew(Composer, SMultiLineEditableTextBox)
					.Font(CourierUI::MonoFont(9))
					.HintText(LOCTEXT("ComposerHint", "Message (Ctrl+Enter in the box sends it; {{variables}} are filled)"))
					.OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& Key)
					{
						if (Key.GetKey() == EKeys::Enter && Key.IsControlDown())
						{
							SendWebSocketMessage();
							return FReply::Handled();
						}
						return FReply::Unhandled();
					})
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.Text(LOCTEXT("SendMessage", "Send"))
				.IsEnabled_Lambda([this]() { return IsStreamOpen(); })
				.OnClicked_Lambda([this]() { SendWebSocketMessage(); return FReply::Handled(); })
			]
		];
	}
	return Panel;
}

TSharedRef<ITableRow> SCourierWindow::OnGenerateLogRow(FLogLinePtr Line, const TSharedRef<STableViewBase>& Owner)
{
	FString Glyph;
	FSlateColor Color = FSlateColor::UseForeground();
	switch (Line->Kind)
	{
	case FLogLine::EKind::Sent:		Glyph = TEXT("↑"); Color = FLinearColor(1.f, 0.8f, 0.3f); break;
	case FLogLine::EKind::Received:	Glyph = TEXT("↓"); Color = FLinearColor(0.42f, 0.83f, 0.47f); break;
	case FLogLine::EKind::Error:	Glyph = TEXT("!"); Color = FLinearColor(1.f, 0.42f, 0.4f); break;
	default:						Glyph = TEXT("•"); Color = CourierWindow::Subdued; break;
	}
	return SNew(STableRow<FLogLinePtr>, Owner)
		.Padding(FMargin(4, 2))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Font(CourierUI::MonoFont(8)).ColorAndOpacity(CourierWindow::Subdued).Text(FText::FromString(Line->Time.ToString(TEXT("%H:%M:%S.%s"))))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont")).ColorAndOpacity(Color).Text(FText::FromString(Glyph))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.Visibility(Line->Tag.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				.Font(FAppStyle::GetFontStyle("SmallFontBold"))
				.ColorAndOpacity(FLinearColor(0.95f, 0.55f, 0.85f))
				// SSE ids can be long JSON (Wikimedia's are); the full tag is in the tooltip.
				.Text(FText::FromString(Line->Tag.Len() > 40 ? Line->Tag.Left(39) + TEXT("…") : Line->Tag))
				.ToolTipText(FText::FromString(Line->Tag))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(STextBlock)
				.Font(CourierUI::MonoFont(9))
				.ColorAndOpacity(Line->Kind == FLogLine::EKind::System ? CourierWindow::Subdued : FSlateColor::UseForeground())
				.AutoWrapText(true)
				.Text(FText::FromString(Line->Text.Len() > 20000 ? Line->Text.Left(20000) + TEXT(" ...") : Line->Text))
			]
		];
}

void SCourierWindow::AddLog(FLogLine::EKind Kind, const FString& Text, const FString& InTag)
{
	FLogLinePtr Line = MakeShared<FLogLine>();
	Line->Kind = Kind;
	Line->Text = Text;
	Line->Tag = InTag;
	Line->Time = FDateTime::Now();
	LogLines.Add(Line);
	constexpr int32 MaxLines = 2000;
	if (LogLines.Num() > MaxLines)
	{
		LogLines.RemoveAt(0, LogLines.Num() - MaxLines);
	}
	if (LogList.IsValid())
	{
		LogList->RequestListRefresh();
		LogList->RequestScrollIntoView(Line);
	}
}

bool SCourierWindow::IsStreamOpen() const
{
	return (Socket.IsValid() && Socket->IsConnected()) || (Sse.IsValid() && Sse->IsOpen()) || (SocketIO.IsValid() && SocketIO->IsConnected());
}

void SCourierWindow::ToggleStream()
{
	if (IsStreamOpen() || bSocketConnecting)
	{
		CloseStreams();
		return;
	}
	switch (Working->Protocol)
	{
	case ECourierProtocol::WebSocket:	ConnectWebSocket(); break;
	case ECourierProtocol::SocketIO:	ConnectSocketIO(); break;
	default:							ConnectSse(); break;
	}
}

void SCourierWindow::ConnectWebSocket()
{
	FString Url;
	Socket = UCourierWebSocket::MakeSocket(*Working, GetVariables(), Url);
	if (!Socket.IsValid())
	{
		AddLog(FLogLine::EKind::Error, TEXT("The URL is empty."));
		return;
	}
	bSocketConnecting = true;
	PendingBinary.Reset();
	AddLog(FLogLine::EKind::System, FString::Printf(TEXT("Connecting to %s"), *Url));

	TWeakPtr<SCourierWindow> WeakSelf = StaticCastSharedRef<SCourierWindow>(AsShared());
	Socket->OnConnected().AddLambda([WeakSelf, Url]()
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->bSocketConnecting = false;
			Self->AddLog(FLogLine::EKind::System, FString::Printf(TEXT("Connected to %s"), *Url));
		}
	});
	Socket->OnConnectionError().AddLambda([WeakSelf](const FString& Error)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->bSocketConnecting = false;
			Self->AddLog(FLogLine::EKind::Error, FString::Printf(TEXT("Connection error: %s"), *Error));
		}
	});
	Socket->OnClosed().AddLambda([WeakSelf](int32 StatusCode, const FString& Reason, bool bWasClean)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->bSocketConnecting = false;
			Self->AddLog(FLogLine::EKind::System, FString::Printf(TEXT("Closed (%d%s%s)%s"), StatusCode, Reason.IsEmpty() ? TEXT("") : TEXT(", "), *Reason, bWasClean ? TEXT("") : TEXT(" - not clean")));
		}
	});
	Socket->OnMessage().AddLambda([WeakSelf](const FString& Message)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->AddLog(FLogLine::EKind::Received, Message);
		}
	});
	Socket->OnBinaryMessage().AddLambda([WeakSelf](const void* Data, SIZE_T Size, bool bIsLastFragment)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->PendingBinary.Append(static_cast<const uint8*>(Data), static_cast<int32>(Size));
			if (bIsLastFragment)
			{
				const int32 Preview = FMath::Min(Self->PendingBinary.Num(), 64);
				FString Hex = BytesToHex(Self->PendingBinary.GetData(), Preview);
				Self->AddLog(FLogLine::EKind::Received, FString::Printf(TEXT("%s%s"), *Hex, Self->PendingBinary.Num() > Preview ? TEXT(" ...") : TEXT("")),
					FString::Printf(TEXT("binary %s"), *CourierUI::FormatBytes(Self->PendingBinary.Num())));
				Self->PendingBinary.Reset();
			}
		}
	});
	Socket->Connect();
}

void SCourierWindow::ConnectSse()
{
	FCourierPreparedRequest Prepared;
	FString Error;
	if (!FCourierHttp::Prepare(*Working, GetVariables(), Prepared, Error))
	{
		AddLog(FLogLine::EKind::Error, Error);
		return;
	}
	Sse = MakeShared<FCourierSseConnection, ESPMode::ThreadSafe>();
	TWeakPtr<SCourierWindow> WeakSelf = StaticCastSharedRef<SCourierWindow>(AsShared());
	Sse->OnOpen = [WeakSelf](int32 StatusCode)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->bSocketConnecting = false;
			Self->AddLog(EHttpResponseCodes::IsOk(StatusCode) ? FLogLine::EKind::System : FLogLine::EKind::Error, FString::Printf(TEXT("HTTP %d"), StatusCode));
		}
	};
	Sse->OnEvent = [WeakSelf](const FCourierSseEvent& Event)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->AddLog(FLogLine::EKind::Received, Event.Data, Event.Id.IsEmpty() ? Event.EventName : FString::Printf(TEXT("%s #%s"), *Event.EventName, *Event.Id));
		}
	};
	Sse->OnClosed = [WeakSelf](const FString& InError, int32 StatusCode)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->bSocketConnecting = false;
			Self->AddLog(InError.IsEmpty() ? FLogLine::EKind::System : FLogLine::EKind::Error, InError.IsEmpty() ? FString(TEXT("Stream closed")) : InError);
		}
	};
	bSocketConnecting = true;
	AddLog(FLogLine::EKind::System, FString::Printf(TEXT("%s %s"), *Prepared.Verb, *Prepared.Url));
	Sse->Open(Prepared);
}

void SCourierWindow::CloseStreams()
{
	bSocketConnecting = false;
	if (Socket.IsValid())
	{
		if (Socket->IsConnected())
		{
			Socket->Close();
		}
		else
		{
			// Still handshaking: drop it so no late events arrive.
			Socket->OnConnected().Clear();
			Socket->OnConnectionError().Clear();
			Socket->OnClosed().Clear();
			Socket->OnMessage().Clear();
			Socket->OnBinaryMessage().Clear();
			Socket.Reset();
		}
	}
	if (Sse.IsValid())
	{
		Sse->Close();
	}
	if (SocketIO.IsValid())
	{
		SocketIO->Disconnect();
		SocketIO.Reset();
	}
}

void SCourierWindow::SendWebSocketMessage()
{
	if (!Composer.IsValid() || !Socket.IsValid() || !Socket->IsConnected())
	{
		return;
	}
	const FString Message = FCourierHttp::ResolveVariables(Composer->GetText().ToString(), GetVariables());
	if (Message.IsEmpty())
	{
		return;
	}
	Socket->Send(Message);
	AddLog(FLogLine::EKind::Sent, Message);
}

void SCourierWindow::ConnectSocketIO()
{
	FCourierRequestSpec Spec = *Working;
	Spec.BodyType = ECourierBodyType::None;
	FCourierPreparedRequest Prepared;
	FString Error;
	const TMap<FString, FString> Variables = GetVariables();
	if (!FCourierHttp::Prepare(Spec, Variables, Prepared, Error))
	{
		AddLog(FLogLine::EKind::Error, Error);
		return;
	}
	TMap<FString, FString> Headers;
	for (const TPair<FString, FString>& Header : Prepared.Headers)
	{
		Headers.Add(Header.Key, Header.Value);
	}
	FString Namespace = FCourierHttp::ResolveVariables(Working->SocketIONamespace, Variables).TrimStartAndEnd();
	if (Namespace.IsEmpty())
	{
		Namespace = TEXT("/");
	}

	SocketIO = MakeShared<FCourierSocketIOConnection>();
	TWeakPtr<SCourierWindow> WeakSelf = StaticCastSharedRef<SCourierWindow>(AsShared());
	SocketIO->OnConnected = [WeakSelf, Namespace](const FString& Sid)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->bSocketConnecting = false;
			Self->AddLog(FLogLine::EKind::System, FString::Printf(TEXT("Connected to namespace %s (socket id %s)"), *Namespace, *Sid));
		}
	};
	SocketIO->OnConnectError = [WeakSelf](const FString& Message)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->bSocketConnecting = false;
			Self->AddLog(FLogLine::EKind::Error, FString::Printf(TEXT("Connect error: %s"), *Message));
		}
	};
	SocketIO->OnDisconnected = [WeakSelf](const FString& Reason)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->bSocketConnecting = false;
			Self->AddLog(FLogLine::EKind::System, FString::Printf(TEXT("Disconnected (%s)"), *Reason));
		}
	};
	SocketIO->OnEvent = [WeakSelf](const FString& Name, const FString& Args)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->AddLog(FLogLine::EKind::Received, Args, Name);
		}
	};
	SocketIO->OnAck = [WeakSelf](int32 Id, const FString& Args)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->AddLog(FLogLine::EKind::Received, Args, FString::Printf(TEXT("ack #%d"), Id));
		}
	};
	const FString Auth = FCourierHttp::ResolveVariables(Working->SocketIOAuth, Variables);
	if (!SocketIO->Connect(Prepared.Url, Headers, Namespace, Auth))
	{
		AddLog(FLogLine::EKind::Error, TEXT("The URL is empty."));
		SocketIO.Reset();
		return;
	}
	bSocketConnecting = true;
	AddLog(FLogLine::EKind::System, FString::Printf(TEXT("Connecting to %s"), *SocketIO->GetUrl()));
}

void SCourierWindow::EmitSocketIOEvent()
{
	if (!SocketIO.IsValid() || !SocketIO->IsConnected() || !Composer.IsValid())
	{
		return;
	}
	const TMap<FString, FString> Variables = GetVariables();
	const FString Event = FCourierHttp::ResolveVariables(ComposerEvent, Variables).TrimStartAndEnd();
	if (Event.IsEmpty())
	{
		AddLog(FLogLine::EKind::Error, TEXT("Type an event name first."));
		return;
	}
	const FString Args = FCourierSocketIO::ArgsFromText(FCourierHttp::ResolveVariables(Composer->GetText().ToString(), Variables));
	const int32 AckId = SocketIO->Emit(Event, Args, bComposerAck);
	AddLog(FLogLine::EKind::Sent, Args, bComposerAck ? FString::Printf(TEXT("%s, ack #%d"), *Event, AckId) : Event);
}

void SCourierWindow::BuildOAuth2Panel(const TSharedRef<SVerticalBox>& Box)
{
	FCourierOAuth2& OAuth2 = Working->Auth.OAuth2;
	auto Field = [this](const FText& Label, FString* Target, bool bPassword, const FText& Hint) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(SBox).WidthOverride(140.f)[SNew(STextBlock).Text(Label)]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SEditableTextBox)
				.Font(CourierUI::MonoFont())
				.IsPassword(bPassword)
				.HintText(Hint)
				.Text(FText::FromString(*Target))
				.OnTextChanged_Lambda([this, Target](const FText& Text) { *Target = Text.ToString(); MarkDirty(); })
			];
	};
	auto Combo = [](const FText& Label, TFunction<FText()> Current, TFunction<void(FMenuBuilder&)> Fill) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(SBox).WidthOverride(140.f)[SNew(STextBlock).Text(Label)]
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SComboButton)
				.OnGetMenuContent_Lambda([Fill]() { FMenuBuilder Menu(true, nullptr); Fill(Menu); return Menu.MakeWidget(); })
				.ButtonContent()[SNew(STextBlock).Text_Lambda([Current]() { return Current(); })]
			];
	};

	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		Combo(LOCTEXT("OAuthGrant", "Grant type"),
			[this]() { return StaticEnum<ECourierOAuth2Grant>()->GetDisplayNameTextByValue(static_cast<int64>(Working->Auth.OAuth2.GrantType)); },
			[this](FMenuBuilder& Menu)
			{
				for (ECourierOAuth2Grant Grant : { ECourierOAuth2Grant::ClientCredentials, ECourierOAuth2Grant::Password, ECourierOAuth2Grant::AuthorizationCode })
				{
					Menu.AddMenuEntry(StaticEnum<ECourierOAuth2Grant>()->GetDisplayNameTextByValue(static_cast<int64>(Grant)), FText::GetEmpty(), FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([this, Grant]() { Working->Auth.OAuth2.GrantType = Grant; MarkDirty(); RebuildAuth(); })));
				}
			})
	];
	const bool bCode = OAuth2.GrantType == ECourierOAuth2Grant::AuthorizationCode;
	if (bCode)
	{
		Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("OAuthAuthUrl", "Auth URL"), &OAuth2.AuthUrl, false, LOCTEXT("OAuthAuthUrlHint", "https://id.example.com/authorize"))];
	}
	Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("OAuthTokenUrl", "Access token URL"), &OAuth2.AccessTokenUrl, false, LOCTEXT("OAuthTokenUrlHint", "https://id.example.com/oauth/token"))];
	Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("OAuthClientId", "Client ID"), &OAuth2.ClientId, false, FText::GetEmpty())];
	Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("OAuthClientSecret", "Client secret"), &OAuth2.ClientSecret, true, LOCTEXT("OAuthSecretHint", "{{clientSecret}}  (empty for public clients)"))];
	if (OAuth2.GrantType == ECourierOAuth2Grant::Password)
	{
		Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("Username", "Username"), &OAuth2.Username, false, FText::GetEmpty())];
		Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("Password", "Password"), &OAuth2.Password, true, FText::GetEmpty())];
	}
	if (bCode)
	{
		Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("OAuthRedirect", "Redirect URI"), &OAuth2.RedirectUri, false, LOCTEXT("OAuthRedirectHint", "http://127.0.0.1:8976/callback"))];
	}
	Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("OAuthScope", "Scope"), &OAuth2.Scope, false, LOCTEXT("OAuthScopeHint", "space separated"))];
	Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("OAuthAudience", "Audience"), &OAuth2.Audience, false, LOCTEXT("OAuthAudienceHint", "optional"))];
	Box->AddSlot().AutoHeight().Padding(0, 2)
	[
		Combo(LOCTEXT("OAuthClientAuth", "Client authentication"),
			[this]() { return StaticEnum<ECourierOAuth2ClientAuth>()->GetDisplayNameTextByValue(static_cast<int64>(Working->Auth.OAuth2.ClientAuthentication)); },
			[this](FMenuBuilder& Menu)
			{
				for (ECourierOAuth2ClientAuth Mode : { ECourierOAuth2ClientAuth::BasicHeader, ECourierOAuth2ClientAuth::Body })
				{
					Menu.AddMenuEntry(StaticEnum<ECourierOAuth2ClientAuth>()->GetDisplayNameTextByValue(static_cast<int64>(Mode)), FText::GetEmpty(), FSlateIcon(),
						FUIAction(FExecuteAction::CreateLambda([this, Mode]() { Working->Auth.OAuth2.ClientAuthentication = Mode; MarkDirty(); })));
				}
			})
	];
	if (bCode)
	{
		Box->AddSlot().AutoHeight().Padding(148, 4, 0, 2)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this]() { return Working->Auth.OAuth2.bUsePkce ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Working->Auth.OAuth2.bUsePkce = State == ECheckBoxState::Checked; MarkDirty(); })
			[
				SNew(STextBlock).Text(LOCTEXT("OAuthPkce", "Use PKCE (S256)"))
			]
		];
	}
	Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("OAuthPrefix", "Header prefix"), &OAuth2.HeaderPrefix, false, LOCTEXT("OAuthPrefixHint", "Bearer"))];
	Box->AddSlot().AutoHeight().Padding(0, 2)[Field(LOCTEXT("OAuthToken", "Access token"), &OAuth2.AccessToken, false, LOCTEXT("OAuthTokenHint", "empty: Courier gets one when you send"))];

	Box->AddSlot().AutoHeight().Padding(0, 8, 0, 2)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
			.Text(LOCTEXT("OAuthGetToken", "Get New Access Token"))
			.IsEnabled_Lambda([this]() { return !bFetchingToken; })
			.OnClicked_Lambda([this]() { FetchOAuth2Token(); return FReply::Handled(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("OAuthClear", "Clear Tokens"))
			.ToolTipText(LOCTEXT("OAuthClearTip", "Forget every cached OAuth 2.0 token"))
			.OnClicked_Lambda([]() { FCourierOAuth2Client::ClearCache(); return FReply::Handled(); })
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Font(CourierUI::MonoFont(9))
			.Text_Lambda([this]() { return GetOAuth2StatusText(); })
			.ColorAndOpacity_Lambda([this]()
			{
				FCourierOAuth2Token Token;
				const bool bHas = FCourierOAuth2Client::GetCachedToken(FCourierOAuth2Client::Resolve(Working->Auth.OAuth2, GetVariables()), Token);
				return bHas && (Token.ExpiresAt.GetTicks() == 0 || Token.ExpiresAt > FDateTime::UtcNow()) ? FSlateColor(FLinearColor(0.42f, 0.83f, 0.47f)) : CourierWindow::Subdued;
			})
		]
	];
	Box->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
	[
		SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(CourierWindow::Subdued)
		.Text(bCode
			? LOCTEXT("OAuthCodeHelp", "Send (or Get New Access Token) opens your browser at the Auth URL and catches the redirect on the Redirect URI, which must be a 127.0.0.1 or localhost address registered with the provider. Tokens are cached and refreshed automatically.")
			: LOCTEXT("OAuthHelp", "Send gets a token when there is none, refreshes it when it expires, and adds the Authorization header. Tokens are cached per token URL, client and scope."))
	];
}

FText SCourierWindow::GetOAuth2StatusText() const
{
	if (bFetchingToken)
	{
		return LOCTEXT("OAuthFetching", "Getting a token...");
	}
	const FCourierOAuth2 Resolved = FCourierOAuth2Client::Resolve(Working->Auth.OAuth2, GetVariables());
	if (!Resolved.AccessToken.IsEmpty())
	{
		return LOCTEXT("OAuthGiven", "Using the access token typed above");
	}
	FCourierOAuth2Token Token;
	if (!FCourierOAuth2Client::GetCachedToken(Resolved, Token))
	{
		return LOCTEXT("OAuthNoToken", "No token yet");
	}
	const FString Preview = Token.AccessToken.Len() > 14 ? Token.AccessToken.Left(10) + TEXT("...") + Token.AccessToken.Right(4) : Token.AccessToken;
	return FText::Format(LOCTEXT("OAuthHasToken", "{0}   {1}{2}"), FText::FromString(Preview), CourierWindow::DescribeExpiry(Token.ExpiresAt),
		Token.RefreshToken.IsEmpty() ? FText::GetEmpty() : LOCTEXT("OAuthRefreshable", ", refresh token kept"));
}

void SCourierWindow::FetchOAuth2Token()
{
	bFetchingToken = true;
	TWeakPtr<SCourierWindow> WeakSelf = StaticCastSharedRef<SCourierWindow>(AsShared());
	FCourierOAuth2Client::GetToken(FCourierOAuth2Client::Resolve(Working->Auth.OAuth2, GetVariables()), [WeakSelf](const FCourierOAuth2Token& Token)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->bFetchingToken = false;
		}
		CourierUI::Notify(Token.bSuccess
			? LOCTEXT("OAuthGot", "OAuth 2.0: got a new access token")
			: FText::Format(LOCTEXT("OAuthFailed", "OAuth 2.0: {0}"), FText::FromString(Token.Error)), Token.bSuccess);
	}, true);
}

TSharedRef<SWidget> SCourierWindow::BuildCookiesPanel()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.ColorAndOpacity(CourierWindow::Subdued)
				.Text_Lambda([this]() { return FText::Format(LOCTEXT("CookieCount", "{0} cookies"), FText::AsNumber(CookieItems.Num())); })
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.Text(LOCTEXT("ClearCookies", "Clear All"))
				.OnClicked_Lambda([]()
				{
					if (CourierUI::Confirm(LOCTEXT("ConfirmClearCookies", "Remove every cookie from the jar?")))
					{
						FCourierCookieJar::Get().Clear();
					}
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SAssignNew(CookieList, SListView<TSharedPtr<FCourierCookie>>)
			.ListItemsSource(&CookieItems)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SCourierWindow::OnGenerateCookieRow)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(4)
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(CourierWindow::Subdued).Font(FAppStyle::GetFontStyle("SmallFont"))
			.Text(LOCTEXT("CookiesHelp", "Requests send matching cookies and keep the ones servers set (per request: Settings > Send and store cookies). Kept in Saved/Courier/Cookies.json."))
		];
}

void SCourierWindow::RefreshCookies()
{
	CookieItems.Reset();
	TArray<FCourierCookie> All = FCourierCookieJar::Get().GetCookies();
	All.Sort([](const FCourierCookie& A, const FCourierCookie& B) { return A.Domain == B.Domain ? A.Name < B.Name : A.Domain < B.Domain; });
	for (const FCourierCookie& Cookie : All)
	{
		CookieItems.Add(MakeShared<FCourierCookie>(Cookie));
	}
	if (CookieList.IsValid())
	{
		CookieList->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SCourierWindow::OnGenerateCookieRow(TSharedPtr<FCourierCookie> Cookie, const TSharedRef<STableViewBase>& Owner)
{
	const FText Expiry = Cookie->Expires.GetTicks() == 0 ? LOCTEXT("SessionCookie", "session") : CourierWindow::DescribeExpiry(Cookie->Expires);
	TArray<FString> Flags;
	if (Cookie->bSecure) { Flags.Add(TEXT("Secure")); }
	if (Cookie->bHttpOnly) { Flags.Add(TEXT("HttpOnly")); }
	const FString Where = (Cookie->bHostOnly ? FString() : FString(TEXT("."))) + Cookie->Domain + (Cookie->Path == TEXT("/") ? FString() : Cookie->Path);
	const FString Value = Cookie->Value.Len() > 40 ? Cookie->Value.Left(37) + TEXT("...") : Cookie->Value;
	return SNew(STableRow<TSharedPtr<FCourierCookie>>, Owner)
		.Padding(FMargin(4, 3))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Font(CourierUI::MonoFont(9))
					.Text(FText::FromString(Cookie->Name + TEXT("=") + Value))
					.ToolTipText(FText::FromString(Cookie->Value))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(CourierWindow::Subdued)
					.Text(FText::Format(LOCTEXT("CookieMeta", "{0}   {1}{2}"), FText::FromString(Where), Expiry,
						Flags.Num() ? FText::FromString(TEXT("   ") + FString::Join(Flags, TEXT(", "))) : FText::GetEmpty()))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("DeleteCookie", "Delete this cookie"))
				.OnClicked_Lambda([Cookie]() { FCourierCookieJar::Get().Remove(Cookie->Domain, Cookie->Name); return FReply::Handled(); })
				[
					SNew(SImage).Image(FAppStyle::GetBrush("Icons.X")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		];
}

// ------------------------------------------------------------------------------------------------
// Collection editor

void SCourierWindow::ShowCollectionEditor(const FGuid& CollectionId)
{
	const FCourierCollection* Collection = FCourierWorkspace::Get().FindCollection(CollectionId);
	if (!Collection)
	{
		return;
	}
	bShowingCollection = true;
	EditingCollection = MakeShared<FCourierCollection>(*Collection);
	RightHost->SetContent(BuildCollectionEditor());
}

TSharedRef<SWidget> SCourierWindow::BuildCollectionEditor()
{
	return SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(12)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 6, 0)
				[
					SNew(SEditableTextBox)
					.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
					.Text(FText::FromString(EditingCollection->Name))
					.OnTextChanged_Lambda([this](const FText& Text) { EditingCollection->Name = Text.ToString(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Text(LOCTEXT("SaveCollection", "Save"))
					.OnClicked_Lambda([this]()
					{
						if (FCourierCollection* Collection = FCourierWorkspace::Get().FindCollection(EditingCollection->Id))
						{
							Collection->Name = EditingCollection->Name;
							Collection->Description = EditingCollection->Description;
							Collection->Variables = EditingCollection->Variables;
							FCourierWorkspace::Get().Save();
							CourierUI::Notify(LOCTEXT("CollectionSaved", "Collection saved"));
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("ExportCollection", "Export to Postman"))
					.OnClicked_Lambda([this]() { ExportCollection(EditingCollection->Id); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("CollectionNewRequest", "New Request"))
					.OnClicked_Lambda([this]() { NewRequest(ECourierProtocol::Http, ECourierBodyType::None, EditingCollection->Id); return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				CourierWindow::SectionLabel(LOCTEXT("CollectionVariables", "COLLECTION VARIABLES"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(CourierWindow::Subdued)
				.Text(LOCTEXT("CollectionVariablesHelp", "Every request in this collection can use these as {{name}}. The active environment overrides them."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
			[
				SNew(SCourierKeyValueList).Rows(&EditingCollection->Variables).KeyHint(LOCTEXT("VarHint", "Variable"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				CourierWindow::SectionLabel(LOCTEXT("Description", "DESCRIPTION"))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).MinDesiredHeight(120.f)
				[
					SNew(SMultiLineEditableTextBox)
					.AutoWrapText(true)
					.Text(FText::FromString(EditingCollection->Description))
					.OnTextChanged_Lambda([this](const FText& Text) { EditingCollection->Description = Text.ToString(); })
				]
			]
		];
}

// ------------------------------------------------------------------------------------------------
// Request lifecycle

void SCourierWindow::NewRequest(ECourierProtocol Protocol, ECourierBodyType BodyType, const FGuid& CollectionId, const FString& Folder)
{
	if (!ResolveUnsavedChanges())
	{
		return;
	}
	FCourierRequestSpec Spec;
	Spec.Id = FGuid::NewGuid();
	Spec.Protocol = Protocol;
	Spec.Folder = Folder;
	Spec.BodyType = BodyType;
	switch (Protocol)
	{
	case ECourierProtocol::WebSocket:
		Spec.Name = TEXT("New WebSocket");
		break;
	case ECourierProtocol::ServerSentEvents:
		Spec.Name = TEXT("New Event Stream");
		break;
	case ECourierProtocol::SocketIO:
		Spec.Name = TEXT("New Socket.IO");
		break;
	default:
		Spec.Name = BodyType == ECourierBodyType::GraphQL ? TEXT("New GraphQL Request") : TEXT("New Request");
		Spec.Verb = BodyType == ECourierBodyType::GraphQL ? ECourierVerb::Post : ECourierVerb::Get;
		break;
	}
	LoadRequest(Spec, CollectionId, nullptr);
	bDirty = CollectionId.IsValid();
}

void SCourierWindow::LoadRequest(const FCourierRequestSpec& Spec, const FGuid& CollectionId, UCourierRequestAsset* Asset)
{
	CloseStreams();
	if (InFlight.IsValid())
	{
		InFlight->OnProcessRequestComplete().Unbind();
		InFlight->CancelRequest();
		InFlight.Reset();
	}
	bSending = false;
	Socket.Reset();
	Sse.Reset();
	SocketIO.Reset();
	LogLines.Reset();
	LastResponse.Reset();
	ResponseBodyText = FText::GetEmpty();

	Working = MakeShared<FCourierRequestSpec>(Spec);
	if (!Working->Id.IsValid())
	{
		Working->Id = FGuid::NewGuid();
	}
	WorkingCollectionId = CollectionId;
	SourceAsset = Asset;
	bDirty = false;
	if (RequestTab == 2 && CourierWindow::HasNoBody(Spec.Protocol))
	{
		RequestTab = 0;
	}
	ShowRequestEditor();
	RefreshTree();
}

void SCourierWindow::OpenRequestAsset(UCourierRequestAsset* Asset)
{
	if (Asset && ResolveUnsavedChanges())
	{
		LoadRequest(Asset->Request, FGuid(), Asset);
	}
}

bool SCourierWindow::ResolveUnsavedChanges()
{
	if (!bDirty)
	{
		return true;
	}
	const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNoCancel,
		FText::Format(LOCTEXT("UnsavedChanges", "Save your changes to '{0}' first?"), FText::FromString(Working->Name)));
	if (Answer == EAppReturnType::Cancel)
	{
		return false;
	}
	if (Answer == EAppReturnType::Yes)
	{
		SaveRequest();
	}
	bDirty = false;
	return true;
}

void SCourierWindow::PickCollectionThen(TFunction<void(const FGuid&)> Then)
{
	FCourierWorkspace& Workspace = FCourierWorkspace::Get();
	if (Workspace.Data.Collections.Num() == 0)
	{
		const FGuid Id = Workspace.AddCollection(TEXT("My Collection")).Id;
		ExpandedKeys.Add(TEXT("C:") + Id.ToString());
		Then(Id);
		return;
	}
	if (Workspace.Data.Collections.Num() == 1)
	{
		Then(Workspace.Data.Collections[0].Id);
		return;
	}
	FMenuBuilder Menu(true, nullptr);
	Menu.BeginSection(NAME_None, LOCTEXT("PickCollection", "Save into collection"));
	for (const FCourierCollection& Collection : Workspace.Data.Collections)
	{
		const FGuid Id = Collection.Id;
		Menu.AddMenuEntry(FText::FromString(Collection.Name), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([Then, Id]() { Then(Id); })));
	}
	Menu.EndSection();
	FSlateApplication::Get().PushMenu(AsShared(), FWidgetPath(), Menu.MakeWidget(), FSlateApplication::Get().GetCursorPos(), FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
}

void SCourierWindow::SaveRequest()
{
	if (UCourierRequestAsset* Asset = SourceAsset.Get())
	{
		Asset->Modify();
		Asset->Request = *Working;
		Asset->MarkPackageDirty();
		FBlueprintActionDatabase::Get().RefreshAssetActions(Asset);
		bDirty = false;
		CourierUI::Notify(FText::Format(LOCTEXT("SavedToAsset", "Saved to {0} (save the asset to keep it)"), FText::FromString(Asset->GetName())));
		return;
	}
	if (!FCourierWorkspace::Get().FindCollection(WorkingCollectionId))
	{
		PickCollectionThen([this](const FGuid& CollectionId)
		{
			WorkingCollectionId = CollectionId;
			SaveRequest();
		});
		return;
	}
	FCourierWorkspace::Get().StoreRequest(WorkingCollectionId, *Working);
	bDirty = false;
	FCourierWorkspace::Get().Save();
}

void SCourierWindow::SaveRequestAsAsset()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UCourierRequestAssetFactory* Factory = NewObject<UCourierRequestAssetFactory>();
	const FString DefaultName = ObjectTools::SanitizeObjectName(TEXT("CR_") + Working->Name.Replace(TEXT(" "), TEXT("")));
	UCourierRequestAsset* Asset = Cast<UCourierRequestAsset>(AssetTools.CreateAssetWithDialog(DefaultName, TEXT("/Game/Courier"), UCourierRequestAsset::StaticClass(), Factory));
	if (!Asset)
	{
		return;
	}
	Asset->Request = *Working;
	Asset->Request.Id = FGuid::NewGuid();

	// Collection variables the request uses become the asset's defaults; environment values
	// (often secrets) are left for the caller to pass in.
	const TArray<FString> Used = FCourierHttp::FindVariableNames(*Working);
	if (const FCourierCollection* Collection = FCourierWorkspace::Get().FindCollection(WorkingCollectionId))
	{
		for (const FCourierKeyValue& Row : Collection->Variables)
		{
			if (Row.bEnabled && Used.Contains(Row.Key))
			{
				Asset->DefaultVariables.Add(Row);
			}
		}
	}
	Asset->MarkPackageDirty();
	FEditorFileUtils::PromptForCheckoutAndSave({ Asset->GetPackage() }, false, false);
	FBlueprintActionDatabase::Get().RefreshAssetActions(Asset);
	CourierUI::Notify(FText::Format(LOCTEXT("AssetMade", "Made {0}. In a Blueprint graph, right-click and search \"Courier: {0}\"."), FText::FromString(Asset->GetName())));
}

void SCourierWindow::CopyCurl()
{
	FPlatformApplicationMisc::ClipboardCopy(*FCourierHttp::ToCurl(*Working, GetVariables()));
	CourierUI::Notify(LOCTEXT("CurlCopied", "cURL command copied"));
}

TMap<FString, FString> SCourierWindow::GetVariables() const
{
	TMap<FString, FString> Variables;
	if (const UCourierRequestAsset* Asset = SourceAsset.Get())
	{
		Variables = FCourierHttp::ToVariableMap(Asset->DefaultVariables);
	}
	Variables.Append(FCourierWorkspace::Get().GetVariables(WorkingCollectionId));
	return Variables;
}

FString SCourierWindow::GetResolvedUrlPreview() const
{
	if (Working->Url.IsEmpty())
	{
		return FString();
	}
	return FCourierHttp::BuildUrl(*Working, GetVariables());
}

FText SCourierWindow::GetUnresolvedVariablesText() const
{
	const TMap<FString, FString> Variables = GetVariables();
	TArray<FString> Missing;
	for (const FString& Name : FCourierHttp::FindVariableNames(*Working))
	{
		if (!Variables.Contains(Name))
		{
			Missing.Add(TEXT("{{") + Name + TEXT("}}"));
		}
	}
	return Missing.Num() > 0 ? FText::Format(LOCTEXT("Unresolved", "not set: {0}"), FText::FromString(FString::Join(Missing, TEXT(" ")))) : FText::GetEmpty();
}

// ------------------------------------------------------------------------------------------------
// HTTP

FText SCourierWindow::GetSendButtonText() const
{
	if (Working->Protocol == ECourierProtocol::Http)
	{
		return bSending ? LOCTEXT("Cancel", "Cancel") : LOCTEXT("Send", "Send");
	}
	return IsStreamOpen() || bSocketConnecting ? LOCTEXT("Disconnect", "Disconnect") : LOCTEXT("Connect", "Connect");
}

void SCourierWindow::SendHttp()
{
	if (bSending)
	{
		bCancelled = true;
		if (InFlight.IsValid())
		{
			InFlight->CancelRequest();
		}
		return;
	}
	bSending = true;
	bCancelled = false;
	const FCourierRequestSpec Snapshot = *Working;
	TWeakPtr<SCourierWindow> WeakSelf = StaticCastSharedRef<SCourierWindow>(AsShared());
	InFlight = FCourierHttp::Send(Snapshot, GetVariables(), [WeakSelf, Snapshot](const FCourierResponse& Response)
	{
		if (const TSharedPtr<SCourierWindow> Self = WeakSelf.Pin())
		{
			Self->bSending = false;
			Self->InFlight.Reset();
			FCourierResponse Shown = Response;
			if (Self->bCancelled && !Shown.bReceivedResponse)
			{
				Shown.Error = TEXT("Cancelled");
			}
			Self->OnHttpComplete(Snapshot, Shown);
			FCourierWorkspace::Get().AddHistory(Snapshot, Shown);
		}
	});
}

void SCourierWindow::OnHttpComplete(const FCourierRequestSpec& Sent, const FCourierResponse& Response)
{
	LastResponse = Response;
	FString Display;
	if (!Response.bReceivedResponse)
	{
		Display = Response.Error;
	}
	else if (Response.Content.Num() == 0)
	{
		Display = TEXT("(empty body)");
	}
	else if (!CourierWindow::LooksLikeText(Response))
	{
		Display = FString::Printf(TEXT("(binary body: %s, %s) - use Save... to write it to a file."), *CourierUI::FormatBytes(Response.SizeBytes), *Response.ContentType);
	}
	else
	{
		Display = bPrettyResponse && UCourierLibrary::IsValidJson(Response.Body) ? UCourierLibrary::PrettyPrintJson(Response.Body) : Response.Body;
		if (Display.Len() > CourierWindow::MaxDisplayChars)
		{
			Display = Display.Left(CourierWindow::MaxDisplayChars) + TEXT("\n\n... (truncated for display; Copy and Save have the full body)");
		}
	}
	ResponseBodyText = FText::FromString(Display);
	RebuildResponseHeaders();
}

// ------------------------------------------------------------------------------------------------
// Import / export

void SCourierWindow::ImportFile()
{
	const FString File = CourierUI::OpenFileDialog(TEXT("Import Postman collection or environment"), TEXT("Postman JSON (*.json)|*.json"));
	if (File.IsEmpty())
	{
		return;
	}
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *File))
	{
		CourierUI::Notify(FText::Format(LOCTEXT("ReadFailed", "Cannot read {0}"), FText::FromString(File)), false);
		return;
	}
	FCourierWorkspace& Workspace = FCourierWorkspace::Get();
	FString Error;
	if (FCourierPostman::IsEnvironmentJson(Json))
	{
		FCourierEnvironment Environment;
		if (!FCourierPostman::ImportEnvironment(Json, Environment, Error))
		{
			CourierUI::Notify(FText::FromString(Error), false);
			return;
		}
		Workspace.Data.Environments.Add(Environment);
		EditingEnvironmentId = Environment.Id;
		SidebarTab = 1;
		Workspace.Save();
		CourierUI::Notify(FText::Format(LOCTEXT("EnvImported", "Imported environment '{0}' ({1} variables)"), FText::FromString(Environment.Name), Environment.Variables.Num()));
		return;
	}
	FCourierCollection Collection;
	if (!FCourierPostman::ImportCollection(Json, Collection, Error))
	{
		CourierUI::Notify(FText::FromString(Error), false);
		return;
	}
	ExpandedKeys.Add(TEXT("C:") + Collection.Id.ToString());
	Workspace.Data.Collections.Add(Collection);
	SidebarTab = 0;
	Workspace.Save();
	CourierUI::Notify(FText::Format(LOCTEXT("CollectionImported", "Imported '{0}' ({1} requests)"), FText::FromString(Collection.Name), Collection.Requests.Num()));
}

void SCourierWindow::ExportCollection(const FGuid& CollectionId)
{
	const FCourierCollection* Collection = FCourierWorkspace::Get().FindCollection(CollectionId);
	if (!Collection)
	{
		return;
	}
	const FString File = CourierUI::SaveFileDialog(TEXT("Export Postman collection"), Collection->Name + TEXT(".postman_collection.json"), TEXT("Postman collection (*.json)|*.json"));
	if (File.IsEmpty())
	{
		return;
	}
	FFileHelper::SaveStringToFile(FCourierPostman::ExportCollection(*Collection), *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	CourierUI::Notify(FText::Format(LOCTEXT("Exported", "Exported to {0}"), FText::FromString(File)));
}

void SCourierWindow::OnWorkspaceChanged()
{
	RefreshTree();
	RefreshHistory();

	// Rebuilding the environment panel drops its unsaved edits, so only do it when the list changed.
	const FCourierWorkspaceData& Data = FCourierWorkspace::Get().Data;
	FString Signature = Data.ActiveEnvironment.ToString() + EditingEnvironmentId.ToString();
	for (const FCourierEnvironment& Environment : Data.Environments)
	{
		Signature += Environment.Id.ToString() + Environment.Name;
	}
	if (Signature != EnvironmentSignature)
	{
		EnvironmentSignature = Signature;
		RebuildEnvironments();
	}
}

#undef LOCTEXT_NAMESPACE
