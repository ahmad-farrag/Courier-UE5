#include "SCourierWidgets.h"

#include "CourierHttp.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Courier"

namespace CourierUI
{
	FSlateColor VerbColor(ECourierVerb Verb)
	{
		switch (Verb)
		{
		case ECourierVerb::Get:		return FLinearColor(0.42f, 0.83f, 0.47f);
		case ECourierVerb::Post:	return FLinearColor(1.00f, 0.80f, 0.30f);
		case ECourierVerb::Put:		return FLinearColor(0.35f, 0.62f, 1.00f);
		case ECourierVerb::Patch:	return FLinearColor(0.75f, 0.55f, 1.00f);
		case ECourierVerb::Delete:	return FLinearColor(1.00f, 0.42f, 0.40f);
		default:					return FLinearColor(0.70f, 0.70f, 0.70f);
		}
	}

	FSlateColor StatusColor(int32 StatusCode)
	{
		if (StatusCode >= 200 && StatusCode < 300) return FLinearColor(0.42f, 0.83f, 0.47f);
		if (StatusCode >= 300 && StatusCode < 400) return FLinearColor(0.35f, 0.62f, 1.00f);
		if (StatusCode >= 400 && StatusCode < 500) return FLinearColor(1.00f, 0.70f, 0.30f);
		return FLinearColor(1.00f, 0.42f, 0.40f);
	}

	FString BadgeText(const FCourierRequestSpec& Spec)
	{
		switch (Spec.Protocol)
		{
		case ECourierProtocol::WebSocket:			return TEXT("WS");
		case ECourierProtocol::ServerSentEvents:	return TEXT("SSE");
		case ECourierProtocol::SocketIO:			return TEXT("SIO");
		default:
			if (Spec.BodyType == ECourierBodyType::GraphQL)
			{
				return TEXT("GQL");
			}
			return Spec.Verb == ECourierVerb::Delete ? FString(TEXT("DEL")) : FCourierHttp::VerbToString(Spec.Verb, Spec.CustomVerb).Left(4);
		}
	}

	FSlateColor BadgeColor(const FCourierRequestSpec& Spec)
	{
		switch (Spec.Protocol)
		{
		case ECourierProtocol::WebSocket:			return FLinearColor(0.30f, 0.85f, 0.85f);
		case ECourierProtocol::ServerSentEvents:	return FLinearColor(0.95f, 0.55f, 0.85f);
		case ECourierProtocol::SocketIO:			return FLinearColor(0.55f, 0.75f, 1.00f);
		default:
			return Spec.BodyType == ECourierBodyType::GraphQL ? FSlateColor(FLinearColor(0.90f, 0.40f, 0.70f)) : VerbColor(Spec.Verb);
		}
	}

	FSlateFontInfo MonoFont(int32 Size)
	{
		return FCoreStyle::GetDefaultFontStyle("Mono", Size);
	}

	FString FormatBytes(int64 Bytes)
	{
		if (Bytes < 1024) return FString::Printf(TEXT("%lld B"), Bytes);
		if (Bytes < 1024 * 1024) return FString::Printf(TEXT("%.1f KB"), Bytes / 1024.0);
		return FString::Printf(TEXT("%.2f MB"), Bytes / (1024.0 * 1024.0));
	}

	TOptional<FString> PromptForText(const FText& Title, const FText& Label, const FString& Default)
	{
		TOptional<FString> Result;
		TSharedPtr<SEditableTextBox> TextBox;
		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(Title)
			.SizingRule(ESizingRule::Autosized)
			.SupportsMaximize(false)
			.SupportsMinimize(false);

		auto Accept = [&Result, &TextBox, &Window]()
		{
			Result = TextBox->GetText().ToString().TrimStartAndEnd();
			Window->RequestDestroyWindow();
		};

		Window->SetContent(
			SNew(SBox)
			.MinDesiredWidth(380.f)
			.Padding(12.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(STextBlock).Text(Label)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
				[
					SAssignNew(TextBox, SEditableTextBox)
					.Text(FText::FromString(Default))
					.SelectAllTextWhenFocused(true)
					.OnTextCommitted_Lambda([Accept](const FText&, ETextCommit::Type Commit)
					{
						if (Commit == ETextCommit::OnEnter)
						{
							Accept();
						}
					})
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.Text(LOCTEXT("PromptOk", "OK"))
						.OnClicked_Lambda([Accept]() { Accept(); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("PromptCancel", "Cancel"))
						.OnClicked_Lambda([&Window]() { Window->RequestDestroyWindow(); return FReply::Handled(); })
					]
				]
			]);

		Window->SetWidgetToFocusOnActivate(TextBox);
		FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());
		return Result;
	}

	bool Confirm(const FText& Message)
	{
		return FMessageDialog::Open(EAppMsgType::YesNo, Message) == EAppReturnType::Yes;
	}

	void Notify(const FText& Message, bool bSuccess)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = bSuccess ? 3.f : 6.f;
		Info.bUseSuccessFailIcons = true;
		const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}

	FString OpenFileDialog(const FString& Title, const FString& Filter)
	{
		IDesktopPlatform* Platform = FDesktopPlatformModule::Get();
		TArray<FString> Files;
		const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		if (Platform && Platform->OpenFileDialog(Parent, Title, FPaths::ProjectDir(), FString(), Filter, EFileDialogFlags::None, Files) && Files.Num() > 0)
		{
			return FPaths::ConvertRelativePathToFull(Files[0]);
		}
		return FString();
	}

	FString SaveFileDialog(const FString& Title, const FString& DefaultFile, const FString& Filter)
	{
		IDesktopPlatform* Platform = FDesktopPlatformModule::Get();
		TArray<FString> Files;
		const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		if (Platform && Platform->SaveFileDialog(Parent, Title, FPaths::ProjectDir(), DefaultFile, Filter, EFileDialogFlags::None, Files) && Files.Num() > 0)
		{
			return FPaths::ConvertRelativePathToFull(Files[0]);
		}
		return FString();
	}
}

// ---- Key / value list ----

void SCourierKeyValueList::Construct(const FArguments& InArgs)
{
	Rows = InArgs._Rows;
	KeyHint = InArgs._KeyHint.IsEmpty() ? LOCTEXT("KeyHint", "Key") : InArgs._KeyHint;
	ValueHint = InArgs._ValueHint.IsEmpty() ? LOCTEXT("ValueHint", "Value") : InArgs._ValueHint;
	OnChanged = InArgs._OnChanged;

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SAssignNew(RowBox, SVerticalBox)
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0, 4, 0, 0)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked_Lambda([this]()
			{
				if (Rows)
				{
					Rows->AddDefaulted();
					Rebuild();
					OnChanged.ExecuteIfBound();
				}
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
				[
					SNew(SImage).Image(FAppStyle::GetBrush("Icons.Plus")).ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("AddRow", "Add"))
				]
			]
		]
	];
	Rebuild();
}

void SCourierKeyValueList::Rebuild()
{
	RowBox->ClearChildren();
	if (!Rows)
	{
		return;
	}
	for (int32 Index = 0; Index < Rows->Num(); ++Index)
	{
		RowBox->AddSlot().AutoHeight().Padding(0, 1)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this, Index]() { return Rows->IsValidIndex(Index) && (*Rows)[Index].bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this, Index](ECheckBoxState State)
				{
					if (Rows->IsValidIndex(Index)) { (*Rows)[Index].bEnabled = State == ECheckBoxState::Checked; OnChanged.ExecuteIfBound(); }
				})
			]
			+ SHorizontalBox::Slot().FillWidth(0.4f).Padding(0, 0, 4, 0)
			[
				SNew(SEditableTextBox)
				.Font(CourierUI::MonoFont())
				.HintText(KeyHint)
				.Text(FText::FromString((*Rows)[Index].Key))
				.OnTextChanged_Lambda([this, Index](const FText& Text)
				{
					if (Rows->IsValidIndex(Index)) { (*Rows)[Index].Key = Text.ToString(); OnChanged.ExecuteIfBound(); }
				})
			]
			+ SHorizontalBox::Slot().FillWidth(0.6f).Padding(0, 0, 4, 0)
			[
				SNew(SEditableTextBox)
				.Font(CourierUI::MonoFont())
				.HintText(ValueHint)
				.Text(FText::FromString((*Rows)[Index].Value))
				.OnTextChanged_Lambda([this, Index](const FText& Text)
				{
					if (Rows->IsValidIndex(Index)) { (*Rows)[Index].Value = Text.ToString(); OnChanged.ExecuteIfBound(); }
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("RemoveRow", "Remove"))
				.OnClicked_Lambda([this, Index]()
				{
					if (Rows->IsValidIndex(Index))
					{
						Rows->RemoveAt(Index);
						Rebuild();
						OnChanged.ExecuteIfBound();
					}
					return FReply::Handled();
				})
				[
					SNew(SImage).Image(FAppStyle::GetBrush("Icons.X")).ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		];
	}
}

// ---- Form field list ----

void SCourierFormFieldList::Construct(const FArguments& InArgs)
{
	Rows = InArgs._Rows;
	bAllowFiles = InArgs._AllowFiles;
	OnChanged = InArgs._OnChanged;

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SAssignNew(RowBox, SVerticalBox)
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0, 4, 0, 0)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked_Lambda([this]()
			{
				if (Rows)
				{
					Rows->AddDefaulted();
					Rebuild();
					OnChanged.ExecuteIfBound();
				}
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
				[
					SNew(SImage).Image(FAppStyle::GetBrush("Icons.Plus")).ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("AddField", "Add field"))
				]
			]
		]
	];
	Rebuild();
}

void SCourierFormFieldList::Rebuild()
{
	RowBox->ClearChildren();
	if (!Rows)
	{
		return;
	}
	for (int32 Index = 0; Index < Rows->Num(); ++Index)
	{
		const bool bIsFile = bAllowFiles && (*Rows)[Index].bIsFile;
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this, Index]() { return Rows->IsValidIndex(Index) && (*Rows)[Index].bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this, Index](ECheckBoxState State)
				{
					if (Rows->IsValidIndex(Index)) { (*Rows)[Index].bEnabled = State == ECheckBoxState::Checked; OnChanged.ExecuteIfBound(); }
				})
			]
			+ SHorizontalBox::Slot().FillWidth(0.35f).Padding(0, 0, 4, 0)
			[
				SNew(SEditableTextBox)
				.Font(CourierUI::MonoFont())
				.HintText(LOCTEXT("FieldKeyHint", "Key"))
				.Text(FText::FromString((*Rows)[Index].Key))
				.OnTextChanged_Lambda([this, Index](const FText& Text)
				{
					if (Rows->IsValidIndex(Index)) { (*Rows)[Index].Key = Text.ToString(); OnChanged.ExecuteIfBound(); }
				})
			];

		if (bAllowFiles)
		{
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("ToggleFile", "Switch between a text value and a file"))
				.OnClicked_Lambda([this, Index]()
				{
					if (Rows->IsValidIndex(Index))
					{
						(*Rows)[Index].bIsFile = !(*Rows)[Index].bIsFile;
						Rebuild();
						OnChanged.ExecuteIfBound();
					}
					return FReply::Handled();
				})
				[
					SNew(STextBlock).Text(bIsFile ? LOCTEXT("FileField", "File") : LOCTEXT("TextField", "Text"))
				]
			];
		}

		Row->AddSlot().FillWidth(0.65f).Padding(0, 0, 4, 0)
		[
			SNew(SEditableTextBox)
			.Font(CourierUI::MonoFont())
			.HintText(bIsFile ? LOCTEXT("FieldFileHint", "File path") : LOCTEXT("FieldValueHint", "Value"))
			.Text_Lambda([this, Index]() { return Rows->IsValidIndex(Index) ? FText::FromString((*Rows)[Index].Value) : FText::GetEmpty(); })
			.OnTextChanged_Lambda([this, Index](const FText& Text)
			{
				if (Rows->IsValidIndex(Index)) { (*Rows)[Index].Value = Text.ToString(); OnChanged.ExecuteIfBound(); }
			})
		];

		if (bIsFile)
		{
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("Browse", "..."))
				.ToolTipText(LOCTEXT("BrowseFile", "Pick the file"))
				.OnClicked_Lambda([this, Index]()
				{
					const FString File = CourierUI::OpenFileDialog(TEXT("Pick a file"), TEXT("All files (*.*)|*.*"));
					if (!File.IsEmpty() && Rows->IsValidIndex(Index))
					{
						(*Rows)[Index].Value = File;
						OnChanged.ExecuteIfBound();
					}
					return FReply::Handled();
				})
			];
		}

		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(LOCTEXT("RemoveField", "Remove"))
			.OnClicked_Lambda([this, Index]()
			{
				if (Rows->IsValidIndex(Index))
				{
					Rows->RemoveAt(Index);
					Rebuild();
					OnChanged.ExecuteIfBound();
				}
				return FReply::Handled();
			})
			[
				SNew(SImage).Image(FAppStyle::GetBrush("Icons.X")).ColorAndOpacity(FSlateColor::UseForeground())
			]
		];

		RowBox->AddSlot().AutoHeight().Padding(0, 1)[Row];
	}
}

#undef LOCTEXT_NAMESPACE
