#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"

#include "ColonySim/Entity.h"

#include "ProtagonistPawn.generated.h"

class UCameraComponent;
class USimSubsystem;

/**
 * What the player drives in direct mode: the camera body, in first or third person, and the
 * position of the colonist it controls. The colonist itself is drawn by USimPawnSubsystem like
 * every pawn, from the sim; this pawn only holds a local copy of its position (in sim metres),
 * moves it each frame against the sim's walls, and UProtagonistSubsystem sends it to the sim once
 * per step. The sim has the last word: every step the local copy is pulled back to the sim's if it
 * drifted (docs/design/protagonist.md).
 *
 * Keys: W A S D walk (Ctrl slower), the mouse looks, the wheel zooms in third person, V toggles
 * first and third person, T takes over the colonist under the crosshair, Tab goes back to the
 * overseer view. The methods below are what the keys call, so tests can drive it without a keyboard.
 *
 * Stairs and ladders (U17, docs/design/levels-draw.md): walking into the stairs from the cell at their foot, or back
 * towards them from the cell at their head, asks the sim to climb (ClimbCommand). The sim moves the colonist up or
 * down; the body stands still meanwhile and takes the sim's place and level when the climb is over.
 */
UCLASS()
class RIM3D_API AProtagonistPawn : public APawn
{
	GENERATED_BODY()

public:
	AProtagonistPawn();

	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	/** Starts controlling a colonist standing at `SimPosition` (sim metres). `CruiseSpeed` is how
	 *  fast it runs on flat ground, metres per second at 1x. The view looks along `InYaw`. */
	void Begin(colony::Entity InThing, const FVector2D& SimPosition, float InCruiseSpeed, float InYaw, int32 InLevel = 0);

	/** Walks the body. Right and Forward are -1 to 1 relative to where the camera looks. Moves nothing
	 *  while the sim is paused. */
	void Move(double Right, double Forward, bool bWalk, float DeltaTime);
	/** Turns the view by degrees; positive pitch looks up. */
	void Look(float YawDegrees, float PitchDegrees);
	/** Zooms the third person camera by wheel notches (positive: closer). */
	void Zoom(float Notches);
	void SetFirstPerson(bool bFirst);
	bool IsFirstPerson() const { return bFirstPerson; }
	void ToggleView() { SetFirstPerson(!bFirstPerson); }
	/** Puts the camera where it is heading right now (after switching bodies or views). */
	void SnapCamera();

	/** Test input: used instead of the keyboard until ClearScriptedInput. */
	void SetScriptedInput(double Right, double Forward, bool bWalk);
	void ClearScriptedInput() { bScripted = false; }

	/** Where the body wants to be, sim metres: what is sent to the sim each step. */
	const FVector2D& GetSimPosition() const { return Position; }
	/** Pulls the local position to the sim's if they are more than a metre apart. Returns true if it did. */
	bool Reconcile(const FVector2D& SimPosition);
	/** Sets the local position without checks (a new body, a loaded game). */
	void SetSimPosition(const FVector2D& SimPosition);
	/** The level the body is on. */
	int32 GetSimLevel() const { return Level; }
	/** Follows the sim's colonist while it climbs (the body stands still) and takes its place and level when it gets
	 *  to another level. Called every frame with the newest snapshot's colonist. */
	void FollowSim(const FVector2D& SimPosition, int32 SimLevel, bool bSimClimbing);
	bool IsClimbing() const { return bClimbing; }
	/** True once, with the direction pushed, when the body walked into stairs or a ladder: the subsystem sends the climb. */
	bool TakeClimbRequest(FVector2D& OutDirection);

	colony::Entity GetThing() const { return Thing; }
	float GetYaw() const { return Yaw; }
	float GetPitch() const { return Pitch; }
	float GetArmLength() const { return ArmLength; }
	const FVector2D& GetLocalVelocity() const { return Velocity; }
	/** Where the camera looks from and along (world units), for aiming. */
	FVector GetViewLocation() const;
	FVector GetViewDirection() const;

	static constexpr float kEyeHeight = 165.0f;
	static constexpr float kMinArm = 150.0f;
	static constexpr float kMaxArm = 800.0f;
	/** Slow walking pace, metres per second. */
	static constexpr float kWalkSpeed = 1.5f;

private:
	void OnSwitchModePressed();
	void OnToggleViewPressed() { ToggleView(); }
	void OnTakeOverPressed();
	void OnZoomIn() { Zoom(1.0f); }
	void OnZoomOut() { Zoom(-1.0f); }

	USimSubsystem* GetBridge() const;
	APlayerController* GetPlayerController() const;
	void UpdateCamera();

	UPROPERTY(VisibleAnywhere, Category = "Protagonist")
	TObjectPtr<UCameraComponent> Camera;

	colony::Entity Thing = colony::kNoEntity;
	FVector2D Position = FVector2D::ZeroVector;  // sim metres
	int32 Level = 0;
	bool bClimbing = false;
	bool bClimbWanted = false;
	FVector2D ClimbDirection = FVector2D::ZeroVector;
	/** Real seconds before another climb may be asked for (the sim has one on its way). */
	float ClimbWait = 0.0f;
	FVector2D Velocity = FVector2D::ZeroVector;  // sim metres per second at 1x
	float CruiseSpeed = 4.6f;
	float Yaw = 0.0f;
	float Pitch = -10.0f;
	float ArmLength = 350.0f;
	bool bFirstPerson = false;
	bool bScripted = false;
	double ScriptRight = 0.0;
	double ScriptForward = 0.0;
	bool bScriptWalk = false;
};
