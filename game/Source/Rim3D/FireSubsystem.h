#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include "FireSubsystem.generated.h"

class AActor;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UPointLightComponent;
class UProceduralMeshComponent;
class USimSubsystem;
class UStaticMesh;
namespace colony { class Sim; }

/** What a pooled light is doing now (smoke test `fire`, docs/design/fire-draw.md). */
enum class EFireLightKind : uint8
{
	Lamp,
	Fire,
	Pawn,
};

struct FFireLightSlot
{
	EFireLightKind Kind = EFireLightKind::Lamp;
	FVector Position = FVector::ZeroVector;
	FLinearColor Color = FLinearColor::White;
	float Intensity = 0.0f;
	float RadiusCm = 0.0f;
	bool bShadows = false;
};

/** What the particles are (each has its own mesh and colour). */
enum class EFireParticle : uint8
{
	FlameCore,
	FlameMid,
	FlameTip,
	Ember,
	Smoke,
	Count,
};

/**
 * Light and fire on screen (docs/design/fire-draw.md): what the sim's light sources (E3), fire layer (E4) and burnt ground
 * look like. All of it is cosmetic; the sim decides what burns and what is lit.
 *
 * - **Lamps and torches** shine as real point lights, one for each of the nearest `Fire.MaxLights` of `sim.lights().sources()`
 *   (building, radius, glow), coloured by the def's `glower.color` and raised to `glower.lift` metres. A lamp with a fuel part
 *   (campfire, torch) flickers and has flames on it (`visual.flame` on the def). Out of fuel or power the sim drops it from the
 *   list and the light goes out.
 * - **Fire** in the sim's burning cells: flames (three instanced meshes that go from yellow to orange to red as they rise),
 *   embers that drift up, and smoke that rises and leans with the wind, all in proportion to the fire's size; one orange light
 *   for each 4 m block of fire. Rain and snow shrink the sim's fires and the flames follow. A burning pawn has flames on its body and
 *   a light.
 * - **Burnt ground** (the sim's `burnt` cells) as a charcoal skin over the ground, and the ash filth as grey patches.
 *
 * Particles are instanced meshes on the engine's unlit vertex colour material (flames, embers) and lit one (smoke), moved on the
 * CPU in real time (frozen while the game is paused), like the fight effects, which keeps DLSS possible.
 *
 * Console: Fire.Enable (0 draws no fire, lights or burnt ground), Fire.MaxLights, Fire.MaxParticles, Fire.LampIntensity (scales every
 * light), Fire.Smoke (0 draws no smoke).
 */
UCLASS()
class RIM3D_API UFireSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Brings the burnt ground and ash up to date with the snapshot now (the tick spreads it over frames). */
	void SyncGround();

	bool IsBuilt() const { return Holder != nullptr; }

	// --- For the smoke test and the screen ---

	/** The lights in use now. */
	const TArray<FFireLightSlot>& GetLights() const { return LightSlots; }
	/** How many lamps the sim has lit (its `lights().sources()`), whether or not they got a light of their own. */
	int32 GetLampSources() const { return LampSources; }
	/** Fire cells being drawn (the sim's, smoothed), and the sum of their drawn sizes. */
	int32 GetFireCells() const { return FireCells.Num(); }
	float GetFireSizeSum() const;
	int32 GetBurningPawns() const { return BurningPawns; }
	/** Lamps and torches with flames on them. */
	int32 GetFlameLamps() const { return FlameLamps; }
	int32 GetParticles(EFireParticle Kind) const { return KindCounts[static_cast<int32>(Kind)]; }
	int32 GetParticleTotal() const { return Particles.Num(); }
	int32 GetBurntDrawn() const { return BurntDrawn; }
	int32 GetAshDrawn() const { return AshDrawn; }
	int32 GetGroundChunks() const { return GroundChunksBuilt; }
	bool IsGroundUpToDate() const { return !bGroundChecking; }
	double GetSlowestChunkMs() const { return SlowestChunkMs; }
	/** The centre of the first fire drawn, world cm (zero if none). */
	FVector GetFirstFirePosition() const { return FireCells.Num() > 0 ? FireCells.CreateConstIterator()->Value.Position : FVector::ZeroVector; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FFireCell
	{
		FVector Position = FVector::ZeroVector;
		float Target = 0.0f;
		float Shown = 0.0f;
	};

	/** A lamp the sim has lit, as read at the last light rebuild. */
	struct FLamp
	{
		FVector Position = FVector::ZeroVector;
		/** Where its flames start (the building's ground plus the def's `visual.flame.lift`). */
		FVector FlamePosition = FVector::ZeroVector;
		FLinearColor Color = FLinearColor::White;
		float RadiusCm = 1000.0f;
		float Glow = 1.0f;
		bool bFlicker = false;
		float FlameLiftCm = 0.0f;
		float FlameSize = 0.0f;
		float FlameScale = 1.0f;
		float Seed = 0.0f;
	};

	struct FEmitter
	{
		FVector Position = FVector::ZeroVector;
		/** How far from the position a flame starts, cm, and how high above it (a burning pawn's body). */
		float SpreadCm = 40.0f;
		float HeightSpreadCm = 0.0f;
		/** 0 to 1.75: the fire's size, which sets how many flames, how big, and how much smoke. */
		float Size = 0.0f;
		/** How big a flame is next to a cell fire's (a torch is a small one). */
		float Scale = 1.0f;
		bool bSmoke = true;
	};

	struct FParticle
	{
		EFireParticle Kind = EFireParticle::FlameCore;
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Age = 0.0f;
		float Life = 1.0f;
		float SizeCm = 20.0f;
		float Seed = 0.0f;
	};

	struct FGroundChunk
	{
		UProceduralMeshComponent* Mesh = nullptr;
		uint64 BurntHash = 0;
		uint64 AshHash = 0;
		int32 Burnt = 0;
		int32 Ash = 0;
	};

	void Teardown();
	void EnsureScene();
	UInstancedStaticMeshComponent* MakeInstanced(UStaticMesh* Mesh);
	UStaticMesh* MakeMesh(UMaterialInterface* Material, int32 Shape, float X, float Y, float Z, const FLinearColor& Color);
	void ReadLamps(const colony::Sim& Sim, const USimSubsystem& Bridge);
	void SyncFires(const USimSubsystem& Bridge);
	void GatherEmitters(const USimSubsystem& Bridge, TArray<FEmitter>& Out);
	void Emit(const TArray<FEmitter>& Emitters, float Delta, const FVector& Camera);
	void MoveParticles(float Delta);
	void DrawParticles();
	void UpdateLights(const colony::Sim& Sim, const USimSubsystem& Bridge, float Delta);
	void CheckGround(const USimSubsystem& Bridge, int32 Budget);
	void StartGroundCheck(const USimSubsystem& Bridge);
	bool BuildGroundChunk(const USimSubsystem& Bridge, int32 ChunkX, int32 ChunkY);
	void ResetFire();

	UPROPERTY()
	TObjectPtr<AActor> Holder;

	UPROPERTY()
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> ParticleInstances;

	UPROPERTY()
	TArray<TObjectPtr<UPointLightComponent>> Lights;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMesh>> KeepMeshes;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> LitMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> UnlitMaterial;

	/** Cells (y * 65536 + x) of the burnt ground and of the ash, by chunk, as of the last snapshot. */
	TArray<TArray<int32>> BurntBuckets;
	TArray<TArray<int32>> AshBuckets;
	TMap<int32, FFireCell> FireCells;
	TArray<FLamp> Lamps;
	TArray<FParticle> Particles;
	TArray<FFireLightSlot> LightSlots;
	TArray<FGroundChunk> GroundChunks;
	FRandomStream Random = FRandomStream(0xF14E);
	TWeakObjectPtr<USimSubsystem> Bridge;

	const colony::Sim* BuiltSim = nullptr;
	int32 BuiltGeneration = -1;
	uint64 SeenLightRebuilds = ~0ULL;
	uint64 SeenFireChanges = ~0ULL;
	uint64 SeenBurntChanges = ~0ULL;
	uint64 SeenFilthChanges = ~0ULL;
	int32 LampSources = 0;
	int32 FlameLamps = 0;
	int32 BurningPawns = 0;
	int32 KindCounts[static_cast<int32>(EFireParticle::Count)] = {};
	FVector BuiltOrigin = FVector::ZeroVector;

	int32 GroundChunksX = 0;
	int32 GroundChunksY = 0;
	int32 GroundCursor = 0;
	bool bGroundChecking = false;
	int32 BurntDrawn = 0;
	int32 AshDrawn = 0;
	int32 GroundChunksBuilt = 0;
	double SlowestChunkMs = 0.0;
};
