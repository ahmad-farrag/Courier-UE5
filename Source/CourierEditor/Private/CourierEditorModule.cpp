#include "CourierEditorModule.h"

#include "Framework/Docking/TabManager.h"
#include "SCourierWindow.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "Courier"

const FName FCourierEditorModule::TabName(TEXT("CourierClient"));

static FAutoConsoleCommand GCourierOpenCommand(
	TEXT("Courier.Open"),
	TEXT("Opens the Courier API client tab."),
	FConsoleCommandDelegate::CreateLambda([]() { FCourierEditorModule::Get().OpenWindow(); }));

static FAutoConsoleCommand GCourierSendCommand(
	TEXT("Courier.Send"),
	TEXT("Sends (or connects) the request open in the Courier tab, like Ctrl+Enter."),
	FConsoleCommandDelegate::CreateLambda([]() { FCourierEditorModule::Get().SendOpenRequest(); }));

FCourierEditorModule& FCourierEditorModule::Get()
{
	return FModuleManager::LoadModuleChecked<FCourierEditorModule>(TEXT("CourierEditor"));
}

void FCourierEditorModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateLambda([this](const FSpawnTabArgs&)
	{
		TSharedRef<SCourierWindow> NewWindow = SNew(SCourierWindow);
		Window = NewWindow;
		return SNew(SDockTab).TabRole(ETabRole::NomadTab)[NewWindow];
	}))
	.SetDisplayName(LOCTEXT("TabTitle", "Courier"))
	.SetTooltipText(LOCTEXT("TabTooltip", "API client: HTTP, GraphQL, WebSocket and Server-Sent Events, with collections, environments and Postman import"))
	.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Web"))
	.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());
	// A floating nomad tab otherwise opens small; the client needs room for its three panes.
	FTabManager::RegisterDefaultTabWindowSize(TabName, FVector2D(1400.f, 900.f));

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FCourierEditorModule::RegisterMenus));
}

void FCourierEditorModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
		FTabManager::UnregisterDefaultTabWindowSize(TabName);
	}
}

void FCourierEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
	FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("Courier"), LOCTEXT("ToolsSection", "Courier"));
	Section.AddMenuEntry(
		TEXT("OpenCourier"),
		LOCTEXT("OpenCourier", "Courier API Client"),
		LOCTEXT("OpenCourierTip", "Send HTTP, GraphQL, WebSocket and Server-Sent Events requests from the editor"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Web"),
		FUIAction(FExecuteAction::CreateRaw(this, &FCourierEditorModule::OpenWindow)));
}

void FCourierEditorModule::OpenWindow()
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabName);
}

void FCourierEditorModule::OpenRequestAsset(UCourierRequestAsset* Asset)
{
	OpenWindow();
	if (const TSharedPtr<SCourierWindow> Pinned = Window.Pin())
	{
		Pinned->OpenRequestAsset(Asset);
	}
}

void FCourierEditorModule::SendOpenRequest()
{
	if (const TSharedPtr<SCourierWindow> Pinned = Window.Pin())
	{
		Pinned->SendOrConnect();
	}
}

IMPLEMENT_MODULE(FCourierEditorModule, CourierEditor)

#undef LOCTEXT_NAMESPACE
