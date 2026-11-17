#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ShuttleSubsystem.generated.h"

class AActor;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UMaterialParameterCollection;
class UPointLightComponent;
class USceneComponent;
class USimSubsystem;
class UStaticMesh;
class UStaticMeshComponent;
namespace colony { struct ShuttleView; }

/** How a shuttle is posed this frame, worked out from its phase and progress (smoke test `shuttle`). */
struct FShuttlePose
{
	/** Metres back along its heading from the landing spot (negative: past it, flying off), and up from the ground. */
	float BackM = 0.0f;
	float UpM = 0.0f;
	/** Nose up (degrees) and a little roll, for braking and hovering. */
	float PitchDeg = 0.0f;
	float RollDeg = 0.0f;
	/** 0 retracted, 1 down. */
	float Gear = 1.0f;
	/** Rear engines: 0 pointing back (forward flight), 1 pointing down (hover). */
	float Nacelles = 1.0f;
	/** Lift vanes, degrees fore (+) or aft. */
	float VaneDeg = 0.0f;
	/** How hard the belly and rear jets burn, 0 to 1. */
	float Lift = 0.0f;
	float Rear = 0.0f;
	/** 0 shut, 1 open (door slid back, step down). */
	float Door = 0.0f;
};

/**
 * The VTOL shuttle on screen (docs/design/shuttle-draw.md). The sim says where a shuttle stands, which way it faces, its phase
 * (descending, landed, ascending) and how far through the phase it is (Snapshot::shuttles); this draws the rest, all of it
 * cosmetic: the flight in from a long way off in forward flight, the rear engines swivelling down to hover, the vertical
 * descent with the gear coming down, the door sliding open once the jets spool down, and the reverse on the way out. The jets
 * burn with flames, light the ground and blow dust out from under the shuttle, and the grass around leans away from it
 * (MPC_Downwash in the plant material).
 *
 * The model is the Codex shuttle imported by the GunArt commandlet (-List=assets/shuttle.json), one static mesh per moving
 * part; its pivots and the other points are in assets/shuttle.json, read at start and on F6. Without the model the def's
 * box parts are drawn. The building the shuttle stands as is not drawn by the building subsystem (visual.drawnBy).
 * In the overseer view, with roofs hidden, a landed shuttle's roof is hidden too so its cabin shows.
 *
 * Console: Shuttle.Enable (0 draws nothing), Shuttle.Dust (0 no dust), Shuttle.Debug (1 logs each shuttle's pose once a second).
 */
UCLASS()
class RIM3D_API UShuttleSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Reads assets/shuttle.json again and remakes the shuttles (also on F6). */
	void ReloadConfig();

	// --- For the smoke test ---
	int32 GetShuttlesDrawn() const { return Shuttles.Num(); }
	/** The pose of a shuttle drawn now, by sim id; false if there is none. */
	bool GetPose(uint32 Id, FShuttlePose& Out) const;
	/** Where a drawn shuttle's actor is (world cm); false if there is none. */
	bool GetLocation(uint32 Id, FVector& Out) const;
	bool HasArt() const { return bHasArt; }
	int32 GetFlamesDrawn() const { return FlamesDrawn; }
	int32 GetDustDrawn() const { return Dust.Num(); }
	/** The strongest grass push written this frame (0 to 1). */
	float GetWashStrength() const { return WashStrength; }
	/** The pose for a phase (0 descending, 1 landed, 2 ascending) and progress, with `LandedSeconds` of game time since it set down. */
	FShuttlePose PoseAt(int32 Phase, float Progress, float LandedSeconds, float HoverSeconds) const;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FPart
	{
		FString Name;
		FVector PivotCm = FVector::ZeroVector;
		TObjectPtr<USceneComponent> Pivot = nullptr;
		TObjectPtr<UStaticMeshComponent> Mesh = nullptr;
	};

	struct FShuttle
	{
		TObjectPtr<AActor> Actor = nullptr;
		TObjectPtr<USceneComponent> Model = nullptr;
		TObjectPtr<UPointLightComponent> Light = nullptr;
		TArray<FPart> Parts;
		int32 Phase = -1;
		float LandedSeconds = 0.0f;
		float HoverSeconds = 0.0f;
		float FlickerSeed = 0.0f;
		FShuttlePose Pose;
		bool bSeen = false;
	};

	struct FDustPuff
	{
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Age = 0.0f;
		float Life = 1.0f;
		float SizeCm = 50.0f;
		float Spin = 0.0f;
	};

	struct FConfig
	{
		FString Art;
		FString ArtRoot;
		FVector ModelCentre = FVector::ZeroVector;
		float GroundLift = 0.0f;
		TArray<TPair<FString, FVector>> Parts;
		TArray<FVector> LiftJets;
		TArray<FVector> RearJets;
		TMap<FString, FRotator> GearUp;
		FRotator NacelleHover = FRotator(90.0f, 0.0f, 0.0f);
		float VaneTilt = 15.0f;
		FVector DoorOpen = FVector(-140.0f, 8.0f, 0.0f);
		FRotator StepStowed = FRotator(0.0f, 0.0f, -85.0f);
		float ApproachM = 160.0f, CruiseM = 45.0f, HoverM = 22.0f, FlareDeg = 8.0f, BobCm = 6.0f, WobbleDeg = 0.8f;
		float DoorAfterS = 2.0f, SpoolDownS = 2.5f;
		float LiftLengthCm = 150.0f, LiftRadiusCm = 22.0f, RearLengthCm = 380.0f, RearRadiusCm = 46.0f;
		FLinearColor CoreColor = FLinearColor(0.55f, 0.75f, 1.0f);
		FLinearColor OuterColor = FLinearColor(1.0f, 0.55f, 0.25f);
		float Brightness = 12.0f, LightCandelas = 400.0f, LightRadiusCm = 2500.0f;
		float DustFromM = 18.0f, DustPerSecond = 90.0f, DustLife = 2.4f, DustSpeed = 900.0f, DustSize = 90.0f, DustGrow = 260.0f, DustOpacity = 0.35f;
		FLinearColor DustColor = FLinearColor(0.42f, 0.37f, 0.3f);
		int32 DustMax = 600;
		float GrassRadiusM = 14.0f, GrassFromM = 20.0f;
	};

	void ReadConfig();
	void EnsureScene();
	void MakeShuttle(FShuttle& S, const USimSubsystem& Bridge, uint32 DefValue);
	void DestroyShuttle(FShuttle& S);
	void ClearAll();
	void PoseShuttle(FShuttle& S, const colony::ShuttleView& View, float Progress, const USimSubsystem& Bridge, float GameDelta, float RealTime);
	void EmitDust(const FShuttle& S, const USimSubsystem& Bridge, float GameDelta, float Strength);
	void DrawEffects(float GameDelta, float RealTime);
	FVector PartPivot(const FShuttle& S, const TCHAR* Name) const;

	TWeakObjectPtr<USimSubsystem> Bridge;
	FConfig Config;
	bool bConfigRead = false;
	bool bHasArt = false;
	int32 BuiltGeneration = -1;
	TMap<uint32, FShuttle> Shuttles;

	UPROPERTY(Transient)
	TObjectPtr<AActor> Holder;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> FlameCore;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> FlameOuter;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> DustMesh;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> Downwash;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> Keep;

	TArray<FDustPuff> Dust;
	float DustCarry = 0.0f;
	int32 FlamesDrawn = 0;
	float WashStrength = 0.0f;
	float RealClock = 0.0f;
	float DebugClock = 0.0f;
	uint32 RandomState = 12345u;
};
