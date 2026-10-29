#pragma once

#include "CoreMinimal.h"
#include "CourierTypes.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;

namespace CourierUI
{
	FSlateColor VerbColor(ECourierVerb Verb);
	FSlateColor StatusColor(int32 StatusCode);
	/** "GET", "WS" or "SSE". */
	FString BadgeText(const FCourierRequestSpec& Spec);
	FSlateColor BadgeColor(const FCourierRequestSpec& Spec);
	FSlateFontInfo MonoFont(int32 Size = 9);
	FString FormatBytes(int64 Bytes);

	/** A small modal text prompt. Unset when cancelled. */
	TOptional<FString> PromptForText(const FText& Title, const FText& Label, const FString& Default);
	bool Confirm(const FText& Message);
	void Notify(const FText& Message, bool bSuccess = true);

	FString OpenFileDialog(const FString& Title, const FString& Filter);
	FString SaveFileDialog(const FString& Title, const FString& DefaultFile, const FString& Filter);
}

/** Editable rows of key / value with an enable box, like Postman's Params and Headers tables. */
class SCourierKeyValueList : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCourierKeyValueList) {}
		SLATE_ARGUMENT(TArray<FCourierKeyValue>*, Rows)
		SLATE_ARGUMENT(FText, KeyHint)
		SLATE_ARGUMENT(FText, ValueHint)
		SLATE_EVENT(FSimpleDelegate, OnChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void Rebuild();

private:
	TArray<FCourierKeyValue>* Rows = nullptr;
	FText KeyHint;
	FText ValueHint;
	FSimpleDelegate OnChanged;
	TSharedPtr<SVerticalBox> RowBox;
};

/** Form body rows: key, text or file value, enable box (and the file toggle for multipart). */
class SCourierFormFieldList : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCourierFormFieldList) : _AllowFiles(false) {}
		SLATE_ARGUMENT(TArray<FCourierFormField>*, Rows)
		SLATE_ARGUMENT(bool, AllowFiles)
		SLATE_EVENT(FSimpleDelegate, OnChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void Rebuild();

private:
	TArray<FCourierFormField>* Rows = nullptr;
	bool bAllowFiles = false;
	FSimpleDelegate OnChanged;
	TSharedPtr<SVerticalBox> RowBox;
};
