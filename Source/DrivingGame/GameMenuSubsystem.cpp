#include "GameMenuSubsystem.h"

#include "Blueprint/UserWidget.h"
#include "CarPawn.h"
#include "CarSettings.h"
#include "DrivingPreferences.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFlow.h"
#include "GameFramework/PlayerController.h"
#include "GameMenuPanel.h"
#include "GameMenuWidget.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Camera/PlayerCameraManager.h"
#include "WheelInputSettings.h"
#include "WheelInputSubsystem.h"
#include "WorldStreamer.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameMenu, Log, All);

namespace
{
/** Holding the D-pad up this long opens the pause menu on a wheel without an assigned menu button. */
constexpr float DPadHoldToOpenSeconds = 1.0f;
/** Degrees per second the start menu's camera turns. */
constexpr float StartMenuTurnRate = 4.f;
/** The panel follows the head for this long after opening, while the headset settles. */
constexpr float PanelSettleSeconds = 1.5f;
constexpr int32 ViewportZOrder = 100;

/** A made-up wheel state for screenshots of the live readout (-MenuDemoWheel), since no wheel is plugged in on the build machine. */
FWheelInputState MakeDemoWheelState()
{
	FWheelInputState State;
	State.bConnected = true;
	State.Steering = 0.24f;
	State.SteeringDegrees = 108.f;
	State.Throttle = 0.55f;
	State.Brake = 0.f;
	State.Clutch = 0.12f;
	State.ShifterGear = 3;
	State.Buttons = (1ll << 3) | (1ll << 14);
	State.DPadY = 0;
	State.AxisValues.Init(-1.f, 64);
	State.AxisValues[0x00] = 0.62f;
	State.AxisValues[0x01] = 0.88f;
	State.AxisValues[0x02] = 0.45f;
	State.AxisValues[0x05] = 1.f;
	State.AxisValues[0x10] = 0.5f;
	State.AxisValues[0x11] = 0.5f;
	return State;
}

FWheelInputState ReadWheelState()
{
	if (FParse::Param(FCommandLine::Get(), TEXT("MenuDemoWheel")))
	{
		return MakeDemoWheelState();
	}
	const UWheelInputSubsystem* Wheel = GEngine ? GEngine->GetEngineSubsystem<UWheelInputSubsystem>() : nullptr;
	return Wheel ? Wheel->GetState() : FWheelInputState();
}

bool WasMenuButtonPressed(const FWheelInputState& State, int64 PreviousButtons)
{
	const int32 Index = GetDefault<UWheelInputSettings>()->MenuButtonIndex;
	if (Index < 0 || Index >= 64)
	{
		return false;
	}
	return State.IsButtonDown(Index) && !((PreviousButtons >> Index) & 1);
}
}

/** Gives the subsystem the key presses before the viewport and the game see them. */
class FGameMenuInputProcessor : public IInputProcessor
{
public:
	explicit FGameMenuInputProcessor(UGameMenuSubsystem* InOwner)
		: Owner(InOwner)
	{
	}

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
	{
		return Owner.IsValid() && Owner->HandleKeyDown(InKeyEvent.GetKey());
	}

	virtual const TCHAR* GetDebugName() const override { return TEXT("GameMenu"); }

private:
	TWeakObjectPtr<UGameMenuSubsystem> Owner;
};

UGameMenuSubsystem::UGameMenuSubsystem() = default;

bool UGameMenuSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	const bool bIsPlayableWorld = World && World->IsGameWorld() && World->GetNetMode() != NM_DedicatedServer;
	return bIsPlayableWorld && !IsRunningCommandlet() && Super::ShouldCreateSubsystem(Outer);
}

TStatId UGameMenuSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGameMenuSubsystem, STATGROUP_Tickables);
}

void UGameMenuSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	Pages = MakeUnique<FMenuPages>(Controller, *this);
	Controller.OnCloseRequested.BindLambda([this]()
	{
		if (OpenMode == EOpenMode::PauseMenu)
		{
			Resume();
		}
	});
	bWorldPanel = UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled() || FParse::Param(FCommandLine::Get(), TEXT("MenuPanel3D"));
	ApplyPreferences();

	if (FSlateApplication::IsInitialized())
	{
		InputProcessor = MakeShared<FGameMenuInputProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
	}

	const FString DebugPage = GameFlow::GetDebugMenuPage();
	if (GameFlow::IsStartMenuLevel(&InWorld))
	{
		OpenMenu(EOpenMode::StartMenu);
	}
	else if (!DebugPage.IsEmpty())
	{
		OpenDebugPage(DebugPage);
	}
}

void UGameMenuSubsystem::Deinitialize()
{
	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
	HideWidget();
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------------------------------------------------
// Opening and closing
// ---------------------------------------------------------------------------------------------------------------------

void UGameMenuSubsystem::OpenMenu(EOpenMode Mode)
{
	APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	if (!PlayerController || Mode == EOpenMode::Closed)
	{
		return;
	}
	OpenMode = Mode;
	UE_LOG(LogGameMenu, Log, TEXT("Opened the %s on the %s"), Mode == EOpenMode::StartMenu ? TEXT("start menu") : TEXT("pause menu"), bWorldPanel ? TEXT("VR panel") : TEXT("screen"));
	bPausesGame = Mode == EOpenMode::PauseMenu && GameFlow::GetDebugMenuPage().IsEmpty();
	SecondsSinceOpened = 0.f;
	Controller.SetRootPage(Mode == EOpenMode::StartMenu ? Pages->BuildMainMenu() : Pages->BuildPauseMenu());
	Controller.ResetTransientState();
	ShowWidget(*PlayerController);
	SetMenuInputMode(*PlayerController, true);
	if (bPausesGame)
	{
		UGameplayStatics::SetGamePaused(GetWorld(), true);
	}
}

void UGameMenuSubsystem::OpenDebugPage(const FString& PageName)
{
	OpenMenu(EOpenMode::PauseMenu);
	if (PageName == TEXT("Pause") || OpenMode == EOpenMode::Closed)
	{
		return;
	}
	Controller.PushPage(Pages->BuildSettingsPage());
	if (PageName == TEXT("Wheel"))
	{
		Controller.PushPage(Pages->BuildWheelPage());
	}
	else if (PageName == TEXT("Graphics"))
	{
		Controller.PushPage(Pages->BuildGraphicsPage());
	}
	else if (PageName == TEXT("Audio"))
	{
		Controller.PushPage(Pages->BuildAudioPage());
	}
}

void UGameMenuSubsystem::CloseMenu()
{
	APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	Pages->SaveAll();
	HideWidget();
	OpenMode = EOpenMode::Closed;
	if (PlayerController)
	{
		SetMenuInputMode(*PlayerController, false);
	}
	if (bPausesGame)
	{
		UGameplayStatics::SetGamePaused(GetWorld(), false);
	}
}

void UGameMenuSubsystem::ShowWidget(APlayerController& PlayerController)
{
	HideWidget();
	Widget = CreateWidget<UGameMenuWidget>(&PlayerController, UGameMenuWidget::StaticClass());
	Widget->Setup(&Controller, bWorldPanel);
	if (!bWorldPanel)
	{
		Widget->AddToViewport(ViewportZOrder);
		return;
	}
	Panel = GetWorld()->SpawnActor<AGameMenuPanel>();
	UMaterialInterface* PanelMaterial = GetDefault<UCarSettings>()->MenuPanelMaterial.LoadSynchronous();
	Panel->Setup(Widget, PanelMaterial);
	KeepPanelInFrontOfViewer();
}

void UGameMenuSubsystem::HideWidget()
{
	if (Widget)
	{
		Widget->RemoveFromParent();
		Widget = nullptr;
	}
	if (Panel)
	{
		Panel->Destroy();
		Panel = nullptr;
	}
}

void UGameMenuSubsystem::SetMenuInputMode(APlayerController& PlayerController, bool bMenuOpen)
{
	if (bWorldPanel)
	{
		return; // in VR the wheel navigates; the desktop mirror needs no cursor
	}
	PlayerController.SetShowMouseCursor(bMenuOpen);
	if (bMenuOpen)
	{
		FInputModeUIOnly InputMode;
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PlayerController.SetInputMode(InputMode);
		return;
	}
	PlayerController.SetInputMode(FInputModeGameOnly());
}

// ---------------------------------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------------------------------

bool UGameMenuSubsystem::HandleKeyDown(const FKey& Key)
{
	const bool bIsMenuKey = Key == EKeys::Escape || Key == EKeys::F1;
	if (!IsMenuOpen())
	{
		if (bIsMenuKey && GetWorld() && GetWorld()->GetFirstPlayerController())
		{
			OpenMenu(EOpenMode::PauseMenu);
			return true;
		}
		return false;
	}
	if (Key == EKeys::Tilde)
	{
		return false; // keep the console available
	}
	if (Key == EKeys::Up)
	{
		Controller.HandleInput(EMenuInput::Up);
	}
	else if (Key == EKeys::Down)
	{
		Controller.HandleInput(EMenuInput::Down);
	}
	else if (Key == EKeys::Left)
	{
		Controller.HandleInput(EMenuInput::Left);
	}
	else if (Key == EKeys::Right)
	{
		Controller.HandleInput(EMenuInput::Right);
	}
	else if (Key == EKeys::Enter || Key == EKeys::SpaceBar)
	{
		Controller.HandleInput(EMenuInput::Confirm);
	}
	else if (bIsMenuKey || Key == EKeys::BackSpace)
	{
		Controller.HandleInput(EMenuInput::Back);
	}
	return true; // the menu is modal: the game gets no keys while it is open
}

void UGameMenuSubsystem::PollWheelMenuButton(const FWheelInputState& WheelState)
{
	const bool bPressed = WasMenuButtonPressed(WheelState, PreviousButtons);
	if (IsMenuOpen() && bPressed && OpenMode == EOpenMode::PauseMenu)
	{
		Resume();
		return;
	}
	if (IsMenuOpen())
	{
		return;
	}
	DPadUpHeldSeconds = WheelState.DPadY < 0 ? DPadUpHeldSeconds + FApp::GetDeltaTime() : 0.f;
	if (bPressed || DPadUpHeldSeconds >= DPadHoldToOpenSeconds)
	{
		bWaitForNeutralDPad = DPadUpHeldSeconds > 0.f;
		DPadUpHeldSeconds = 0.f;
		OpenMenu(EOpenMode::PauseMenu);
	}
}

void UGameMenuSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	const float RealDeltaSeconds = FApp::GetDeltaTime();
	FWheelInputState WheelState = ReadWheelState();
	PollWheelMenuButton(WheelState);
	PreviousButtons = WheelState.Buttons;
	if (!IsMenuOpen())
	{
		return;
	}

	SecondsSinceOpened += RealDeltaSeconds;
	if (bWaitForNeutralDPad)
	{
		bWaitForNeutralDPad = WheelState.DPadX != 0 || WheelState.DPadY != 0;
		WheelState.DPadX = 0;
		WheelState.DPadY = 0;
	}
	Controller.Update(RealDeltaSeconds, WheelState);
	if (OpenMode == EOpenMode::StartMenu)
	{
		SpinStartMenuCamera(RealDeltaSeconds);
	}
	if (bWorldPanel && SecondsSinceOpened < PanelSettleSeconds)
	{
		KeepPanelInFrontOfViewer();
	}
}

void UGameMenuSubsystem::SpinStartMenuCamera(float DeltaSeconds)
{
	APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	if (!PlayerController || !GameFlow::GetDebugMenuPage().IsEmpty())
	{
		return;
	}
	if (!bStartMenuYawKnown)
	{
		StartMenuYaw = PlayerController->GetControlRotation().Yaw; // begin where the start pose looks
		bStartMenuYawKnown = true;
	}
	StartMenuYaw += StartMenuTurnRate * DeltaSeconds;
	PlayerController->SetControlRotation(FRotator(-4.f, StartMenuYaw, 0.f));
}

void UGameMenuSubsystem::KeepPanelInFrontOfViewer()
{
	APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	if (!Panel || !PlayerController || !PlayerController->PlayerCameraManager)
	{
		return;
	}
	Panel->PlaceInFrontOf(PlayerController->PlayerCameraManager->GetCameraLocation(), PlayerController->PlayerCameraManager->GetCameraRotation().Yaw);
}

// ---------------------------------------------------------------------------------------------------------------------
// Host actions
// ---------------------------------------------------------------------------------------------------------------------

AWorldStreamer* UGameMenuSubsystem::FindStreamer() const
{
	TActorIterator<AWorldStreamer> It(GetWorld());
	return It ? *It : nullptr;
}

ACarPawn* UGameMenuSubsystem::FindCar() const
{
	const APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	return PlayerController ? Cast<ACarPawn>(PlayerController->GetPawn()) : nullptr;
}

bool UGameMenuSubsystem::HasCar() const
{
	return FindCar() != nullptr;
}

void UGameMenuSubsystem::OpenLevelWithOptions(const FString& Options)
{
	Pages->SaveAll();
	UGameplayStatics::OpenLevel(GetWorld(), FName(GameFlow::StreamedMapPath), /*bAbsolute=*/true, Options);
}

void UGameMenuSubsystem::StartDrive(const FString& Region, bool bFreeCamera)
{
	OpenLevelWithOptions(FString::Printf(TEXT("Region=%s?%s=1"), *Region, bFreeCamera ? TEXT("FreeCam") : TEXT("Drive")));
}

void UGameMenuSubsystem::ReturnToMainMenu()
{
	OpenLevelWithOptions(TEXT("Menu=1"));
}

void UGameMenuSubsystem::Resume()
{
	CloseMenu();
}

void UGameMenuSubsystem::ResetCar()
{
	CloseMenu();
	if (ACarPawn* Car = FindCar())
	{
		Car->ResetCarUpright();
	}
}

void UGameMenuSubsystem::QuitGame()
{
	Pages->SaveAll();
	UKismetSystemLibrary::QuitGame(GetWorld(), GetWorld()->GetFirstPlayerController(), EQuitPreference::Quit, false);
}

void UGameMenuSubsystem::ApplyPreferences()
{
	const UDrivingPreferences* Preferences = GetDefault<UDrivingPreferences>();
	FApp::SetVolumeMultiplier(FMath::Clamp(Preferences->MasterVolume, 0.f, 1.f));

	if (Preferences->ScreenPercentage > 0)
	{
		if (IConsoleVariable* ScreenPercentage = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage")))
		{
			ScreenPercentage->Set(static_cast<float>(Preferences->ScreenPercentage), ECVF_SetByGameSetting);
		}
	}

	AWorldStreamer* Streamer = FindStreamer();
	if (Streamer && Preferences->ViewDistanceMeters > 0)
	{
		// Keep the proportions of the streamer's own defaults between the detail rings.
		const AWorldStreamer* Defaults = GetDefault<AWorldStreamer>();
		const float FarCm = Preferences->ViewDistanceMeters * 100.f;
		Streamer->FarDistance = FarCm;
		Streamer->MiddleDistance = FarCm * Defaults->MiddleDistance / Defaults->FarDistance;
		Streamer->NearDistance = FarCm * Defaults->NearDistance / Defaults->FarDistance;
	}
	UDrivingPreferences::OnChanged().Broadcast();
}

int32 UGameMenuSubsystem::GetEffectiveScreenPercentage() const
{
	const int32 Chosen = GetDefault<UDrivingPreferences>()->ScreenPercentage;
	if (Chosen > 0)
	{
		return Chosen;
	}
	const IConsoleVariable* ScreenPercentage = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
	const int32 LaunchValue = ScreenPercentage ? FMath::RoundToInt(ScreenPercentage->GetFloat()) : 0;
	return LaunchValue > 0 ? LaunchValue : 100; // the engine's own default leaves the variable unset
}

int32 UGameMenuSubsystem::GetEffectiveViewDistanceMeters() const
{
	const int32 Chosen = GetDefault<UDrivingPreferences>()->ViewDistanceMeters;
	if (Chosen > 0)
	{
		return Chosen;
	}
	const AWorldStreamer* Streamer = FindStreamer();
	return FMath::RoundToInt((Streamer ? Streamer->FarDistance : GetDefault<AWorldStreamer>()->FarDistance) / 100.f);
}
