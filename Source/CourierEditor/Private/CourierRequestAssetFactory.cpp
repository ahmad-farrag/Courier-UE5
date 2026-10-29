#include "CourierRequestAssetFactory.h"

#include "CourierEditorModule.h"
#include "CourierRequestAsset.h"

#define LOCTEXT_NAMESPACE "Courier"

UCourierRequestAssetFactory::UCourierRequestAssetFactory()
{
	SupportedClass = UCourierRequestAsset::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* UCourierRequestAssetFactory::FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	UCourierRequestAsset* Asset = NewObject<UCourierRequestAsset>(InParent, InClass, InName, Flags);
	Asset->Request.Name = InName.ToString();
	Asset->Request.Id = FGuid::NewGuid();
	return Asset;
}

FText UAssetDefinition_CourierRequest::GetAssetDisplayName() const
{
	return LOCTEXT("AssetName", "Courier Request");
}

FLinearColor UAssetDefinition_CourierRequest::GetAssetColor() const
{
	return FLinearColor(1.f, 0.45f, 0.15f);
}

TSoftClassPtr<UObject> UAssetDefinition_CourierRequest::GetAssetClass() const
{
	return UCourierRequestAsset::StaticClass();
}

TConstArrayView<FAssetCategoryPath> UAssetDefinition_CourierRequest::GetAssetCategories() const
{
	static const FAssetCategoryPath Categories[] = { EAssetCategoryPaths::Misc };
	return Categories;
}

EAssetCommandResult UAssetDefinition_CourierRequest::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	for (UCourierRequestAsset* Asset : OpenArgs.LoadObjects<UCourierRequestAsset>())
	{
		FCourierEditorModule::Get().OpenRequestAsset(Asset);
	}
	return EAssetCommandResult::Handled;
}

#undef LOCTEXT_NAMESPACE
