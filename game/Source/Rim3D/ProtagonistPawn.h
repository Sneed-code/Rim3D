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
 * Keys (defaults; all rebindable, KeyBindings.h and docs/design/controls.md): W A S D walk (Ctrl slower, Shift
 * toggles sprint), the mouse looks, the wheel zooms in third person, V toggles first and third person, Y takes over the
 * colonist under the crosshair, O goes back to the overseer view. The methods below are what the keys call, so tests can drive it without a keyboard.
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

	/** Mouse speed in direct control (Settings window): the Protagonist.MouseTurn value, saved in GameUserSettings.ini. */
	static float GetMouseTurn();
	static void SetMouseTurn(float Value);
	static constexpr float kDefaultMouseTurn = 1.2f;

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
	/** Third person only (sneed 2026-11-22: "lets disable it its a 3rd person game"): first person is gone, so this
	 *  always leaves the camera behind the body. Kept so older callers still build. The one view from the eyes is a
	 *  scoped gun fully aimed (IsScopeView), which shows only the scope. */
	void SetFirstPerson(bool bFirst);
	bool IsFirstPerson() const { return false; }
	/** Holding aim with a scoped gun (the bolt-action rifle), fully aimed: the camera is at the eyes, the body hidden and
	 *  the view the scope's (UGunRangeSubsystem::GetScopeFov). */
	bool IsScopeView() const { return bScopeView; }
	/** For smoke tests' outside cameras: false keeps the over-the-shoulder aim even with a scope. */
	void SetScopeAllowed(bool bAllowed) { bScopeAllowed = bAllowed; }
	/** Puts the camera where it is heading right now (after switching bodies or views), with no soft follow. */
	void SnapCamera();

	/** Holding aim (right mouse button with a gun in hand): the view blends to the aimed one over about a fifth of a second
	 *  (over the shoulder and zoomed; a scoped gun goes to its scope once fully aimed) and back when it is let go (docs/design/shooter-feel-draw.md).
	 *  Test input: `SetScriptedAim` is used instead of the mouse button until ClearScriptedAim. */
	void SetScriptedAim(bool bHeld) { bScriptedAim = true; bScriptAimHeld = bHeld; }
	void ClearScriptedAim() { bScriptedAim = false; }
	bool IsAimHeld() const { return bAimHeld; }
	/** Locked on to an enemy (docs/design/focus.md): the view turns to keep it ahead; in third person the mouse only tilts. */
	bool IsFocusLocked() const { return bFocusLocked; }
	/** 0 not aiming to 1 fully aimed (eased). */
	float GetAimBlend() const { return AimBlend; }
	/** Centimetres the third person camera's pivot trails the drawn body on the ground (its soft follow). */
	double GetCameraTrail() const { return CameraTrail; }
	/** The third person distance now, cm: the zoomed one, closer while aiming. Shots start this far along the view. */
	float GetEffectiveArmLength() const { return EffectiveArm; }
	/** Whether the body is hidden because the view is at its eyes: only while looking through a scope. */
	bool IsBodyHiddenInView() const;
	/** The field of view now (80 degrees, narrowing while aimed). */
	float GetFieldOfView() const;
	/** What the camera is nudged by now (kick, shake, roll dip) on top of the aim, and its motion blur. */
	FRotator GetFeelRotation() const;
	float GetMotionBlurAmount() const;

	/** Test input: used instead of the keyboard until ClearScriptedInput. */
	void SetScriptedInput(double Right, double Forward, bool bWalk, bool bSprint = false);
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
	/** Where the camera looks from and along (world units), for aiming: the aim itself, without the gun's kick, the shake or a
	 *  roll's dip, so the crosshair is where the bullet goes. */
	FVector GetViewLocation() const;
	FVector GetViewDirection() const;
	/** The view camera (the gun range hangs its first-person arms on it). */
	UCameraComponent* GetCamera() const { return Camera; }

	static constexpr float kEyeHeight = 165.0f;
	static constexpr float kMinArm = 150.0f;
	static constexpr float kMaxArm = 800.0f;
	/** Slow walking pace, metres per second. */
	static constexpr float kWalkSpeed = 1.5f;
	/** Sprinting (the sprint toggle on): this much faster than the colonist's cruise speed, and at least kMinSprintSpeed m/s. */
	static constexpr float kSprintFactor = 1.5f;
	static constexpr float kMinSprintSpeed = 6.5f;
	bool IsSprinting() const { return bSprinting; }
	/** A gun in hand (the sim's weapon has verbs): lock-on keeps the target but not the facing or the view (lock-on.md). */
	bool HasGunDrawn() const;
	/** The sprint key is a toggle (sneed 2026-11-17): on until pressed again, kept across colonists you take over. */
	static bool IsSprintToggleOn();
	static void SetSprintToggle(bool bOn);
	/** The speed the keys ask for now (walk, run or sprint, slowed by the ground), metres per second; 0 with none held. */
	float GetTargetSpeed() const { return TargetSpeed; }
	/** Space: the body jumps (an arc and the jump animations on the pawn actor; the sim keeps it on the ground). */
	bool Jump();

private:
	void OnSwitchModePressed();
	void OnTakeOverPressed();
	void OnZoomIn() { Zoom(1.0f); }
	void OnZoomOut() { Zoom(-1.0f); }
	void OnJumpPressed() { Jump(); }

	USimSubsystem* GetBridge() const;
	APlayerController* GetPlayerController() const;
	void UpdateCamera();

	UPROPERTY(VisibleAnywhere, Category = "Protagonist")
	TObjectPtr<UCameraComponent> Camera;

	colony::Entity Thing = colony::kNoEntity;
	bool bSprinting = false;
	float TargetSpeed = 0.0f;
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
	bool bFirstPerson = false;  // always false now (third person only); the old first-person branches stay unused
	bool bScopeView = false;
	bool bScopeAllowed = true;
	/** The aim camera's shoulder (right) and lift offsets now, easing to the held gun's (cm; X < 0 before the first frame). */
	FVector2D AimCameraNow = FVector2D(-1.0, -1.0);
	bool bAimHeld = false;
	bool bFocusLocked = false;
	void FollowFocus(float DeltaTime);
	/** Real seconds since the last roll ended (large when none): the lock-on view eases back in over this. */
	float SinceRoll = 100.0f;
	/** The third person camera's pivot, following the drawn body softly (Elden Ring: the camera glides after a roll,
	 *  it isn't bolted to the body). Cleared by SnapCamera. */
	FVector SmoothFeet = FVector::ZeroVector;
	bool bSmoothFeetValid = false;
	bool bRollingNow = false;
	/** 0 running to 1 rolling, eased: how softly the camera follows. */
	float RollFollow = 0.0f;
	float CameraDelta = 0.0f;
	double CameraTrail = 0.0;
	bool bScriptedAim = false;
	bool bScriptAimHeld = false;
	float AimBlend = 0.0f;
	float EffectiveArm = 350.0f;
	bool bScripted = false;
	double ScriptRight = 0.0;
	double ScriptForward = 0.0;
	bool bScriptWalk = false;
	bool bScriptSprint = false;
};
