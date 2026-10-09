#include "GameMenuPanel.h"

#include "Components/WidgetComponent.h"
#include "GameMenuWidget.h"
#include "Materials/MaterialInterface.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
/** EV100 the level's exposure is locked to, so the panel can cancel it; the daylight value of Scripts/world_lighting.py. */
TAutoConsoleVariable<float> PanelExposureEv(TEXT("dg.MenuPanelExposureEv"), 13.f,
	TEXT("EV100 of the level's locked exposure; the VR menu panel multiplies its colour by 1.2 * 2^EV to display at normal brightness."));
/** Size of the drawn menu in pixels; matches the layout of SGameMenu. */
const FVector2D PanelPixelSize(1600.0, 900.0);
/** Centimetres of panel per pixel. 0.072 makes the panel 1.15 m wide, about 60 degrees of view at the distance below. */
constexpr double PanelCentimetresPerPixel = 0.072;
constexpr double PanelDistanceCm = 100.0;
/** Panel centre relative to the viewer's eye height, cm. */
constexpr double PanelHeightOffsetCm = -4.0;
}

AGameMenuPanel::AGameMenuPanel()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	SetTickableWhenPaused(true);

	WidgetComponent = CreateDefaultSubobject<UWidgetComponent>(TEXT("Widget"));
	SetRootComponent(WidgetComponent);
	WidgetComponent->PrimaryComponentTick.bTickEvenWhenPaused = true;
	WidgetComponent->SetTickWhenOffscreen(true);
	WidgetComponent->SetWidgetSpace(EWidgetSpace::World);
	WidgetComponent->SetDrawSize(PanelPixelSize);
	WidgetComponent->SetBlendMode(EWidgetBlendMode::Transparent);
	WidgetComponent->SetTwoSided(true);
	WidgetComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WidgetComponent->SetCastShadow(false);
	WidgetComponent->SetRelativeScale3D(FVector(1.0, PanelCentimetresPerPixel, PanelCentimetresPerPixel));
}

void AGameMenuPanel::Setup(UGameMenuWidget* Widget, UMaterialInterface* PanelMaterial)
{
	if (PanelMaterial)
	{
		WidgetComponent->SetMaterial(0, PanelMaterial);
	}
	WidgetComponent->SetWidget(Widget);
}

void AGameMenuPanel::PlaceInFrontOf(const FVector& ViewerLocation, float ViewerYaw)
{
	const FRotator Heading(0.f, ViewerYaw, 0.f);
	const FVector Location = ViewerLocation + Heading.Vector() * PanelDistanceCm + FVector(0.0, 0.0, PanelHeightOffsetCm);
	// The widget's front faces along -X, so turn the panel around to look back at the viewer.
	SetActorLocationAndRotation(Location, FRotator(0.f, ViewerYaw + 180.f, 0.f));
}

void AGameMenuPanel::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// The menu material has to undo the scene's locked exposure to show at normal brightness.
	UMaterialInstanceDynamic* PanelMaterial = WidgetComponent->GetMaterialInstance();
	if (PanelMaterial)
	{
		const float ExposureScale = 1.2f * FMath::Pow(2.f, PanelExposureEv.GetValueOnGameThread());
		PanelMaterial->SetScalarParameterValue(TEXT("Gain"), ExposureScale);
	}
}
