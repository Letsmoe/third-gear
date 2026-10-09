#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "GameMenuWidget.generated.h"

class FMenuController;

/** Hosts the Slate menu (SGameMenu) so it can be added to the viewport or shown on a world-space widget component. */
UCLASS()
class DRIVINGGAME_API UGameMenuWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Sets what to draw. Call before the widget is first shown. The controller must outlive the widget. */
	void Setup(FMenuController* InController, bool bInWorldPanel);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	FMenuController* Controller = nullptr;
	bool bWorldPanel = false;
};
