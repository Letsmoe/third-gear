#pragma once

#include "CoreMinimal.h"
#include "MenuController.h"
#include "MenuPages.h"
#include "Subsystems/WorldSubsystem.h"
#include "GameMenuSubsystem.generated.h"

class AGameMenuPanel;
class FGameMenuInputProcessor;
class UGameMenuWidget;

/**
 * Owns the start menu and the pause menu of a level. The start menu opens by itself when the game is launched without
 * a test switch (see GameFlow); Esc, F1, the wheel's menu button, or holding the D-pad up for a second open the pause menu,
 * which pauses the game. On the desktop the menu is a full-screen overlay; in VR it is a panel floating in front of the driver.
 */
UCLASS()
class DRIVINGGAME_API UGameMenuSubsystem : public UTickableWorldSubsystem, public IGameMenuHost
{
	GENERATED_BODY()

public:
	UGameMenuSubsystem();

	// UWorldSubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableWhenPaused() const override { return true; }

	/**
	 * Handles a key press before the game sees it: Esc and F1 open and close the menu, and while it is open the arrow keys,
	 * Enter and Backspace navigate it. Returns true if the key was used.
	 */
	bool HandleKeyDown(const FKey& Key);

	bool IsMenuOpen() const { return OpenMode != EOpenMode::Closed; }

	// IGameMenuHost
	virtual void StartDrive(const FString& Region, bool bFreeCamera) override;
	virtual void Resume() override;
	virtual void ResetCar() override;
	virtual void ReturnToMainMenu() override;
	virtual void QuitGame() override;
	virtual void ApplyPreferences() override;
	virtual bool HasCar() const override;
	virtual int32 GetEffectiveScreenPercentage() const override;
	virtual int32 GetEffectiveViewDistanceMeters() const override;

private:
	enum class EOpenMode : uint8
	{
		Closed,
		StartMenu,
		PauseMenu,
	};

	void OpenMenu(EOpenMode Mode);
	void CloseMenu();
	void OpenDebugPage(const FString& PageName);
	void ShowWidget(APlayerController& Controller);
	void HideWidget();
	void SetMenuInputMode(APlayerController& Controller, bool bMenuOpen);
	void PollWheelMenuButton(const FWheelInputState& WheelState);
	void SpinStartMenuCamera(float DeltaSeconds);
	void KeepPanelInFrontOfViewer();
	void OpenLevelWithOptions(const FString& Options);
	class AWorldStreamer* FindStreamer() const;
	class ACarPawn* FindCar() const;

	FMenuController Controller;
	TUniquePtr<FMenuPages> Pages;
	TSharedPtr<FGameMenuInputProcessor> InputProcessor;

	UPROPERTY(Transient)
	TObjectPtr<UGameMenuWidget> Widget;

	UPROPERTY(Transient)
	TObjectPtr<AGameMenuPanel> Panel;

	EOpenMode OpenMode = EOpenMode::Closed;
	bool bWorldPanel = false;
	bool bPausesGame = true;
	/** After opening with the D-pad, ignore it until it is released, so the opening press does not move the selection. */
	bool bWaitForNeutralDPad = false;
	int64 PreviousButtons = 0;
	float DPadUpHeldSeconds = 0.f;
	float SecondsSinceOpened = 0.f;
	float StartMenuYaw = 0.f;
	bool bStartMenuYawKnown = false;
};
