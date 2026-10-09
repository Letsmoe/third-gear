#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameMenuPanel.generated.h"

class UGameMenuWidget;
class UWidgetComponent;

/** A flat panel floating in the world that shows the menu in VR. It keeps drawing while the game is paused. */
UCLASS()
class DRIVINGGAME_API AGameMenuPanel : public AActor
{
	GENERATED_BODY()

public:
	AGameMenuPanel();

	/** Shows the widget on the panel. The material draws it independent of the scene's exposure. */
	void Setup(UGameMenuWidget* Widget, UMaterialInterface* PanelMaterial);

	/** Places the panel in front of a viewer so that it faces them, centred on the horizontal view direction. */
	void PlaceInFrontOf(const FVector& ViewerLocation, float ViewerYaw);

	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UWidgetComponent> WidgetComponent;
};
