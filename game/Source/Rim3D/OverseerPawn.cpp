#include "OverseerPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"

#include "OrdersSubsystem.h"
#include "OverseerSubsystem.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "TerrainDraw.h"

#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"

namespace
{
	constexpr float OverseerDefaultDistance = 4500.0f;
	constexpr float OverseerDefaultTilt = 55.0f;
	// One wheel notch scales the distance by this much.
	constexpr float OverseerZoomPerNotch = 0.85f;
	// Q and E turn at this many degrees a second; the middle button turns this many degrees a pixel.
	constexpr float OverseerKeyTurnSpeed = 90.0f;
	constexpr float OverseerMouseTurn = 0.25f;
	constexpr float OverseerZoomEase = 10.0f;
}

AOverseerPawn::AOverseerPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Root);
	Camera->SetFieldOfView(60.0f);
	Camera->bUsePawnControlRotation = false;
}

USimSubsystem* AOverseerPawn::GetBridge() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<USimSubsystem>() : nullptr;
}

APlayerController* AOverseerPawn::GetPlayerController() const
{
	return Cast<APlayerController>(GetController());
}

void AOverseerPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	PlayerInputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &AOverseerPawn::OnSelectPressed);
	PlayerInputComponent->BindKey(EKeys::LeftMouseButton, IE_Released, this, &AOverseerPawn::OnSelectReleased);
	PlayerInputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &AOverseerPawn::OnOrderPressed);
	PlayerInputComponent->BindKey(EKeys::R, IE_Pressed, this, &AOverseerPawn::OnRotateOrDraftPressed);
	PlayerInputComponent->BindKey(EKeys::Escape, IE_Pressed, this, &AOverseerPawn::OnClearPressed);
	PlayerInputComponent->BindKey(EKeys::F, IE_Pressed, this, &AOverseerPawn::OnFocusPressed);
	PlayerInputComponent->BindKey(EKeys::Home, IE_Pressed, this, &AOverseerPawn::OnResetPressed);
	PlayerInputComponent->BindKey(EKeys::Tab, IE_Pressed, this, &AOverseerPawn::OnTakeControlPressed);
	PlayerInputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &AOverseerPawn::OnZoomIn);
	PlayerInputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &AOverseerPawn::OnZoomOut);
}

void AOverseerPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (APlayerController* PC = GetPlayerController())
	{
		const auto Down = [PC](const FKey& A, const FKey& B) { return PC->IsInputKeyDown(A) || PC->IsInputKeyDown(B); };
		const double Right = (Down(EKeys::D, EKeys::Right) ? 1.0 : 0.0) - (Down(EKeys::A, EKeys::Left) ? 1.0 : 0.0);
		const double Forward = (Down(EKeys::W, EKeys::Up) ? 1.0 : 0.0) - (Down(EKeys::S, EKeys::Down) ? 1.0 : 0.0);
		if (Right != 0.0 || Forward != 0.0)
		{
			Pan(Right, Forward, DeltaTime, Down(EKeys::LeftShift, EKeys::RightShift));
		}
		const float Turn = (PC->IsInputKeyDown(EKeys::E) ? 1.0f : 0.0f) - (PC->IsInputKeyDown(EKeys::Q) ? 1.0f : 0.0f);
		if (Turn != 0.0f)
		{
			Rotate(Turn * OverseerKeyTurnSpeed * DeltaTime, 0.0f);
		}

		// Middle button: turn and tilt by how far the cursor moved since the last frame.
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		if (PC->GetMousePosition(MouseX, MouseY))
		{
			const FVector2D Mouse(MouseX, MouseY);
			if (bHadMouse && PC->IsInputKeyDown(EKeys::MiddleMouseButton))
			{
				const FVector2D Delta = Mouse - LastMouse;
				Rotate(static_cast<float>(Delta.X) * OverseerMouseTurn, static_cast<float>(Delta.Y) * OverseerMouseTurn);
			}
			LastMouse = Mouse;
			bHadMouse = true;
		}
		else
		{
			bHadMouse = false;
		}
	}

	Distance = FMath::FInterpTo(Distance, TargetDistance, DeltaTime, OverseerZoomEase);
	KeepFocusOnMap();
	PlaceBody();
}

void AOverseerPawn::Pan(double Right, double Forward, float DeltaTime, bool bFast)
{
	// Faster when zoomed out, so crossing the map takes about the same time from any height.
	const double Speed = (static_cast<double>(Distance) * 0.8 + 500.0) * (bFast ? 3.0 : 1.0);
	const FRotator Heading(0.0, Yaw, 0.0);
	const FVector Ahead = Heading.Vector();
	const FVector Side = FRotationMatrix(Heading).GetScaledAxis(EAxis::Y);
	Focus += (Ahead * Forward + Side * Right).GetSafeNormal() * Speed * DeltaTime;
	KeepFocusOnMap();
}

void AOverseerPawn::Rotate(float YawDegrees, float TiltDegrees)
{
	Yaw = FRotator::NormalizeAxis(Yaw + YawDegrees);
	Tilt = FMath::Clamp(Tilt + TiltDegrees, kMinTilt, kMaxTilt);
}

void AOverseerPawn::Zoom(float Notches)
{
	TargetDistance = FMath::Clamp(TargetDistance * FMath::Pow(OverseerZoomPerNotch, Notches), kMinDistance, kMaxDistance);
}

void AOverseerPawn::FocusOn(const FVector& WorldLocation)
{
	Focus = WorldLocation;
	KeepFocusOnMap();
}

void AOverseerPawn::ResetView()
{
	TargetDistance = OverseerDefaultDistance;
	Tilt = OverseerDefaultTilt;
	Yaw = 0.0f;
}

void AOverseerPawn::SnapCamera()
{
	Distance = TargetDistance;
	KeepFocusOnMap();
	PlaceBody();
}

// The focus stays on the map and rides the ground's height.
void AOverseerPawn::KeepFocusOnMap()
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	const colony::Map& Map = Bridge->GetSim()->map();
	const FVector Origin = Bridge->GetWorldOrigin();
	const float XMetres = FMath::Clamp(static_cast<float>((Focus.X - Origin.X) / 100.0), 0.0f, static_cast<float>(Map.width() - 1));
	const float YMetres = FMath::Clamp(static_cast<float>((Focus.Y - Origin.Y) / 100.0), 0.0f, static_cast<float>(Map.height() - 1));
	Focus = Bridge->SimToWorld(XMetres, YMetres, 0);
}

void AOverseerPawn::PlaceBody()
{
	const FRotator Look(-Tilt, Yaw, 0.0);
	FVector Location = Focus - Look.Vector() * Distance;
	// Never dip under the ground on a hillside.
	if (const USimSubsystem* Bridge = GetBridge(); Bridge && Bridge->GetSim())
	{
		const FVector Local = (Location - Bridge->GetWorldOrigin()) / 100.0;
		const colony::Map& Map = Bridge->GetSim()->map();
		if (Local.X >= 0.0 && Local.Y >= 0.0 && Local.X < Map.width() && Local.Y < Map.height())
		{
			const double GroundZ = Bridge->SimToWorld(static_cast<float>(Local.X), static_cast<float>(Local.Y), 0).Z;
			// Rock stands as a block on the ground, and a mined-out tunnel keeps the mountain's roof over it.
			const colony::CellPos Cell{FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), 0};
			const double Block = Map.roof(Cell) == colony::Roof::Thick ? TerrainDraw::RockHeightM * 100.0 : 0.0;
			Location.Z = FMath::Max(Location.Z, GroundZ + Block + 150.0);
		}
	}
	SetActorLocationAndRotation(Location, Look);
}

FSimPick AOverseerPawn::PickAtScreenPosition(const FVector2D& ScreenPosition) const
{
	const USimSubsystem* Bridge = GetBridge();
	const APlayerController* PC = GetPlayerController();
	FVector Origin;
	FVector Direction;
	if (!Bridge || !PC || !PC->DeprojectScreenPositionToWorld(static_cast<float>(ScreenPosition.X), static_cast<float>(ScreenPosition.Y), Origin, Direction))
	{
		return FSimPick();
	}
	return SimPicking::PickFromRay(*Bridge, Origin, Direction);
}

FSimPick AOverseerPawn::SelectAtScreenPosition(const FVector2D& ScreenPosition)
{
	const FSimPick Pick = PickAtScreenPosition(ScreenPosition);
	if (UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>())
	{
		Overseer->Select(Pick);
	}
	return Pick;
}

void AOverseerPawn::OnSelectPressed()
{
	float MouseX = 0.0f;
	float MouseY = 0.0f;
	if (const APlayerController* PC = GetPlayerController(); PC && PC->GetMousePosition(MouseX, MouseY))
	{
		// A tool of the architect menu in hand takes the click; else it selects.
		UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>();
		if (!Orders || !Orders->HandleLeftPressed(FVector2D(MouseX, MouseY)))
		{
			SelectAtScreenPosition(FVector2D(MouseX, MouseY));
		}
	}
}

void AOverseerPawn::OnSelectReleased()
{
	float MouseX = 0.0f;
	float MouseY = 0.0f;
	if (const APlayerController* PC = GetPlayerController(); PC && PC->GetMousePosition(MouseX, MouseY))
	{
		if (UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>())
		{
			Orders->HandleLeftReleased(FVector2D(MouseX, MouseY));
		}
	}
}

void AOverseerPawn::OnOrderPressed()
{
	float MouseX = 0.0f;
	float MouseY = 0.0f;
	if (const APlayerController* PC = GetPlayerController(); PC && PC->GetMousePosition(MouseX, MouseY))
	{
		if (UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>())
		{
			Orders->HandleRightClick(FVector2D(MouseX, MouseY));
		}
	}
}

void AOverseerPawn::OnRotateOrDraftPressed()
{
	if (UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>())
	{
		Orders->HandleRotateOrDraftKey();
	}
}

void AOverseerPawn::OnClearPressed()
{
	// Esc puts down a tool first; only with none in hand does it clear the selection.
	if (UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>(); Orders && Orders->CancelTool())
	{
		return;
	}
	if (UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>())
	{
		Overseer->ClearSelection();
	}
}

void AOverseerPawn::OnFocusPressed()
{
	if (UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>())
	{
		Overseer->FocusOnSelection();
	}
}

void AOverseerPawn::OnResetPressed()
{
	ResetView();
}

void AOverseerPawn::OnTakeControlPressed()
{
	if (UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>())
	{
		Protagonist->ToggleMode();
	}
}
