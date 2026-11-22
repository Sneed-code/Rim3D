#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "TreeFallSubsystem.generated.h"

class AActor;
class USimSubsystem;
class UStaticMeshComponent;
namespace colony { struct Event; }

/**
 * Falling trees (docs/design/structure-damage.md "Falling trees"), all cosmetic. When the sim takes a tree down (a
 * PlantRemoved "felled" by a woodcutter, or "broken" by blows, shots or a blast, with the way it falls in (x, y)) the
 * tree does not just vanish from its chunk: a copy of it, the same mesh, size and turn, takes its place and topples over.
 *
 * The fall is a rigid pole hinged at its foot (angular acceleration 3 g sin(angle) / 2 x height): it leans slowly at
 * first, then faster and faster, the way a real felled tree goes. It stops where its trunk or crown meets the ground,
 * a wall or anything else standing there (the map's heights and building heights along the way), bounces a little
 * and settles, with a crash and dust where the crown lands. It lies there a while, then sinks slowly into the ground and
 * is gone (the wood lies there as the sim's logs). Nothing snaps: every move is continuous, and it never pops back up.
 *
 * Paused game: the fall waits. A new game or a load clears every falling tree. Console: TreeFall.Enabled,
 * TreeFall.LieSeconds, TreeFall.SinkSeconds, TreeFall.Restitution, TreeFall.CrownRest, TreeFall.Max.
 */
UCLASS()
class RIM3D_API UTreeFallSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& InCollection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// For the smoke test.
	int32 GetFalling() const;
	int32 GetFallsStarted() const { return FallsStarted; }
	int32 GetFallsLanded() const { return FallsLanded; }
	int32 GetFallsGone() const { return FallsGone; }
	/** The largest jump in the lean of any tree between two frames, degrees (a snap shows as a big one). */
	float GetBiggestStepDegrees() const { return BiggestStepDeg; }
	/** Seconds the last tree to land took from the first give to the ground; 0 before any has landed. */
	float GetLastFallSeconds() const { return LastFallSeconds; }
	/** For the smoke test: plays a PlantRemoved event as if the sim had sent it (a tree falls where none stood). */
	void PlayEventForTest(const colony::Event& Event);

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	enum class EPhase : uint8 { Falling, Lying, Sinking };

	struct FFall
	{
		TWeakObjectPtr<UStaticMeshComponent> Tree;
		/** The standing tree's transform (world) and where it is hinged (its foot). */
		FTransform Standing;
		FVector Foot = FVector::ZeroVector;
		/** The way it falls (unit, horizontal) and the axis it turns round. */
		FVector Dir = FVector::XAxisVector;
		FVector Axis = FVector::YAxisVector;
		float HeightM = 10.0f;
		float CrownCm = 200.0f;
		/** Lean from upright (radians), its rate, and the lean at which it meets the ground. */
		float Angle = 0.0f;
		float Rate = 0.0f;
		float RestAngle = 1.5f;
		float Age = 0.0f;
		float Sink = 0.0f;
		int32 Bounces = 0;
		EPhase Phase = EPhase::Falling;
	};

	void OnSimEvent(const colony::Event& Event);
	void EnsureScene();
	void Teardown();
	/** The lean (radians) at which the trunk or crown first meets the ground or something standing on it. */
	float FindRestAngle(const FFall& Fall) const;
	void Place(FFall& Fall) const;
	void Land(FFall& Fall);

	TWeakObjectPtr<USimSubsystem> Bridge;
	FDelegateHandle EventHandle;
	UPROPERTY()
	TObjectPtr<AActor> Holder;
	TArray<FFall> Falls;
	FRandomStream Random{0x54524545u};
	int32 BuiltGeneration = -1;
	int32 FallsStarted = 0;
	int32 FallsLanded = 0;
	int32 FallsGone = 0;
	float BiggestStepDeg = 0.0f;
	float LastFallSeconds = 0.0f;
};
