#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class SCourierWindow;
class UCourierRequestAsset;

class COURIEREDITOR_API FCourierEditorModule : public IModuleInterface
{
public:
	static const FName TabName;

	static FCourierEditorModule& Get();

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Opens (or focuses) the Courier tab. */
	void OpenWindow();
	/** Opens the tab with the asset's request loaded. */
	void OpenRequestAsset(UCourierRequestAsset* Asset);
	/** Sends (or connects) the request open in the tab, if the tab is open. */
	void SendOpenRequest();

private:
	void RegisterMenus();

	TWeakPtr<SCourierWindow> Window;
	FDelegateHandle CookiesChangedHandle;
};
