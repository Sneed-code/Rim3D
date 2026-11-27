#include "ProtagonistPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"

#include "GunRangeSubsystem.h"
#include "HandsOnSubsystem.h"
#include "KeyBindings.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistSubsystem.h"
#include "ShooterFeelSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/BodyMotion.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Water.h"

namespace
{
	// Degrees per unit of the mouse axis (the engine has already scaled each count by the input's 0.07), so the
	// default turns about 0.08 degrees a count. 0.12 was 0.008 a count and much too slow (sneed's G2 playtest), and
	// 0.7 still too slow in the gun range; the Settings window has a slider (saved in GameUserSettings.ini).
	TAutoConsoleVariable<float> GProtagonistMouseTurn(TEXT("Protagonist.MouseTurn"), AProtagonistPawn::kDefaultMouseTurn,
		TEXT("How fast the mouse turns the view in direct control: degrees per unit of mouse axis (1.2 default, higher turns faster)."));
	const TCHAR* const ProtagonistControlsSection = TEXT("Rim3D.Controls");
	bool bProtagonistMouseTurnLoaded = false;

	void ProtagonistLoadMouseTurn()
	{
		if (bProtagonistMouseTurnLoaded || !GConfig)
		{
			return;
		}
		bProtagonistMouseTurnLoaded = true;
		float Saved = 0.0f;
		if (GConfig->GetFloat(ProtagonistControlsSection, TEXT("MouseTurn"), Saved, GGameUserSettingsIni) && Saved > 0.0f)
		{
			GProtagonistMouseTurn->Set(Saved, ECVF_SetByGameSetting);
		}
	}
	TAutoConsoleVariable<float> GProtagonistDepthOfField(TEXT("Look.DepthOfField"), 2.0f,
		TEXT("The third person camera's cinematic depth of field: the lens f-stop, focused on the colonist (lower blurs the distance more; 0 turns it off). Fades out while aiming and in first person."));
	// The sprint toggle belongs to the player, not the body: it carries over to the next colonist taken over.
	bool bProtagonistSprintToggle = false;
	constexpr float ProtagonistMaxPitch = 80.0f;
	// One wheel notch scales the third person distance by this much.
	constexpr float ProtagonistZoomPerNotch = 0.85f;
	// How quickly the body gets up to speed and stops, per second (higher is snappier).
	constexpr float ProtagonistAcceleration = 12.0f;
	// The local position and the sim's may differ by this much (metres) before the sim wins.
	constexpr float ProtagonistReconcileDistance = 1.0f;
	// The camera never goes lower than this above the ground (cm).
	constexpr double ProtagonistMinCameraHeight = 30.0;
	// First person: how far in front of the head the camera sits (cm).
	constexpr double ProtagonistEyeForward = 12.0;
	// Third person looks at a point this far above the feet (cm).
	constexpr double ProtagonistPivotHeight = 150.0;
	// Walking into stairs within about 45 degrees of their way up (or down) climbs them; the sim is given this long
	// (real seconds) to start the climb before the body asks again.
	constexpr double ProtagonistClimbDot = 0.7;
	constexpr float ProtagonistClimbWait = 0.5f;
	// Aiming (U21): how fast the view blends, the closest the over-the-shoulder camera comes and how far right of the body it sits (cm).
	constexpr float ProtagonistAimBlendPerSecond = 5.5f;
	constexpr float ProtagonistAimArm = 120.0f;
	// 45 right and 8 up: the colonist left of the crosshair, the rifle pointing into the middle of the screen. Earlier
	// pictures with the rifle hidden behind the head (2026-11-19/20) came from the body keeping its old facing while aimed
	// (fixed in PawnActor); at 55-68 right and 16-26 up the body sat in the bottom corner and the gun left the frame.
	constexpr float ProtagonistAimShoulder = 45.0f;
	constexpr float ProtagonistAimLift = 8.0f;
	constexpr float ProtagonistAimCameraEase = 6.0f;
	constexpr float ProtagonistNormalFov = 80.0f;
	// A scoped gun's aim goes to the scope only once this far aimed in (so the view never passes through the head).
	constexpr float ProtagonistScopeAt = 0.97f;
	// Locked on (docs/design/focus.md): how fast the view turns to keep the focus ahead, degrees a second.
	constexpr float ProtagonistFocusTurnThird = 360.0f;
	constexpr float ProtagonistFocusTurnFirst = 100.0f;
	// The locked-on view closes its gap with this time constant (s): it eases on, faster when further off,
	// never over the turn rates above (Elden Ring: no whip).
	constexpr float ProtagonistFocusEaseSeconds = 0.08f;
	// Through a roll the lock-on view keeps following the enemy, more softly (sneed 2026-11-24: no swing back after the
	// roll; the body itself curves round to the enemy in the sim, MeleeSystem::steerRoll): this share of the turn rate
	// and this time constant. After the roll it eases back up to the full rate over ProtagonistFocusRecentreSeconds.
	constexpr float ProtagonistFocusRollShare = 0.5f;
	constexpr float ProtagonistFocusRollEaseSeconds = 0.2f;
	constexpr float ProtagonistFocusRecentreSeconds = 0.5f;
	// The third person camera follows the body softly (time constants, real seconds): barely while running, so it feels
	// as it did, and more in a roll, so the view glides after the tumble instead of lurching with it (Elden Ring). Aiming
	// follows a little more tightly. It never trails more than the most below (cm), and jumps (a teleport, a new body) are not followed.
	constexpr float ProtagonistFollowRunSeconds = 0.05f;
	constexpr float ProtagonistFollowRollSeconds = 0.2f;
	constexpr float ProtagonistFollowAimSeconds = 0.04f;
	constexpr float ProtagonistRollFollowPerSecond = 3.0f;
	constexpr double ProtagonistFollowMaxTrail = 140.0;
	constexpr double ProtagonistFollowTeleport = 500.0;
	// The engine's default vignette, which the pulses add to.
	constexpr float ProtagonistBaseVignette = 0.4f;

	float ProtagonistSmooth(float Value)
	{
		const float T = FMath::Clamp(Value, 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}
}

AProtagonistPawn::AProtagonistPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Root);
	Camera->SetFieldOfView(80.0f);
	Camera->bUsePawnControlRotation = false;
}

bool AProtagonistPawn::IsSprintToggleOn()
{
	return bProtagonistSprintToggle;
}

void AProtagonistPawn::SetSprintToggle(bool bOn)
{
	bProtagonistSprintToggle = bOn;
}

float AProtagonistPawn::GetMouseTurn()
{
	ProtagonistLoadMouseTurn();
	return GProtagonistMouseTurn.GetValueOnGameThread();
}

void AProtagonistPawn::SetMouseTurn(float Value)
{
	ProtagonistLoadMouseTurn();
	const float Clamped = FMath::Clamp(Value, 0.1f, 5.0f);
	GProtagonistMouseTurn->Set(Clamped, ECVF_SetByGameSetting);
	if (GConfig)
	{
		GConfig->SetFloat(ProtagonistControlsSection, TEXT("MouseTurn"), Clamped, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
}

USimSubsystem* AProtagonistPawn::GetBridge() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<USimSubsystem>() : nullptr;
}

APlayerController* AProtagonistPawn::GetPlayerController() const
{
	return Cast<APlayerController>(GetController());
}

void AProtagonistPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	// The mouse wheel zooms; the other keys are rebindable (KeyBindings.h) and read in Tick.
	PlayerInputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &AProtagonistPawn::OnZoomIn);
	PlayerInputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &AProtagonistPawn::OnZoomOut);
}

bool AProtagonistPawn::Jump()
{
	const USimSubsystem* Bridge = GetBridge();
	const USimPawnSubsystem* Pawns = GetWorld() ? GetWorld()->GetSubsystem<USimPawnSubsystem>() : nullptr;
	ASimPawnActor* Actor = Pawns && Thing != colony::kNoEntity ? Pawns->FindPawn(Thing) : nullptr;
	if (!Actor || !Bridge || Bridge->GetSpeed() == 0 || bClimbing)
	{
		return false;
	}
	return Actor->StartJump();
}

void AProtagonistPawn::OnSwitchModePressed()
{
	if (UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>())
	{
		Protagonist->ToggleMode();
	}
}

void AProtagonistPawn::OnTakeOverPressed()
{
	if (UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>())
	{
		Protagonist->TakeOverLookedAt();
	}
}

void AProtagonistPawn::Begin(colony::Entity InThing, const FVector2D& SimPosition, float InCruiseSpeed, float InYaw, int32 InLevel)
{
	Thing = InThing;
	Yaw = FRotator::NormalizeAxis(InYaw);
	Position = SimPosition;
	Level = InLevel;
	bClimbing = false;
	bClimbWanted = false;
	ClimbWait = 0.0f;
	Velocity = FVector2D::ZeroVector;
	CruiseSpeed = InCruiseSpeed > 0.1f ? InCruiseSpeed : 4.6f;
	SnapCamera();
}

void AProtagonistPawn::SetScriptedInput(double Right, double Forward, bool bWalk, bool bSprint)
{
	bScripted = true;
	ScriptRight = Right;
	ScriptForward = Forward;
	bScriptWalk = bWalk;
	bScriptSprint = bSprint;
}

void AProtagonistPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	double Right = 0.0;
	double Forward = 0.0;
	bool bWalk = false;
	bSprinting = false;
	if (bScripted)
	{
		Right = ScriptRight;
		Forward = ScriptForward;
		bWalk = bScriptWalk;
		bSprinting = bScriptSprint && !bWalk;
	}
	else if (APlayerController* PC = GetPlayerController())
	{
		using Rim3DKeys::IsDown;
		using Rim3DKeys::WasPressed;
		UProtagonistSubsystem* Unstuck = GetWorld() ? GetWorld()->GetSubsystem<UProtagonistSubsystem>() : nullptr;
		if (Unstuck && WasPressed(PC, ERim3DAction::Unstuck))
		{
			Unstuck->StartUnstuck();
		}
		// F12 Unstuck: standing still, no actions, until the sim moves the colonist (docs/design/earthworks.md).
		const bool bUnstucking = Unstuck && Unstuck->IsUnstucking();
		Right = bUnstucking ? 0.0 : (IsDown(PC, ERim3DAction::MoveRight) ? 1.0 : 0.0) - (IsDown(PC, ERim3DAction::MoveLeft) ? 1.0 : 0.0);
		Forward = bUnstucking ? 0.0 : (IsDown(PC, ERim3DAction::MoveForward) ? 1.0 : 0.0) - (IsDown(PC, ERim3DAction::MoveBack) ? 1.0 : 0.0);
		bWalk = IsDown(PC, ERim3DAction::WalkSlow);
		// Sprint is a toggle (sneed 2026-11-17): on until pressed again, and it stays on when you take over another colonist.
		// It doesn't sprint while walking slowly or aiming.
		// Shift is also the heavy attack's key when held with a swing (souls-combat.md): sprint then switches when the key
		// comes up, and not at all if a swing was pressed while it was held.
		if (IsDown(PC, ERim3DAction::Sprint) && IsDown(PC, ERim3DAction::HeavyAttack) && WasPressed(PC, ERim3DAction::Attack))
		{
			bSprintKeyChorded = true;
		}
		if (Rim3DKeys::WasReleased(PC, ERim3DAction::Sprint) && !Rim3DKeys::AreGameKeysBlocked())
		{
			if (!bSprintKeyChorded)
			{
				SetSprintToggle(!IsSprintToggleOn());
			}
			bSprintKeyChorded = false;
		}
		bSprinting = IsSprintToggleOn() && !bWalk && !bAimHeld;
		if (!bUnstucking && WasPressed(PC, ERim3DAction::Jump))
		{
			Jump();
		}
		if (WasPressed(PC, ERim3DAction::SwitchMode))
		{
			OnSwitchModePressed();
		}
		if (WasPressed(PC, ERim3DAction::TakeOver))
		{
			OnTakeOverPressed();
		}
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		PC->GetInputMouseDelta(MouseX, MouseY);
		// The build screen (B) has the mouse for its cursor: the view holds still under it.
		if ((MouseX != 0.0f || MouseY != 0.0f) && !Rim3DKeys::AreGameKeysBlocked())
		{
			const float Turn = FMath::Max(0.01f, GetMouseTurn());
			// Locked on in third person the camera keeps the enemy ahead: the mouse only tilts it (Dark Souls).
			Look(bFocusLocked && !bFirstPerson ? 0.0f : MouseX * Turn, MouseY * Turn);
		}
	}
	if (Thing != colony::kNoEntity)
	{
		FollowFocus(DeltaTime);
		Move(Right, Forward, bWalk, DeltaTime);
	}

	// Holding aim: the right mouse button with a gun in hand (a melee weapon or a shield blocks instead).
	bool bWantAim = false;
	if (bScriptedAim)
	{
		bWantAim = bScriptAimHeld;
	}
	else if (const APlayerController* PC = GetPlayerController(); PC && Thing != colony::kNoEntity)
	{
		bWantAim = Rim3DKeys::IsDown(PC, ERim3DAction::AimBlock);
	}
	if (bWantAim)
	{
		const USimSubsystem* Bridge = GetBridge();
		const colony::ThingView* View = Bridge && Bridge->GetSim() ? Bridge->GetCurrentSnapshot().find(Thing) : nullptr;
		// With a shield on the arm the right mouse button holds it up instead (UWeaponDrawSubsystem, docs/design/shields.md).
		bWantAim = View && !View->downed && !View->dead && View->weapon.valid() && Bridge->GetDefs() && Bridge->GetDefs()->get(View->weapon).has("verbs")
			&& !(View->shield.valid() && !View->shieldSlung);
	}
	bAimHeld = bWantAim;
	AimBlend = FMath::FInterpConstantTo(AimBlend, bAimHeld ? 1.0f : 0.0f, DeltaTime, ProtagonistAimBlendPerSecond);
	RollFollow = FMath::FInterpConstantTo(RollFollow, bRollingNow ? 1.0f : 0.0f, DeltaTime, ProtagonistRollFollowPerSecond);
	CameraDelta = DeltaTime;
	UpdateCamera();
	CameraDelta = 0.0f;
}

bool AProtagonistPawn::HasGunDrawn() const
{
	const USimSubsystem* Bridge = GetBridge();
	const colony::ThingView* View = Bridge && Bridge->GetSim() ? Bridge->GetCurrentSnapshot().find(Thing) : nullptr;
	return View && View->weapon.valid() && Bridge->GetDefs() && Bridge->GetDefs()->get(View->weapon).has("verbs");
}

void AProtagonistPawn::FollowFocus(float DeltaTime)
{
	// The sim turns the body to face the focus (MeleeSystem::faceFocus); the view turns after it so WASD strafes round the
	// enemy: quickly in third person, gently in first. Not while aiming a gun (the aim leads) or rolling.
	bFocusLocked = false;
	const USimSubsystem* Bridge = GetBridge();
	const colony::ThingView* Me = Bridge && Bridge->GetSim() ? Bridge->GetCurrentSnapshot().find(Thing) : nullptr;
	// The roll's clock: the camera follows softly while it lasts, and the lock-on eases back in after it.
	const bool bRolling = Me && Me->rollPhase > 0;
	if (bRolling)
	{
		SinceRoll = 0.0f;
	}
	else if (Me && Bridge->GetSpeed() != 0)
	{
		SinceRoll = FMath::Min(SinceRoll + DeltaTime, 100.0f);
	}
	bRollingNow = bRolling;
	// Nor with a gun in hand (free aim, sneed 2026-11-18, docs/design/lock-on.md), nor while sprinting (the run goes
	// where the keys say, as in Elden Ring).
	if (!Me || Me->focusTarget == colony::kNoEntity || bAimHeld || bSprinting || HasGunDrawn())
	{
		return;
	}
	const colony::ThingView* Target = Bridge->GetCurrentSnapshot().find(Me->focusTarget);
	if (!Target || Target->level != Level)
	{
		return;
	}
	const FVector2D To(Target->x - Position.X, Target->y - Position.Y);
	if (To.SizeSquared() < 0.09)
	{
		return;
	}
	bFocusLocked = true;
	const float Wanted = FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(To.Y), static_cast<float>(To.X)));
	// Eased, not a fixed spin (sneed 2026-11-19: after a roll the view whipped back to the enemy): it closes a share of
	// the gap each frame, capped at the turn rate; in a roll softer and slower, easing back to full after it, so the view
	// glides along with the roll and never has a gap to swing across at its end.
	const float Recentre = bRolling ? 0.0f : ProtagonistSmooth(SinceRoll / ProtagonistFocusRecentreSeconds);
	const float Cap = (bFirstPerson ? ProtagonistFocusTurnFirst : ProtagonistFocusTurnThird)
		* FMath::Lerp(ProtagonistFocusRollShare, 1.0f, Recentre);
	const float Gap = FMath::FindDeltaAngleDegrees(Yaw, Wanted);
	const float Share = 1.0f - FMath::Exp(-DeltaTime / FMath::Lerp(ProtagonistFocusRollEaseSeconds, ProtagonistFocusEaseSeconds, Recentre));
	const float Step = FMath::Clamp(Gap * Share, -Cap * DeltaTime, Cap * DeltaTime);
	Yaw = FRotator::NormalizeAxis(Yaw + Step);
}

bool AProtagonistPawn::IsBodyHiddenInView() const
{
	return bScopeView;
}

float AProtagonistPawn::GetFieldOfView() const
{
	return Camera ? Camera->FieldOfView : 80.0f;
}

FRotator AProtagonistPawn::GetFeelRotation() const
{
	return Camera ? Camera->GetRelativeRotation() : FRotator::ZeroRotator;
}

float AProtagonistPawn::GetMotionBlurAmount() const
{
	return Camera ? Camera->PostProcessSettings.MotionBlurAmount : 0.0f;
}

void AProtagonistPawn::Move(double Right, double Forward, bool bWalk, float DeltaTime)
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	// A paused game freezes the body; the view still turns. So does a climb: the sim moves the colonist.
	ClimbWait = FMath::Max(0.0f, ClimbWait - DeltaTime);
	if (Bridge->GetSpeed() == 0 || bClimbing)
	{
		Velocity = FVector2D::ZeroVector;
		return;
	}
	const colony::Map& Map = Bridge->GetSim()->map();
	const colony::BodyMotionSystem& Bodies = Bridge->GetSim()->combat().motion();

	const double YawRadians = FMath::DegreesToRadians(static_cast<double>(Yaw));
	const FVector2D Ahead(FMath::Cos(YawRadians), FMath::Sin(YawRadians));
	const FVector2D Side(-Ahead.Y, Ahead.X);
	const FVector2D Wanted = (Ahead * Forward + Side * Right).GetSafeNormal();
	FVector2D Target = FVector2D::ZeroVector;
	if (!Wanted.IsNearlyZero())
	{
		// Ground and slope slow the walk the way they slow every pawn (the sim's own numbers).
		const float Factor = colony::walkSpeedFactor(Map, static_cast<float>(Position.X), static_cast<float>(Position.Y),
			static_cast<float>(Wanted.X), static_cast<float>(Wanted.Y));
		// ... and so do load, worn weight and hunger (a starving colonist walks at 75%): the sim's own factor.
		const colony::ThingView* Self = Bridge->GetCurrentSnapshot().find(Thing);
		const float SimFactor = Self ? FMath::Clamp(Self->moveFactor, 0.05f, 2.0f) : 1.0f;
		const float Speed = bWalk ? kWalkSpeed : (bSprinting ? FMath::Max(CruiseSpeed * kSprintFactor, kMinSprintSpeed) : CruiseSpeed);
		Target = Wanted * Speed * Factor * SimFactor;
	}
	TargetSpeed = static_cast<float>(Target.Size());
	Velocity = FMath::Vector2DInterpTo(Velocity, Target, DeltaTime, ProtagonistAcceleration);
	if (Velocity.SizeSquared() < 1.0e-6f)
	{
		Velocity = FVector2D::ZeroVector;
		return;
	}

	// The whole move, or sliding along one axis (a wall), or no move: the sim does the same.
	const FVector2D Delta = Velocity * DeltaTime;
	const FVector2D Tries[3] = {Delta, FVector2D(Delta.X, 0.0), FVector2D(0.0, Delta.Y)};
	const auto Fits = [&](const FVector2D& Try)
	{
		// The player may swim: deep water counts as open (docs/design/water.md), as the sim's DirectMove does. Other
		// standing bodies stop the walk the same way (sneed 2026-11-18: no running through people).
		const float ToX = static_cast<float>(Position.X + Try.X);
		const float ToY = static_cast<float>(Position.Y + Try.Y);
		return colony::swimmerFitsAt(Map, ToX, ToY, colony::kPawnRadius, Level) &&
			Bodies.bodiesClear(Thing, static_cast<float>(Position.X), static_cast<float>(Position.Y), ToX, ToY, Level);
	};
	// Pushing into stairs or a ladder (or back towards them from their head, into the stairwell over them, which may
	// have a floor laid) climbs them. A climbable wall is the same (docs/design/climbing.md): walking into it climbs it,
	// walking off its top edge climbs back down.
	if (!Wanted.IsNearlyZero() && !Delta.IsNearlyZero(1.0e-7) && ClimbWait <= 0.0f)
	{
		const colony::CellPos Here{FMath::FloorToInt32(Position.X), FMath::FloorToInt32(Position.Y), Level};
		const FVector2D Lead = Position + Delta + Wanted * (colony::kPawnRadius + 0.05);
		const bool bBlocked = !Fits(Delta);
		for (const colony::LevelLink& Link : Map.levelLinks())
		{
			if (Link.low != Here && Link.high != Here)
			{
				continue;
			}
			const colony::CellPos Into{FMath::FloorToInt32(Lead.X), FMath::FloorToInt32(Lead.Y), Level};
			const colony::CellPos Under{Into.x, Into.y, Level - 1};
			const bool bIntoStairs = (Map.inBounds(Into) && Map.buildingAt(Into) == Link.building) || (Map.inBounds(Under) && Map.buildingAt(Under) == Link.building);
			const colony::CellPos Other = Link.low == Here ? Link.high : Link.low;
			const FVector2D Towards = FVector2D(Other.x - Here.x, Other.y - Here.y).GetSafeNormal();
			if ((bBlocked || bIntoStairs) && FVector2D::DotProduct(Towards, Wanted) > ProtagonistClimbDot)
			{
				bClimbWanted = true;
				ClimbDirection = Wanted;
				ClimbWait = ProtagonistClimbWait;
				Velocity = FVector2D::ZeroVector;
				return;
			}
		}
	}
	bool bMoved = false;
	for (const FVector2D& Try : Tries)
	{
		if (!Try.IsNearlyZero(1.0e-7) && Fits(Try))
		{
			Position += Try;
			bMoved = true;
			break;
		}
	}
	if (!bMoved)
	{
		Velocity = FVector2D::ZeroVector;
	}
}

void AProtagonistPawn::Look(float YawDegrees, float PitchDegrees)
{
	Yaw = FRotator::NormalizeAxis(Yaw + YawDegrees);
	Pitch = FMath::Clamp(Pitch + PitchDegrees, -ProtagonistMaxPitch, ProtagonistMaxPitch);
}

void AProtagonistPawn::Zoom(float Notches)
{
	// With a building in hand the wheel picks the building instead (U10).
	if (const UWorld* World = GetWorld())
	{
		if (UHandsOnSubsystem* Hands = World->GetSubsystem<UHandsOnSubsystem>(); Hands && Hands->HandleWheel(Notches))
		{
			return;
		}
	}
	ArmLength = FMath::Clamp(ArmLength * FMath::Pow(ProtagonistZoomPerNotch, Notches), kMinArm, kMaxArm);
}

void AProtagonistPawn::SetFirstPerson(bool /*bFirst*/)
{
	bFirstPerson = false;
	SnapCamera();
}

void AProtagonistPawn::SnapCamera()
{
	bSmoothFeetValid = false;
	UpdateCamera();
}

bool AProtagonistPawn::Reconcile(const FVector2D& SimPosition)
{
	if (FVector2D::Distance(Position, SimPosition) > ProtagonistReconcileDistance)
	{
		Position = SimPosition;
		Velocity = FVector2D::ZeroVector;
		return true;
	}
	return false;
}

void AProtagonistPawn::SetSimPosition(const FVector2D& SimPosition)
{
	Position = SimPosition;
	Velocity = FVector2D::ZeroVector;
}

void AProtagonistPawn::FollowSim(const FVector2D& SimPosition, int32 SimLevel, bool bSimClimbing)
{
	if (bSimClimbing)
	{
		bClimbing = true;
		Velocity = FVector2D::ZeroVector;
		return;
	}
	if (bClimbing || SimLevel != Level)
	{
		// Off the stairs: on at the other end, on its level.
		bClimbing = false;
		Level = SimLevel;
		SetSimPosition(SimPosition);
	}
}

bool AProtagonistPawn::TakeClimbRequest(FVector2D& OutDirection)
{
	if (!bClimbWanted)
	{
		return false;
	}
	bClimbWanted = false;
	OutDirection = ClimbDirection;
	return true;
}

FVector AProtagonistPawn::GetViewLocation() const
{
	// The aim's own place: the camera component is nudged on top of it by the kick and the shake.
	return GetActorLocation();
}

FVector AProtagonistPawn::GetViewDirection() const
{
	return GetActorRotation().Vector();
}

// The camera follows where the colonist is drawn (the sim's blended position), so it moves as
// smoothly as the body does; the feet fall back to the local position before the first snapshot.
void AProtagonistPawn::UpdateCamera()
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	FVector Feet;
	if (Thing == colony::kNoEntity || !Bridge->GetBlendedLocation(Thing, Feet))
	{
		Feet = Bridge->SimToWorld(static_cast<float>(Position.X), static_cast<float>(Position.Y), 0);
	}
	const FRotator Look(Pitch, Yaw, 0.0);
	const float Blend = ProtagonistSmooth(AimBlend);
	// The eyes and the camera's pivot were set for the 1.80 m man: they follow this colonist's own height, so a short
	// colonist sees the world from lower down (docs/design/gear-size.md).
	double BodyScale = 1.0;
	if (const USimPawnSubsystem* Pawns = GetWorld() ? GetWorld()->GetSubsystem<USimPawnSubsystem>() : nullptr)
	{
		if (const ASimPawnActor* Drawn = Thing != colony::kNoEntity ? Pawns->FindPawn(Thing) : nullptr)
		{
			BodyScale = FMath::Clamp(Drawn->GetBodyScale(), 0.5f, 1.5f);
			// A swimming body floats up from the bottom; the eyes and the camera go with it (docs/design/water.md, W4).
			Feet.Z += Drawn->GetSwimLiftCm();
		}
	}
	// The soft follow (third person only): the pivot glides after the drawn body, more in a roll, not at all while aimed.
	const double Trail = FVector::Dist(SmoothFeet, Feet);
	if (!bSmoothFeetValid || bFirstPerson || Trail > ProtagonistFollowTeleport)
	{
		SmoothFeet = Feet;
		bSmoothFeetValid = true;
	}
	else if (CameraDelta > 0.0f)
	{
		// Aimed it still follows softly, just less (sneed 2026-11-23: the view shook while walking and aiming; locked to
		// the body it took every hitch of the drawn position).
		const float Seconds = FMath::Lerp(FMath::Lerp(ProtagonistFollowRunSeconds, ProtagonistFollowRollSeconds, ProtagonistSmooth(RollFollow)),
			ProtagonistFollowAimSeconds, Blend);
		const double Share = Seconds > 1.0e-3f ? 1.0 - FMath::Exp(-CameraDelta / Seconds) : 1.0;
		SmoothFeet = FMath::Lerp(SmoothFeet, Feet, Share);
		const FVector Behind = SmoothFeet - Feet;
		if (Behind.Size() > ProtagonistFollowMaxTrail)
		{
			SmoothFeet = Feet + Behind.GetSafeNormal() * ProtagonistFollowMaxTrail;
		}
	}
	CameraTrail = FVector::Dist2D(SmoothFeet, Feet);
	const FVector Eye = Feet + FVector(0.0, 0.0, kEyeHeight * BodyScale) + FRotator(0.0, Yaw, 0.0).Vector() * ProtagonistEyeForward;
	FVector Location;
	EffectiveArm = ArmLength;
	if (bFirstPerson)
	{
		Location = Eye;
		EffectiveArm = 0.0f;
	}
	else
	{
		// Aimed (over the shoulder): the camera comes in close behind and to the right of the colonist.
		const FVector Pivot = SmoothFeet + FVector(0.0, 0.0, ProtagonistPivotHeight * BodyScale);
		EffectiveArm = FMath::Lerp(ArmLength, FMath::Min(ArmLength, ProtagonistAimArm), Blend);
		const FVector Right = FRotator(0.0, Yaw, 0.0).RotateVector(FVector::RightVector);
		// A gun may ask for its own shoulder offset (guns.json "aimCamera": the rocket launcher sits in front of the
		// usual one); it eases over when the gun changes.
		FVector2D WantCamera(ProtagonistAimShoulder, ProtagonistAimLift);
		if (const UWorld* CamWorld = GetWorld())
		{
			if (const UGunRangeSubsystem* CamRange = CamWorld->GetSubsystem<UGunRangeSubsystem>())
			{
				const FVector2D GunCamera = CamRange->GetAimCamera();
				WantCamera = FVector2D(GunCamera.X >= 0.0 ? GunCamera.X : WantCamera.X, GunCamera.Y >= 0.0 ? GunCamera.Y : WantCamera.Y);
			}
		}
		if (AimCameraNow.X < 0.0)
		{
			AimCameraNow = WantCamera;
		}
		AimCameraNow = FMath::Vector2DInterpTo(AimCameraNow, WantCamera, CameraDelta, ProtagonistAimCameraEase);
		Location = Pivot - Look.Vector() * EffectiveArm + Right * (AimCameraNow.X * Blend) + FVector(0.0, 0.0, AimCameraNow.Y * BodyScale * Blend);
		// Never under the ground on a hillside.
		const FVector Local = (Location - Bridge->GetWorldOrigin()) / 100.0;
		const colony::Map& Map = Bridge->GetSim()->map();
		if (Local.X >= 0.0 && Local.Y >= 0.0 && Local.X < Map.width() && Local.Y < Map.height())
		{
			const double GroundZ = Bridge->SimToWorld(static_cast<float>(Local.X), static_cast<float>(Local.Y), 0).Z;
			Location.Z = FMath::Max(Location.Z, GroundZ + ProtagonistMinCameraHeight);
		}
	}
	// A scoped gun (the bolt-action rifle) fully aimed: straight to the scope, never a first-person view of the arms or
	// gun (sneed 2026-11-22). The camera goes to the eyes with the body hidden, the scope's field of view and its ring
	// drawn over the view (UGunRangeSubsystem). Any other gun keeps the over-the-shoulder aim.
	float ScopeFov = 0.0f;
	if (const UWorld* World = GetWorld())
	{
		if (const UGunRangeSubsystem* Range = World->GetSubsystem<UGunRangeSubsystem>())
		{
			ScopeFov = Range->GetScopeFov();
		}
	}
	bScopeView = bScopeAllowed && ScopeFov > 0.0f && bAimHeld && AimBlend >= ProtagonistScopeAt;
	if (bScopeView)
	{
		Location = Eye;
		EffectiveArm = 0.0f;
	}
	SetActorLocationAndRotation(Location, Look);

	// The field of view narrows while aiming, and the camera is nudged by the gun's kick, the shake and a roll (the aim
	// above is not: the crosshair stays where the bullet goes).
	Camera->SetFieldOfView(bScopeView ? ScopeFov : FMath::Lerp(ProtagonistNormalFov, UShooterFeelSubsystem::GetAimFov(), Blend));
	if (UWorld* World = GetWorld())
	{
		if (UShooterFeelSubsystem* Feel = World->GetSubsystem<UShooterFeelSubsystem>())
		{
			const FCameraFeel& Nudge = Feel->GetCameraFeel();
			Camera->SetFieldOfView(bScopeView ? ScopeFov : FMath::Lerp(ProtagonistNormalFov, UShooterFeelSubsystem::GetAimFov(), Blend) + Nudge.FovAdd);
			Camera->SetRelativeRotation(Nudge.Rotation);
			Camera->SetRelativeLocation(Nudge.Location);
			FPostProcessSettings& PostProcess = Camera->PostProcessSettings;
			PostProcess.bOverride_MotionBlurAmount = true;
			PostProcess.MotionBlurAmount = Nudge.MotionBlur;
			PostProcess.bOverride_VignetteIntensity = true;
			PostProcess.VignetteIntensity = ProtagonistBaseVignette + Nudge.Vignette;
			PostProcess.bOverride_SceneFringeIntensity = true;
			PostProcess.SceneFringeIntensity = Nudge.Fringe;
			// Cinematic depth of field on the colonist, like a film camera: the far background softens a little. Off in first
			// person and while aiming, so a distant target stays sharp.
			const float FStop = GProtagonistDepthOfField.GetValueOnGameThread();
			const bool bFocus = FStop > 0.0f && !bScopeView && Blend < 0.99f && EffectiveArm > 1.0f;
			PostProcess.bOverride_DepthOfFieldFocalDistance = bFocus;
			PostProcess.bOverride_DepthOfFieldFstop = bFocus;
			PostProcess.bOverride_DepthOfFieldSensorWidth = bFocus;
			if (bFocus)
			{
				PostProcess.DepthOfFieldFocalDistance = static_cast<float>(EffectiveArm);
				// A wider aperture number while the aim blends in, so the blur fades out smoothly.
				PostProcess.DepthOfFieldFstop = FStop / FMath::Max(1.0f - Blend, 0.05f);
				PostProcess.DepthOfFieldSensorWidth = 36.0f;  // full frame
			}
			Camera->PostProcessBlendWeight = 1.0f;
		}
	}
}
