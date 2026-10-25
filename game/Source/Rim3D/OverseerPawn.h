#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"

#include "SimPicking.h"

#include "OverseerPawn.generated.h"

class UCameraComponent;
class USimSubsystem;

/**
 * The overseer's free top-down camera (RimWorld's view, in 3D). It looks at a focus point on the
 * sim's ground from a distance, yaw and pitch; the pawn itself is only the camera body.
 *
 * Keys: W A S D or the arrows pan (Shift is faster), Q and E turn, the mouse wheel zooms, holding
 * the middle button and moving the mouse turns and tilts, Home resets the view, F centres on the
 * selection, Esc clears the selection, the left button selects (a pawn, thing or ground cell).
 * The methods below are what the keys call, so tests can drive the camera without a keyboard.
 */
UCLASS()
class RIM3D_API AOverseerPawn : public APawn
{
	GENERATED_BODY()

public:
	AOverseerPawn();

	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	/** Moves the focus. Right and Forward are -1 to 1 along the camera's own axes on the ground. */
	void Pan(double Right, double Forward, float DeltaTime, bool bFast = false);
	/** Turns the camera around the focus (degrees) and tilts it (positive looks more downward). */
	void Rotate(float YawDegrees, float TiltDegrees);
	/** Zooms by mouse-wheel notches; positive zooms in. Eases to the new distance. */
	void Zoom(float Notches);
	/** Centres on a world location (Unreal units). */
	void FocusOn(const FVector& WorldLocation);
	/** Back to the default distance, tilt and yaw, keeping the focus. */
	void ResetView();
	/** Jumps to where the camera is easing to, and puts the body there now. */
	void SnapCamera();

	/** What is under a screen point (viewport pixels). */
	FSimPick PickAtScreenPosition(const FVector2D& ScreenPosition) const;
	/** Picks under a screen point and selects the result (clears the selection if nothing is there). */
	FSimPick SelectAtScreenPosition(const FVector2D& ScreenPosition);

	const FVector& GetFocus() const { return Focus; }
	float GetDistance() const { return Distance; }
	float GetTargetDistance() const { return TargetDistance; }
	float GetYaw() const { return Yaw; }
	float GetTilt() const { return Tilt; }

	static constexpr float kMinDistance = 600.0f;
	static constexpr float kMaxDistance = 30000.0f;
	static constexpr float kMinTilt = 25.0f;
	static constexpr float kMaxTilt = 85.0f;

private:
	void OnSelectPressed();
	void OnClearPressed();
	void OnFocusPressed();
	void OnResetPressed();
	void OnTakeControlPressed();
	void OnZoomIn() { Zoom(1.0f); }
	void OnZoomOut() { Zoom(-1.0f); }

	USimSubsystem* GetBridge() const;
	APlayerController* GetPlayerController() const;
	void KeepFocusOnMap();
	void PlaceBody();

	UPROPERTY(VisibleAnywhere, Category = "Overseer")
	TObjectPtr<UCameraComponent> Camera;

	FVector Focus = FVector::ZeroVector;
	float Distance = 4500.0f;
	float TargetDistance = 4500.0f;
	float Yaw = 0.0f;
	float Tilt = 55.0f;
	FVector2D LastMouse = FVector2D::ZeroVector;
	bool bHadMouse = false;
};
