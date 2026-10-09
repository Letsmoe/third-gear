#include "GameMenuWidget.h"

#include "MenuController.h"
#include "SGameMenu.h"

void UGameMenuWidget::Setup(FMenuController* InController, bool bInWorldPanel)
{
	Controller = InController;
	bWorldPanel = bInWorldPanel;
}

TSharedRef<SWidget> UGameMenuWidget::RebuildWidget()
{
	return SNew(SGameMenu).Controller(Controller).bWorldPanel(bWorldPanel);
}
