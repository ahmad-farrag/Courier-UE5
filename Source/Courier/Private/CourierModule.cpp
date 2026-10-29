#include "CourierRequestAsset.h"
#include "CourierHttp.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogCourier);

TArray<FString> UCourierRequestAsset::GetVariableNames() const
{
	return FCourierHttp::FindVariableNames(Request);
}

IMPLEMENT_MODULE(FDefaultModuleImpl, Courier)
