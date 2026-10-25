#include "ProtagonistPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"

#include "HandsOnSubsystem.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Map.h"
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

void AProtagonistPawn::Begin(colony::Entity InThing, const FVector2D& SimPosition, float InCruiseSpeed, float InYaw)
{
	Thing = InThing;
	Yaw = FRotator::NormalizeAxis(InYaw);
	Position = SimPosition;
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
	UpdateCamera();
}

void AProtagonistPawn::Move(double Right, double Forward, bool bWalk, float DeltaTime)
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	// A paused game freezes the body; the view still turns.
	if (Bridge->GetSpeed() == 0)
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
	bool bMoved = false;
	for (const FVector2D& Try : Tries)
	{
		if (!Try.IsNearlyZero(1.0e-7) &&
			colony::pawnFitsAt(Map, static_cast<float>(Position.X + Try.X), static_cast<float>(Position.Y + Try.Y), colony::kPawnRadius, 0))
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

FVector AProtagonistPawn::GetViewLocation() const
{
	return Camera->GetComponentLocation();
}

FVector AProtagonistPawn::GetViewDirection() const
{
	return Camera->GetComponentRotation().Vector();
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
	FVector Location;
	if (bFirstPerson)
	{
		Location = Feet + FVector(0.0, 0.0, kEyeHeight) + FRotator(0.0, Yaw, 0.0).Vector() * ProtagonistEyeForward;
	}
	else
	{
		const FVector Pivot = Feet + FVector(0.0, 0.0, ProtagonistPivotHeight);
		Location = Pivot - Look.Vector() * ArmLength;
		// Never under the ground on a hillside.
		const FVector Local = (Location - Bridge->GetWorldOrigin()) / 100.0;
		const colony::Map& Map = Bridge->GetSim()->map();
		if (Local.X >= 0.0 && Local.Y >= 0.0 && Local.X < Map.width() && Local.Y < Map.height())
		{
			const double GroundZ = Bridge->SimToWorld(static_cast<float>(Local.X), static_cast<float>(Local.Y), 0).Z;
			Location.Z = FMath::Max(Location.Z, GroundZ + ProtagonistMinCameraHeight);
		}
	}
	SetActorLocationAndRotation(Location, Look);
}
