#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "BloodSubsystem.generated.h"

class AActor;
class UDecalComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPrimitiveComponent;
class USimSubsystem;
class USkeletalMeshComponent;
class USkinBlood;
class UStaticMesh;
namespace colony { class Sim; }

/** What made a wound; it decides the shape of the spray and of the mark (docs/design/blood.md). */
enum class EBloodWound : uint8
{
	Auto,     // work it out from the sim's PawnInjured for that pawn (else a bullet or a blunt blow)
	Gunshot,
	Cut,
	Stab,
	Blunt,
	Bite,
	Blast,
	Burn,     // burns and frostbite: no blood
};

/** A body's fluid: its colour fresh and dry (the Filth def's `look`). */
struct FBloodFluid
{
	FLinearColor Wet = FLinearColor(0.30f, 0.012f, 0.010f);
	FLinearColor Dry = FLinearColor(0.075f, 0.012f, 0.010f);
	/** The Filth def it is (Blood, InsectBlood, MachineFluid); invalid when not from a def. */
	colony::DefId Filth;
};

/** One hit on a body, in Unreal units. Whoever draws the blow (a bullet's arrival, a melee swing, the creator's
 *  demo, the gun range) fills one in and calls UBloodSubsystem::AddHit. */
struct FBloodHit
{
	/** Where the blow met the skin (world cm). */
	FVector At = FVector::ZeroVector;
	/** The way the bullet or blade was travelling (any length; zero: straight down). */
	FVector Direction = FVector::ForwardVector;
	EBloodWound Kind = EBloodWound::Auto;
	/** What Auto falls back to when the sim reported no injury (a bullet: Gunshot; a swing: Blunt). */
	EBloodWound Fallback = EBloodWound::Blunt;
	/** Hit points of damage after armour (the sim's `amount`). */
	float Damage = 10.0f;
	/** How hard, 0.2 to 2.5; below 0: worked out from Damage and Kind. */
	float Force = -1.0f;
	/** The sim pawn hit, if there is one: its fluid, its wounds bleeding on and its pool come from the sim. */
	colony::Entity Victim = colony::kNoEntity;
	/** Body part index in the pawn's Bodies def (the sim's `part`), -1 if not known. */
	int32 Part = -1;
	/** The mesh that takes wound and stain decals (the pawn's GetDecalMesh), and the actor the drops ignore. */
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	/** The fluid; when false it comes from the victim's def (red without one). */
	bool bHasFluid = false;
	FBloodFluid Fluid;
	/** Half the body's thickness along the shot (cm) for where a bullet comes out; <= 0: from the height of the hit. */
	float BodyHalfThicknessCm = -1.0f;
	/** A hit from outside the sim (creator demo, gun range targets) bleeds this many seconds; 0: 45 seconds. */
	float BleedSecondsWithoutSim = 0.0f;
};

/**
 * Blood on screen (docs/design/blood.md). Cosmetic: the sim decides who is hurt, how badly, which wounds bleed and
 * where blood filth lies; this draws it.
 *
 * - Spray: drops (stretched along their flight, lit and glossy) thrown along the blow, scaled by its force and kind:
 *   a bullet's small back-spatter at the way in and a big cone out of the far side, a blade's cast-off fan, a blunt
 *   blow's short burst, a blast's all-round spray; a fine mist that hangs a moment; a few bits for heavy hits.
 * - Drops fly, slow in the air (small ones sooner), fall and splash where they land: ground, walls, things or another
 *   body. A splat's shape follows the angle it came in at: round straight down, long with a tail at a slant.
 * - Wounds: a hole, a bigger torn exit, a gash along the blade, stuck to the bone so they move with the body.
 * - Bleeding: while the sim says a wound bleeds, it drips (faster for a worse wound), runs down the skin, spurts with
 *   the heartbeat when it is very bad, and a downed or dead body bleeds a pool that spreads.
 * - Stains stay (BG3-style): they dry from wet red to dark brown over about 5 minutes at 1x, and go when rain
 *   washes them outdoors or a colonist cleans the cell; stains on a body wash off in the rain.
 * - The sim's filth (blood, dirt, vomit, ash, animal filth) is drawn as stains on its cells.
 * - Each body's own fluid: red blood, yellow insect blood, blue machine fluid (Filth defs' `look`).
 *
 * Needs the assets the BloodArt commandlet makes (UnrealEditor-Cmd.exe Rim3D.uproject -run=BloodArt); without them
 * IsReady is false and UFightSubsystem draws its old simple blood.
 *
 * Console: Blood.Enabled, Blood.Quality (0 low, 1 normal, 2 high), Blood.MaxDrops, Blood.MaxStains, Blood.DryMinutes,
 * Blood.Filth, Blood.DropScale, Blood.SplatScale, Blood.GroundShade, Blood.FlipU / Blood.FlipV (decal image axes), Blood.Debug.
 */
UCLASS()
class RIM3D_API UBloodSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Blood is on and its assets loaded. */
	bool IsReady();

	/** A blow on a body. Drawn at the next tick, once the sim's injury events of this step are in. */
	void AddHit(const FBloodHit& Hit);

	/** Wipes every drop, stain, pool and bleeding wound (the creator's Clean up). */
	void CleanAll();

	/** A pawn def's fluid (its Filth def's `look`), red if it has none. */
	static FBloodFluid FluidForPawnDef(const colony::Sim* Sim, colony::DefId PawnDef);
	/** A Filth def's look; false if it has none. */
	static bool FluidForFilthDef(const colony::Sim* Sim, colony::DefId FilthDef, FBloodFluid& Out, bool* bOutFluid = nullptr);
	/** Force from damage and kind, 0.2 to 2.5 (1 is a typical rifle hit). */
	static float ForceFor(EBloodWound Kind, float Damage);
	/** The kind of blood for a sim Hediffs def name (Cut, Gunshot, Crush...). */
	static EBloodWound KindForInjury(const FString& HediffName);

	// --- What has happened, for smoke tests and the debug line ---
	int32 GetHits() const { return Hits; }
	int32 GetDropsSpawned() const { return DropsSpawned; }
	int32 GetMistSpawned() const { return MistSpawned; }
	int32 GetSpraySpawned() const { return SpraySpawned; }
	int32 GetSprayInAir() const { return Spray.Num(); }
	int32 GetActiveDrops() const { return Drops.Num(); }
	int32 GetActiveMist() const { return Mist.Num(); }
	int32 GetSplatsMade() const { return SplatsMade; }
	int32 GetSplatsMerged() const { return SplatsMerged; }
	int32 GetWallSplats() const { return WallSplats; }
	int32 GetBodyStainsMade() const { return BodyStainsMade; }
	int32 GetWoundMarks() const { return WoundMarks; }
	int32 GetExitsTraced() const { return ExitsTraced; }
	/** Marks painted into skin (USkinBlood) and bodies carrying blood in their skin now. */
	int32 GetSkinPaints() const;
	int32 GetSkinMasks() const;
	/** Blood painted into skin; null before Initialize. */
	USkinBlood* GetSkinBlood() const;
	int32 GetExitWounds() const { return ExitWounds; }
	int32 GetDrips() const { return Drips; }
	int32 GetSpurts() const { return Spurts; }
	int32 GetPools() const { return PoolsMade; }
	int32 GetWashed() const { return Washed; }
	int32 GetBleedingWounds() const { return Bleeds.Num(); }
	int32 GetLiveStains() const { return Stains.Num(); }
	int32 GetLiveBodyStains() const { return BodyStains.Num(); }
	int32 GetFilthDecals() const { return FilthDecals.Num(); }
	int32 GetHitsOfKind(EBloodWound Kind) const { return HitsByKind[static_cast<int32>(Kind)]; }
	/** Splats seen of a colour: hits whose fluid was this Filth def. */
	int32 GetHitsWithFluid(colony::DefId Filth) const { const int32* Count = HitsByFluid.Find(Filth.value); return Count ? *Count : 0; }
	bool AreAssetsMissing() const { return bAssetsMissing; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	enum class EDropKind : uint8 { Drop, Chunk };

	struct FDrop
	{
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float RadiusCm = 0.2f;
		float Age = 0.0f;
		float Life = 3.0f;
		float GroundZ = -1.0e9f;  // lands here if it is not traced
		FLinearColor Color;
		FBloodFluid Fluid;
		TWeakObjectPtr<AActor> Ignore;
		EDropKind Kind = EDropKind::Drop;
		bool bTraced = false;     // traced against the world every frame (the bigger drops, within the budget)
		bool bSplats = true;      // leaves a mark where it lands
	};

	struct FMistPuff
	{
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float RadiusCm = 10.0f;
		float Grow = 2.5f;
		float Age = 0.0f;
		float Life = 0.6f;
		float Opacity = 0.5f;
		FLinearColor Color;
	};

	/** A gush of blood in the air (sneed 2026-11-20: big spray you can see on every melee hit and gunshot): a ragged
	 *  streak drawn along its flight, facing the camera, gone in a fraction of a second. Cosmetic. */
	struct FSprayStreak
	{
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Length = 20.0f;  // cm
		float Width = 6.0f;
		float Grow = 1.6f;     // size at the end of its life against the start
		float Age = 0.0f;
		float Life = 0.25f;
		float Opacity = 0.9f;
		float Seed = 0.0f;
		FLinearColor Color;
	};

	/** A stain on the world: a splat, a pool, a run on a wall. */
	struct FStain
	{
		TWeakObjectPtr<UDecalComponent> Decal;
		FVector Position = FVector::ZeroVector;
		FVector Normal = FVector::UpVector;
		float RadiusCm = 2.0f;
		int32 Serial = 0;
		float WetSince = 0.0f;      // the clock (game-speed seconds) when it was last wet
		float LastVisit = 0.0f;
		int64 LastVisitStep = -1;
		float WashLeft = 1.0f;      // 1 fresh .. 0 washed away
		float Fade = 1.0f;          // opacity while going
		FBloodFluid Fluid;
		int32 Cell = -1;            // sim cell index on the ground level, -1 without a sim
		bool bOutdoor = true;
		bool bPool = false;
		bool bGoing = false;        // being cleaned or washed off: fades out
		float LastColorAge = -100.0f;
	};

	/** A stain on a body: a wound, a splash, a run. */
	struct FBodyStain
	{
		TWeakObjectPtr<UDecalComponent> Decal;
		TWeakObjectPtr<USkeletalMeshComponent> Mesh;
		colony::Entity Owner = colony::kNoEntity;
		FBloodFluid Fluid;
		float WetSince = 0.0f;
		float LastVisit = 0.0f;
		float WashLeft = 1.0f;
		bool bWound = false;
		bool bGoing = false;
		float Fade = 1.0f;
		float LastColorAge = -100.0f;
	};

	/** A wound that bleeds over time, stuck to a bone. */
	struct FBleed
	{
		colony::Entity Victim = colony::kNoEntity;
		TWeakObjectPtr<USkeletalMeshComponent> Mesh;
		FName Bone;
		FVector LocalPosition = FVector::ZeroVector;  // in the bone's space
		FVector LocalOutward = FVector::ForwardVector;
		int32 Part = -1;
		EBloodWound Kind = EBloodWound::Cut;
		float Force = 1.0f;
		FBloodFluid Fluid;
		float Age = 0.0f;
		float DripIn = 0.5f;
		float SpurtIn = 0.3f;
		bool bArterial = false;
		float RunLengthCm = 0.0f;
		TWeakObjectPtr<UDecalComponent> Run;
		float SkinRunPainted = 0.0f;  // how long the run painted into the skin is (cm)
		float SkinSeed = -1.0f;
		float NoSimSeconds = 0.0f;   // > 0: bleeds this long without a sim
		float Rate = 0.0f;           // drips a second now
		float NotBleedingFor = 0.0f;
	};

	/** A pool under a downed or dead body. */
	struct FPool
	{
		colony::Entity Victim = colony::kNoEntity;
		int32 Stain = -1;  // its FStain's serial
		float RadiusCm = 0.0f;
		float MaxRadiusCm = 60.0f;
		FVector Centre = FVector::ZeroVector;
		FVector LastTrail = FVector::ZeroVector;  // where the crawl trail last reached
		bool bLeft = false;                       // the body crawled off it: it stops growing
	};

	/** A pile of the sim's filth drawn on its cell. */
	struct FFilthStain
	{
		TWeakObjectPtr<UDecalComponent> Decal;
		int32 Thickness = 0;
		float WetSince = 0.0f;
		FBloodFluid Fluid;
		bool bFluid = false;
		float LastColorAge = -100.0f;
	};

	struct FPendingHit
	{
		FBloodHit Hit;
		int64 Step = 0;
	};

	struct FInjury
	{
		FString Hediff;
		float Amount = 0.0f;
		int64 Step = -1;
		int32 Part = -1;
	};

	void OnSimEvent(const colony::Event& Event);
	bool EnsureAssets();
	UInstancedStaticMeshComponent* MakeInstanced(UStaticMesh* Mesh, UMaterialInterface* Material, int32 CustomFloats);
	void ResetAll();

	void DrawHit(const FBloodHit& Hit);
	void SprayCone(const FVector& At, const FVector& Axis, float ConeDegrees, int32 Count, float MinSpeed, float MaxSpeed,
		float MinRadius, float MaxRadius, const FBloodFluid& Fluid, AActor* Ignore, float GroundZ);
	void SprayMist(const FVector& At, const FVector& Axis, float ConeDegrees, int32 Count, float Speed, float RadiusCm, float Life,
		float Opacity, const FBloodFluid& Fluid);
	void SpawnDrop(const FVector& At, const FVector& Velocity, float RadiusCm, const FBloodFluid& Fluid, AActor* Ignore, float GroundZ,
		EDropKind Kind = EDropKind::Drop);
	void Land(const FDrop& Drop, const FVector& At, const FVector& Normal, UPrimitiveComponent* What);

	/** On the ground or a wall; `bScaled`: drawn Blood.SplatScale times as big (not the sim's filth, already cell sized). */
	UDecalComponent* MakeWorldDecal(int32 Cell, const FVector& At, const FVector& Normal, const FVector& Along, float HalfLength,
		float HalfWidth, const FLinearColor& Color, bool bScaled = true);
	UDecalComponent* MakeBodyDecal(USkeletalMeshComponent* Mesh, int32 Cell, const FVector& At, const FVector& Into, const FVector& Along,
		float HalfLength, float HalfWidth, const FLinearColor& Color);
	int32 AddStain(UDecalComponent* Decal, const FVector& At, const FVector& Normal, float RadiusCm, const FBloodFluid& Fluid, bool bPool);
	void AddBodyStain(UDecalComponent* Decal, USkeletalMeshComponent* Mesh, colony::Entity Owner, const FBloodFluid& Fluid, bool bWound);
	void AddBleed(const FBloodHit& Hit, EBloodWound Kind, float Force, const FBloodFluid& Fluid, const FVector& At, const FVector& Outward);

	void UpdateDrops(float Dt);
	void UpdateMist(float Dt);
	/** Gushes along `Axis` within `ConeDegrees`; `Burst`: one round splash at the wound as well. */
	void SprayGush(const FVector& At, const FVector& Axis, float ConeDegrees, int32 Count, float MinSpeed, float MaxSpeed, float MinLength, float MaxLength,
		bool bBurst, float Strength, const FBloodFluid& Fluid);
	void UpdateSpray(float Dt);
	void UpdateBleeding(float Dt, float GameSeconds);
	void UpdateStains(float Dt, float GameSeconds);
	void SyncFilth();
	void TrimStains();
	void DrawInstances();

	/** Sim cell index on the ground level for a world point, -1 off the map or without a sim. */
	int32 CellOf(const FVector& World) const;
	bool IsOutdoor(int32 Cell) const;
	float RainRate() const;
	void WashCell(int32 Cell, int32 Radius);
	FLinearColor ColorAt(const FBloodFluid& Fluid, float Age) const;
	/** The decal colour: the fluid's colour by age, alpha packing wetness and opacity (see the material). */
	FLinearColor DecalColorFor(const FBloodFluid& Fluid, float Age, float Opacity) const;
	void ApplyCvarsToMaterials();
	float GroundBelow(const FVector& At, AActor* Ignore) const;
	AActor* ActorOf(colony::Entity Pawn) const;

	UPROPERTY()
	TObjectPtr<AActor> Holder;

	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> DropInstances;

	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> ChunkInstances;

	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> MistInstances;
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> SprayInstances;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> SplatterBase;

	/** Blood painted into MakeHuman skin (in place of body decals). */
	UPROPERTY()
	TObjectPtr<USkinBlood> SkinBlood;

	/** One per cell of the splat atlas (4 x 4). */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> CellMaterials;

	TArray<FDrop> Drops;
	TArray<FMistPuff> Mist;
	TArray<FSprayStreak> Spray;
	TArray<FStain> Stains;
	TArray<FBodyStain> BodyStains;
	TArray<FBleed> Bleeds;
	TArray<FPool> Pools;
	TArray<FPendingHit> Pending;
	TMap<uint32, FInjury> Injuries;     // by pawn id: its worst injury of the latest step
	TMap<uint32, int64> SimHitSteps;    // by pawn id: the step the sim last reported a bullet or blow landing on it
	TMap<uint64, FFilthStain> FilthDecals;  // by cell index << 32 | Filth def
	float Clock = 0.0f;                 // game-speed seconds since the start
	FVector LastBlastCentre = FVector::ZeroVector;
	int64 LastBlastStep = -100;

	FRandomStream Random = FRandomStream(0xB100D);
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> BridgeSource;
	int32 LastGeneration = -1;
	uint64 LastFilthChanges = ~0ull;
	bool bFilthSeenOnce = false;
	int64 LastStep = -1;
	bool bAssetsTried = false;
	bool bAssetsMissing = false;
	int32 NextSerial = 1;
	int32 StainCursor = 0;
	int32 TracesThisFrame = 0;
	int32 LastFlips = -1;

	int32 Hits = 0;
	int32 HitsByKind[8] = {};
	TMap<uint32, int32> HitsByFluid;
	int32 DropsSpawned = 0;
	int32 MistSpawned = 0;
	int32 SpraySpawned = 0;
	int32 SkinDropsThisFrame = 0;
	int32 SplatsMade = 0;
	int32 SplatsMerged = 0;
	int32 WallSplats = 0;
	int32 BodyStainsMade = 0;
	int32 WoundMarks = 0;
	int32 ExitWounds = 0;
	int32 ExitsTraced = 0;  // exits found on the drawn body (not guessed)
	int32 Drips = 0;
	int32 Spurts = 0;
	int32 PoolsMade = 0;
	int32 CrawlSmears = 0;
	int32 Washed = 0;
};
