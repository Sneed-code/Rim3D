#include "ProtagonistPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"

#include "HandsOnSubsystem.h"
#include "ProtagonistSubsystem.h"
#include "ShooterFeelSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Sim.h"

namespace
{
	// Degrees per mouse count.
	constexpr float ProtagonistMouseTurn = 0.12f;
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
	constexpr float ProtagonistAimShoulder = 42.0f;
	constexpr float ProtagonistNormalFov = 80.0f;
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
	PlayerInputComponent->BindKey(EKeys::Tab, IE_Pressed, this, &AProtagonistPawn::OnSwitchModePressed);
	PlayerInputComponent->BindKey(EKeys::V, IE_Pressed, this, &AProtagonistPawn::OnToggleViewPressed);
	PlayerInputComponent->BindKey(EKeys::T, IE_Pressed, this, &AProtagonistPawn::OnTakeOverPressed);
	PlayerInputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &AProtagonistPawn::OnZoomIn);
	PlayerInputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &AProtagonistPawn::OnZoomOut);
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

void AProtagonistPawn::SetScriptedInput(double Right, double Forward, bool bWalk)
{
	bScripted = true;
	ScriptRight = Right;
	ScriptForward = Forward;
	bScriptWalk = bWalk;
}

void AProtagonistPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	double Right = 0.0;
	double Forward = 0.0;
	bool bWalk = false;
	if (bScripted)
	{
		Right = ScriptRight;
		Forward = ScriptForward;
		bWalk = bScriptWalk;
	}
	else if (APlayerController* PC = GetPlayerController())
	{
		Right = (PC->IsInputKeyDown(EKeys::D) ? 1.0 : 0.0) - (PC->IsInputKeyDown(EKeys::A) ? 1.0 : 0.0);
		Forward = (PC->IsInputKeyDown(EKeys::W) ? 1.0 : 0.0) - (PC->IsInputKeyDown(EKeys::S) ? 1.0 : 0.0);
		bWalk = PC->IsInputKeyDown(EKeys::LeftControl) || PC->IsInputKeyDown(EKeys::RightControl);
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		PC->GetInputMouseDelta(MouseX, MouseY);
		if (MouseX != 0.0f || MouseY != 0.0f)
		{
			Look(MouseX * ProtagonistMouseTurn, MouseY * ProtagonistMouseTurn);
		}
	}
	if (Thing != colony::kNoEntity)
	{
		Move(Right, Forward, bWalk, DeltaTime);
	}

	// Holding aim: the right mouse button with a gun in hand (a melee weapon blocks with it instead).
	bool bWantAim = false;
	if (bScriptedAim)
	{
		bWantAim = bScriptAimHeld;
	}
	else if (const APlayerController* PC = GetPlayerController(); PC && Thing != colony::kNoEntity)
	{
		bWantAim = PC->IsInputKeyDown(EKeys::RightMouseButton);
	}
	if (bWantAim)
	{
		const USimSubsystem* Bridge = GetBridge();
		const colony::ThingView* View = Bridge && Bridge->GetSim() ? Bridge->GetCurrentSnapshot().find(Thing) : nullptr;
		bWantAim = View && !View->downed && !View->dead && View->weapon.valid() && Bridge->GetDefs() && Bridge->GetDefs()->get(View->weapon).has("verbs");
	}
	bAimHeld = bWantAim;
	AimBlend = FMath::FInterpConstantTo(AimBlend, bAimHeld ? 1.0f : 0.0f, DeltaTime, ProtagonistAimBlendPerSecond);
	UpdateCamera();
}

bool AProtagonistPawn::IsBodyHiddenInView() const
{
	return bFirstPerson || (UShooterFeelSubsystem::IsAimFirstPerson() && AimBlend > 0.8f);
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
		Target = Wanted * (bWalk ? kWalkSpeed : CruiseSpeed) * Factor;
	}
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
		return colony::pawnFitsAt(Map, static_cast<float>(Position.X + Try.X), static_cast<float>(Position.Y + Try.Y), colony::kPawnRadius, Level);
	};
	// Pushing into stairs or a ladder (or back towards them from their head, into the stairwell over them, which may
	// have a floor laid) climbs them.
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

void AProtagonistPawn::SetFirstPerson(bool bFirst)
{
	bFirstPerson = bFirst;
	SnapCamera();
}

void AProtagonistPawn::SnapCamera()
{
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
	const FVector Eye = Feet + FVector(0.0, 0.0, kEyeHeight) + FRotator(0.0, Yaw, 0.0).Vector() * ProtagonistEyeForward;
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
		const FVector Pivot = Feet + FVector(0.0, 0.0, ProtagonistPivotHeight);
		EffectiveArm = FMath::Lerp(ArmLength, FMath::Min(ArmLength, ProtagonistAimArm), Blend);
		const FVector Right = FRotator(0.0, Yaw, 0.0).RotateVector(FVector::RightVector);
		Location = Pivot - Look.Vector() * EffectiveArm + Right * (ProtagonistAimShoulder * Blend);
		// Never under the ground on a hillside.
		const FVector Local = (Location - Bridge->GetWorldOrigin()) / 100.0;
		const colony::Map& Map = Bridge->GetSim()->map();
		if (Local.X >= 0.0 && Local.Y >= 0.0 && Local.X < Map.width() && Local.Y < Map.height())
		{
			const double GroundZ = Bridge->SimToWorld(static_cast<float>(Local.X), static_cast<float>(Local.Y), 0).Z;
			Location.Z = FMath::Max(Location.Z, GroundZ + ProtagonistMinCameraHeight);
		}
		// Aimed in the first person way: the camera goes to the eyes.
		if (UShooterFeelSubsystem::IsAimFirstPerson())
		{
			Location = FMath::Lerp(Location, Eye, Blend);
			EffectiveArm = FMath::Lerp(EffectiveArm, 0.0f, Blend);
		}
	}
	SetActorLocationAndRotation(Location, Look);

	// The field of view narrows while aiming, and the camera is nudged by the gun's kick, the shake and a roll (the aim
	// above is not: the crosshair stays where the bullet goes).
	Camera->SetFieldOfView(FMath::Lerp(ProtagonistNormalFov, UShooterFeelSubsystem::GetAimFov(), Blend));
	if (UWorld* World = GetWorld())
	{
		if (UShooterFeelSubsystem* Feel = World->GetSubsystem<UShooterFeelSubsystem>())
		{
			const FCameraFeel& Nudge = Feel->GetCameraFeel();
			Camera->SetRelativeRotation(Nudge.Rotation);
			Camera->SetRelativeLocation(Nudge.Location);
			FPostProcessSettings& PostProcess = Camera->PostProcessSettings;
			PostProcess.bOverride_MotionBlurAmount = true;
			PostProcess.MotionBlurAmount = Nudge.MotionBlur;
			PostProcess.bOverride_VignetteIntensity = true;
			PostProcess.VignetteIntensity = ProtagonistBaseVignette + Nudge.Vignette;
			PostProcess.bOverride_SceneFringeIntensity = true;
			PostProcess.SceneFringeIntensity = Nudge.Fringe;
			Camera->PostProcessBlendWeight = 1.0f;
		}
	}
}
