#include "FireSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

#include "SimSubsystem.h"
#include "TerrainDraw.h"
#include "VisualMesh.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Fire.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Weather.h"

DEFINE_LOG_CATEGORY_STATIC(LogFire, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GFireEnable(
		TEXT("Fire.Enable"), 1, TEXT("1 draws the sim's lamps and fires as lights, flames, embers and smoke, and the burnt ground, 0 draws none."), ECVF_Default);
	TAutoConsoleVariable<int32> GFireMaxLights(
		TEXT("Fire.MaxLights"), 24, TEXT("How many of the nearest lamps, fires and burning pawns get a real light."), ECVF_Default);
	TAutoConsoleVariable<int32> GFireMaxParticles(
		TEXT("Fire.MaxParticles"), 3500, TEXT("Flames, embers and smoke drawn at most."), ECVF_Default);
	TAutoConsoleVariable<float> GFireLampIntensity(
		TEXT("Fire.LampIntensity"), 1.0f, TEXT("Scales the brightness of every light fire and lamps give."), ECVF_Default);
	TAutoConsoleVariable<int32> GFireSmoke(
		TEXT("Fire.Smoke"), 1, TEXT("1 draws smoke over fires, 0 draws none."), ECVF_Default);

	const TCHAR* const FireLitMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");

	// Lights, in candelas (the engine's default exposure puts a clear noon at 10 lux): a lamp at full glow, and a fire.
	constexpr float FireLampCandelas = 40.0f;
	constexpr float FireCellCandelas = 10.0f;
	constexpr float FireCandelasPerSize = 3.5f;
	constexpr float FireCandelasMax = 60.0f;
	constexpr float FirePawnCandelas = 14.0f;
	constexpr int32 FireShadowLights = 4;
	// Fires are blocked into this many metres for their lights.
	constexpr int32 FireLightBlockM = 4;
	// Nothing is made for what is farther than this from the camera, cm.
	constexpr double FireCullCm = 15000.0;

	// Per second, for a fire of size 1 (what a cell's fire starts at is 0.1, and the sim's biggest is 1.75).
	constexpr float FireFlamesPerSecond = 20.0f;
	constexpr float FireEmbersPerSecond = 2.4f;
	constexpr float FireSmokePerSecond = 1.5f;
	constexpr float FireFlameBaseCm = 58.0f;

	// How fast a drawn fire follows the sim's size: size per second.
	constexpr float FireSizeRate = 1.2f;

	// Burnt ground and ash on the ground: lifts in cm.
	constexpr float FireBurntLiftCm = 0.9f;
	constexpr float FireAshLiftCm = 1.2f;
	const FLinearColor FireBurntColor(0.035f, 0.031f, 0.028f);
	const FLinearColor FireAshColor(0.07f, 0.067f, 0.064f);

	int32 FireChunksFor(int32 Cells)
	{
		return (Cells + TerrainDraw::ChunkCells - 1) / TerrainDraw::ChunkCells;
	}

	uint32 FireHash(int32 X, int32 Y)
	{
		uint32 H = static_cast<uint32>(X) * 73856093u ^ static_cast<uint32>(Y) * 19349663u;
		H ^= H >> 13;
		H *= 0x5bd1e995u;
		H ^= H >> 15;
		return H;
	}

	float FireNoise(float T, float Seed)
	{
		return 0.82f + 0.12f * FMath::Sin(T * 13.0f + Seed * 50.0f) + 0.06f * FMath::Sin(T * 31.0f + Seed * 17.0f);
	}
}

bool UFireSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UFireSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFireSubsystem, STATGROUP_Tickables);
}

void UFireSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Bridge = Collection.InitializeDependency<USimSubsystem>();
}

void UFireSubsystem::Deinitialize()
{
	Teardown();
	Super::Deinitialize();
}

float UFireSubsystem::GetFireSizeSum() const
{
	float Sum = 0.0f;
	for (const TPair<int32, FFireCell>& Pair : FireCells)
	{
		Sum += Pair.Value.Shown;
	}
	return Sum;
}

void UFireSubsystem::Teardown()
{
	if (Holder)
	{
		Holder->Destroy();
		Holder = nullptr;
	}
	ParticleInstances.Reset();
	Lights.Reset();
	KeepMeshes.Reset();
	GroundChunks.Reset();
	ResetFire();
	BuiltSim = nullptr;
	BuiltGeneration = -1;
}

void UFireSubsystem::ResetFire()
{
	FireCells.Reset();
	Lamps.Reset();
	Particles.Reset();
	LightSlots.Reset();
	BurntBuckets.Reset();
	AshBuckets.Reset();
	SeenLightRebuilds = ~0ULL;
	SeenFireChanges = ~0ULL;
	SeenBurntChanges = ~0ULL;
	SeenFilthChanges = ~0ULL;
	LampSources = 0;
	FlameLamps = 0;
	BurningPawns = 0;
	FMemory::Memzero(KindCounts);
	bGroundChecking = false;
	GroundCursor = 0;
	BurntDrawn = 0;
	AshDrawn = 0;
	GroundChunksBuilt = 0;
	SlowestChunkMs = 0.0;
}

// ---------------------------------------------------------------------------
// The scene.

UStaticMesh* UFireSubsystem::MakeMesh(UMaterialInterface* Material, int32 Shape, float X, float Y, float Z, const FLinearColor& Color)
{
	VisualMesh::FPart Part;
	Part.Shape = static_cast<VisualMesh::EShape>(Shape);
	Part.Size = FVector3f(X, Y, Z);
	Part.Color = Color;
	TArray<VisualMesh::FPart> Parts;
	Parts.Add(Part);
	UStaticMesh* Mesh = VisualMesh::MakeMesh(this, Material, Parts, VisualMesh::EPartSet::All, nullptr, false);
	if (Mesh)
	{
		KeepMeshes.Add(Mesh);
	}
	return Mesh;
}

UInstancedStaticMeshComponent* UFireSubsystem::MakeInstanced(UStaticMesh* Mesh)
{
	UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(Holder);
	Component->SetStaticMesh(Mesh);
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetGenerateOverlapEvents(false);
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(false);
	Component->bReceivesDecals = false;
	Component->bUseAsOccluder = false;
	Component->SetupAttachment(Holder->GetRootComponent());
	Component->RegisterComponent();
	return Component;
}

void UFireSubsystem::EnsureScene()
{
	if (Holder || !GetWorld())
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("FireRoot"));
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	Holder->SetActorLabel(TEXT("Fire"));
#endif
	LitMaterial = LoadObject<UMaterialInterface>(nullptr, FireLitMaterialPath);
	UnlitMaterial = LitMaterial;
	if (!LitMaterial)
	{
		UE_LOG(LogFire, Error, TEXT("Could not load %s; fire and smoke cannot be drawn"), FireLitMaterialPath);
		return;
	}

	// Cone: 12 cm wide at the base and 20 tall, scaled per flame. Colours are linear.
	const int32 Cone = static_cast<int32>(VisualMesh::EShape::Cone);
	const int32 Box = static_cast<int32>(VisualMesh::EShape::Box);
	const int32 Sphere = static_cast<int32>(VisualMesh::EShape::Sphere);
	ParticleInstances.Reset();
	ParticleInstances.Add(MakeInstanced(MakeMesh(UnlitMaterial, Cone, 6.0f, 6.0f, 20.0f, FLinearColor(0.85f, 0.42f, 0.05f))));
	ParticleInstances.Add(MakeInstanced(MakeMesh(UnlitMaterial, Cone, 6.0f, 6.0f, 20.0f, FLinearColor(0.8f, 0.17f, 0.015f))));
	ParticleInstances.Add(MakeInstanced(MakeMesh(UnlitMaterial, Cone, 6.0f, 6.0f, 20.0f, FLinearColor(0.45f, 0.04f, 0.008f))));
	ParticleInstances.Add(MakeInstanced(MakeMesh(UnlitMaterial, Box, 2.0f, 2.0f, 2.0f, FLinearColor(1.0f, 0.55f, 0.12f))));
	ParticleInstances.Add(MakeInstanced(MakeMesh(LitMaterial, Sphere, 10.0f, 10.0f, 20.0f, FLinearColor(0.006f, 0.006f, 0.007f))));

	Lights.Reset();
	for (int32 I = 0; I < 48; ++I)
	{
		UPointLightComponent* Light = NewObject<UPointLightComponent>(Holder);
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetIntensity(0.0f);
		Light->SetAttenuationRadius(500.0f);
		Light->SetSourceRadius(25.0f);
		Light->SetCastShadows(false);
		Light->SetupAttachment(Root);
		Light->RegisterComponent();
		Light->SetVisibility(false);
		Lights.Add(Light);
	}
}

// ---------------------------------------------------------------------------
// What the sim has.

void UFireSubsystem::ReadLamps(const colony::Sim& Sim, const USimSubsystem& Source)
{
	Lamps.Reset();
	FlameLamps = 0;
	const colony::DefDatabase& Defs = Sim.defs();
	const entt::registry& Registry = Sim.things().registry();
	for (const colony::LightSource& Light : Sim.lights().sources())
	{
		FLamp Lamp;
		Lamp.Glow = Light.glow;
		Lamp.RadiusCm = Light.radius * 100.0f;
		Lamp.Seed = static_cast<float>(FireHash(Light.cell.x, Light.cell.y) & 1023u) / 1023.0f;
		float LiftM = 1.0f;
		if (Registry.valid(Light.building))
		{
			if (const colony::Thing* Thing = Registry.try_get<colony::Thing>(Light.building); Thing && Thing->def.valid() && Thing->def.value < Defs.size())
			{
				const colony::Def& Def = Defs.get(Thing->def);
				Lamp.Color = VisualMesh::ReadColor(Def, "glower.color", FLinearColor(1.0f, 0.75f, 0.45f));
				LiftM = static_cast<float>(Def.number("glower.lift", 1.0));
				// Fuel lamps (campfire, torch) flicker; with a `visual.flame` they have flames.
				Lamp.bFlicker = Def.has("fuel");
				if (Def.has("visual.flame"))
				{
					Lamp.FlameLiftCm = static_cast<float>(Def.number("visual.flame.lift", 0.2)) * 100.0f;
					Lamp.FlameSize = static_cast<float>(Def.number("visual.flame.size", 0.3));
					Lamp.FlameScale = FMath::Clamp(Lamp.FlameSize * 1.4f, 0.2f, 1.0f);
					++FlameLamps;
				}
			}
		}
		const FVector Ground = Source.SimToWorld(Light.cell.x + 0.5f, Light.cell.y + 0.5f, Light.cell.level);
		Lamp.Position = Ground + FVector(0.0, 0.0, LiftM * 100.0);
		Lamp.FlamePosition = Ground + FVector(0.0, 0.0, Lamp.FlameLiftCm);
		Lamps.Add(Lamp);
	}
	LampSources = Lamps.Num();
}

void UFireSubsystem::SyncFires(const USimSubsystem& Source)
{
	const colony::Snapshot& Snapshot = Source.GetCurrentSnapshot();
	if (Snapshot.fireChanges == SeenFireChanges)
	{
		return;
	}
	SeenFireChanges = Snapshot.fireChanges;
	for (TPair<int32, FFireCell>& Pair : FireCells)
	{
		Pair.Value.Target = 0.0f;
	}
	const colony::Sim* Sim = Source.GetSim();
	for (const colony::FireView& Fire : Snapshot.fires)
	{
		const int32 Key = Sim->map().index(Fire.cell);
		FFireCell& Cell = FireCells.FindOrAdd(Key);
		if (Cell.Position.IsZero())
		{
			Cell.Position = Source.SimToWorld(Fire.cell.x + 0.5f, Fire.cell.y + 0.5f, Fire.cell.level) + FVector(0.0, 0.0, 3.0);
			Cell.Shown = FMath::Min(0.12f, Fire.size);
		}
		Cell.Target = Fire.size;
	}
}

void UFireSubsystem::GatherEmitters(const USimSubsystem& Source, TArray<FEmitter>& Out)
{
	for (const TPair<int32, FFireCell>& Pair : FireCells)
	{
		if (Pair.Value.Shown < 0.03f)
		{
			continue;
		}
		FEmitter Emitter;
		Emitter.Position = Pair.Value.Position;
		Emitter.Size = Pair.Value.Shown;
		Emitter.SpreadCm = 42.0f;
		Out.Add(Emitter);
	}
	for (const FLamp& Lamp : Lamps)
	{
		if (Lamp.FlameSize <= 0.0f)
		{
			continue;
		}
		FEmitter Emitter;
		Emitter.Position = Lamp.FlamePosition;
		Emitter.Size = Lamp.FlameSize;
		Emitter.Scale = Lamp.FlameScale;
		Emitter.SpreadCm = FMath::Max(2.5f, Lamp.FlameSize * 22.0f);
		Emitter.bSmoke = Lamp.FlameSize > 0.3f;
		Out.Add(Emitter);
	}
	BurningPawns = 0;
	for (const colony::ThingView& View : Source.GetCurrentSnapshot().things)
	{
		if (!View.onFire || View.category != colony::ThingCategory::Pawn)
		{
			continue;
		}
		FVector Location;
		if (!Source.GetBlendedLocation(View.id, Location))
		{
			continue;
		}
		++BurningPawns;
		FEmitter Emitter;
		Emitter.Position = Location;
		Emitter.Size = 0.6f;
		Emitter.Scale = 0.8f;
		Emitter.SpreadCm = 22.0f;
		Emitter.HeightSpreadCm = 140.0f;
		Out.Add(Emitter);
	}
}

// ---------------------------------------------------------------------------
// Particles.

void UFireSubsystem::Emit(const TArray<FEmitter>& Emitters, float Delta, const FVector& Camera)
{
	if (Delta <= 0.0f)
	{
		return;
	}
	const int32 Max = FMath::Max(0, GFireMaxParticles.GetValueOnGameThread());
	const bool bSmoke = GFireSmoke.GetValueOnGameThread() != 0;
	const USimSubsystem* Source = Bridge.Get();
	const float Wind = Source && Source->GetSim() ? static_cast<float>(Source->GetSim()->weather().wind()) : 1.0f;
	// How many to make this frame from a rate per second: the whole part, and the rest by chance.
	const auto Count = [this, Delta](float PerSecond)
	{
		const float Wanted = PerSecond * Delta;
		int32 Whole = FMath::FloorToInt32(Wanted);
		Whole += Random.FRand() < Wanted - Whole ? 1 : 0;
		return Whole;
	};
	for (const FEmitter& Emitter : Emitters)
	{
		if (Particles.Num() >= Max)
		{
			break;
		}
		if (FVector::DistSquared(Emitter.Position, Camera) > FMath::Square(FireCullCm))
		{
			continue;
		}
		const float Size = Emitter.Size;
		const auto Start = [&]()
		{
			const float Angle = Random.FRand() * 2.0f * PI;
			const float Radius = FMath::Sqrt(Random.FRand()) * Emitter.SpreadCm;
			return Emitter.Position + FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, Random.FRand() * Emitter.HeightSpreadCm);
		};
		for (int32 I = Count(FireFlamesPerSecond * FMath::Pow(Size, 0.7f)); I > 0; --I)
		{
			FParticle Flame;
			Flame.Kind = EFireParticle::FlameCore;
			Flame.Position = Start();
			Flame.Velocity = FVector(Random.FRandRange(-12.0f, 12.0f), Random.FRandRange(-12.0f, 12.0f), Random.FRandRange(80.0f, 140.0f) * (0.6f + 0.4f * Emitter.Scale));
			Flame.Life = Random.FRandRange(0.42f, 0.8f);
			Flame.SizeCm = FireFlameBaseCm * Emitter.Scale * (0.55f + 0.5f * FMath::Sqrt(FMath::Min(Size, 1.75f))) * Random.FRandRange(0.8f, 1.2f);
			Flame.Seed = Random.FRand();
			Particles.Add(Flame);
		}
		for (int32 I = Count(FireEmbersPerSecond * Size * (0.4f + Emitter.Scale)); I > 0; --I)
		{
			FParticle Ember;
			Ember.Kind = EFireParticle::Ember;
			Ember.Position = Start() + FVector(0.0, 0.0, 20.0 * Emitter.Scale);
			Ember.Velocity = FVector(Random.FRandRange(-40.0f, 40.0f), Random.FRandRange(-40.0f, 40.0f) + Wind * 35.0f, Random.FRandRange(90.0f, 210.0f));
			Ember.Life = Random.FRandRange(1.1f, 2.4f);
			Ember.SizeCm = Random.FRandRange(0.8f, 1.6f);
			Ember.Seed = Random.FRand();
			Particles.Add(Ember);
		}
		if (bSmoke && Emitter.bSmoke)
		{
			for (int32 I = Count(FireSmokePerSecond * Size); I > 0; --I)
			{
				FParticle Smoke;
				Smoke.Kind = EFireParticle::Smoke;
				Smoke.Position = Start() + FVector(0.0, 0.0, 40.0 * Emitter.Scale);
				Smoke.Velocity = FVector(Random.FRandRange(-10.0f, 10.0f), Random.FRandRange(-10.0f, 10.0f) + Wind * 30.0f, Random.FRandRange(65.0f, 110.0f));
				Smoke.Life = Random.FRandRange(3.0f, 4.8f);
				Smoke.SizeCm = Random.FRandRange(7.0f, 11.0f) * (0.6f + 0.4f * FMath::Min(Size, 1.75f));
				Smoke.Seed = Random.FRand();
				Particles.Add(Smoke);
			}
		}
	}
}

void UFireSubsystem::MoveParticles(float Delta)
{
	const float Wind = Bridge.IsValid() && Bridge->GetSim() ? static_cast<float>(Bridge->GetSim()->weather().wind()) : 1.0f;
	for (int32 Index = Particles.Num() - 1; Index >= 0; --Index)
	{
		FParticle& P = Particles[Index];
		P.Age += Delta;
		if (P.Age >= P.Life)
		{
			Particles.RemoveAtSwap(Index);
			continue;
		}
		P.Position += P.Velocity * Delta;
		switch (P.Kind)
		{
			case EFireParticle::Smoke:
				P.Velocity.Y += (Wind * 55.0f - P.Velocity.Y) * FMath::Min(1.0f, 0.5f * Delta);
				break;
			case EFireParticle::Ember:
				P.Velocity.X += FMath::Sin((P.Age + P.Seed * 7.0f) * 6.0f) * 90.0f * Delta;
				P.Velocity.Z *= FMath::Max(0.0f, 1.0f - 0.35f * Delta);
				break;
			default:
				// A flame flickers sideways as it climbs.
				P.Position.X += FMath::Sin((P.Age + P.Seed * 5.0f) * 11.0f) * 14.0f * Delta;
				P.Position.Y += FMath::Cos((P.Age + P.Seed * 3.0f) * 9.0f) * 14.0f * Delta;
				break;
		}
	}
}

void UFireSubsystem::DrawParticles()
{
	TArray<TArray<FTransform>> ByKind;
	ByKind.SetNum(static_cast<int32>(EFireParticle::Count));
	FMemory::Memzero(KindCounts);
	for (const FParticle& P : Particles)
	{
		const float T = P.Age / P.Life;
		EFireParticle Kind = P.Kind;
		FTransform Transform;
		switch (P.Kind)
		{
			case EFireParticle::Smoke:
			{
				// Grows as it climbs, thins away (shrinks) over the last third.
				const float Scale = P.SizeCm / 10.0f * (0.6f + 1.4f * T) * (T > 0.65f ? (1.0f - T) / 0.35f : 1.0f);
				Transform = FTransform(FRotator::ZeroRotator, P.Position, FVector(FMath::Max(Scale, 0.01f)));
				break;
			}
			case EFireParticle::Ember:
			{
				// Winks out: flickers and shrinks.
				const float Flicker = 0.6f + 0.4f * FMath::Sin((P.Age * 25.0f) + P.Seed * 40.0f);
				const float Scale = P.SizeCm / 2.0f * Flicker * (1.0f - T * T);
				Transform = FTransform(FRotator(P.Age * 300.0f, P.Age * 450.0f, 0.0f), P.Position, FVector(FMath::Max(Scale, 0.01f)));
				break;
			}
			default:
			{
				// Yellow at the base, orange, then red at the tip; narrower and shorter as it rises.
				Kind = T < 0.28f ? EFireParticle::FlameCore : (T < 0.62f ? EFireParticle::FlameMid : EFireParticle::FlameTip);
				const float Scale = P.SizeCm / 20.0f * FMath::Pow(1.0f - T, 0.75f);
				Transform = FTransform(FRotator(0.0f, P.Seed * 360.0f, 0.0f), P.Position, FVector(FMath::Max(Scale, 0.01f)));
				break;
			}
		}
		ByKind[static_cast<int32>(Kind)].Add(Transform);
		++KindCounts[static_cast<int32>(Kind)];
	}
	for (int32 Kind = 0; Kind < ByKind.Num(); ++Kind)
	{
		if (UInstancedStaticMeshComponent* Component = ParticleInstances.IsValidIndex(Kind) ? ParticleInstances[Kind].Get() : nullptr)
		{
			if (Component->GetInstanceCount() > 0 || !ByKind[Kind].IsEmpty())
			{
				Component->ClearInstances();
				if (!ByKind[Kind].IsEmpty())
				{
					Component->AddInstances(ByKind[Kind], false, true);
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Lights.

void UFireSubsystem::UpdateLights(const colony::Sim& Sim, const USimSubsystem& Source, float Delta)
{
	(void)Sim;
	(void)Delta;
	struct FCandidate
	{
		FFireLightSlot Slot;
		double Distance = 0.0;
	};
	TArray<FCandidate> Candidates;
	const double Time = GetWorld()->GetTimeSeconds();
	const float Scale = GFireLampIntensity.GetValueOnGameThread();
	APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	const FVector Camera = CameraManager ? CameraManager->GetCameraLocation() : FVector::ZeroVector;

	for (const FLamp& Lamp : Lamps)
	{
		FCandidate Candidate;
		Candidate.Slot.Kind = EFireLightKind::Lamp;
		Candidate.Slot.Position = Lamp.Position;
		Candidate.Slot.Color = Lamp.Color;
		Candidate.Slot.RadiusCm = Lamp.RadiusCm;
		Candidate.Slot.Intensity = FireLampCandelas * Lamp.Glow * Scale * (Lamp.bFlicker ? FireNoise(static_cast<float>(Time), Lamp.Seed) : 1.0f);
		Candidate.Distance = FVector::Dist(Lamp.Position, Camera);
		Candidates.Add(Candidate);
	}

	// Fires: one light for each block of FireLightBlockM metres, at the middle of its flames.
	struct FBlock
	{
		FVector Sum = FVector::ZeroVector;
		float Size = 0.0f;
	};
	TMap<int64, FBlock> Blocks;
	for (const TPair<int32, FFireCell>& Pair : FireCells)
	{
		if (Pair.Value.Shown < 0.03f)
		{
			continue;
		}
		const int64 BX = FMath::FloorToInt64(Pair.Value.Position.X / (FireLightBlockM * 100.0));
		const int64 BY = FMath::FloorToInt64(Pair.Value.Position.Y / (FireLightBlockM * 100.0));
		FBlock& Block = Blocks.FindOrAdd(BX * 1000003LL + BY);
		Block.Sum += Pair.Value.Position * Pair.Value.Shown;
		Block.Size += Pair.Value.Shown;
	}
	for (const TPair<int64, FBlock>& Pair : Blocks)
	{
		FCandidate Candidate;
		Candidate.Slot.Kind = EFireLightKind::Fire;
		Candidate.Slot.Position = Pair.Value.Sum / Pair.Value.Size + FVector(0.0, 0.0, 60.0);
		Candidate.Slot.Color = FLinearColor(1.0f, 0.45f, 0.12f);
		Candidate.Slot.RadiusCm = FMath::Clamp(550.0f + 160.0f * FMath::Sqrt(Pair.Value.Size), 550.0f, 1500.0f);
		const float Flicker = FireNoise(static_cast<float>(Time) * 1.3f, static_cast<float>(Pair.Key & 255) / 255.0f);
		Candidate.Slot.Intensity = FMath::Min(FireCellCandelas + FireCandelasPerSize * Pair.Value.Size, FireCandelasMax) * Scale * Flicker;
		Candidate.Distance = FVector::Dist(Candidate.Slot.Position, Camera);
		Candidates.Add(Candidate);
	}
	for (const colony::ThingView& View : Source.GetCurrentSnapshot().things)
	{
		FVector Location;
		if (View.onFire && View.category == colony::ThingCategory::Pawn && Source.GetBlendedLocation(View.id, Location))
		{
			FCandidate Candidate;
			Candidate.Slot.Kind = EFireLightKind::Pawn;
			Candidate.Slot.Position = Location + FVector(0.0, 0.0, 100.0);
			Candidate.Slot.Color = FLinearColor(1.0f, 0.48f, 0.15f);
			Candidate.Slot.RadiusCm = 450.0f;
			Candidate.Slot.Intensity = FirePawnCandelas * Scale * FireNoise(static_cast<float>(Time) * 1.7f, static_cast<float>(entt::to_integral(View.id) & 255) / 255.0f);
			Candidate.Distance = FVector::Dist(Candidate.Slot.Position, Camera);
			Candidates.Add(Candidate);
		}
	}
	Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Distance < B.Distance; });

	const int32 Wanted = FMath::Clamp(GFireMaxLights.GetValueOnGameThread(), 0, Lights.Num());
	LightSlots.Reset();
	int32 Shadows = 0;
	for (int32 I = 0; I < Lights.Num(); ++I)
	{
		UPointLightComponent* Light = Lights[I];
		if (!Light)
		{
			continue;
		}
		if (I >= Wanted || I >= Candidates.Num())
		{
			if (Light->IsVisible())
			{
				Light->SetVisibility(false);
			}
			continue;
		}
		FFireLightSlot Slot = Candidates[I].Slot;
		Slot.bShadows = Shadows < FireShadowLights && Slot.Kind != EFireLightKind::Pawn;
		Shadows += Slot.bShadows ? 1 : 0;
		Light->SetWorldLocation(Slot.Position);
		Light->SetLightColor(Slot.Color);
		Light->SetIntensity(Slot.Intensity);
		Light->SetAttenuationRadius(Slot.RadiusCm);
		Light->SetCastShadows(Slot.bShadows);
		if (!Light->IsVisible())
		{
			Light->SetVisibility(true);
		}
		LightSlots.Add(Slot);
	}
}

// ---------------------------------------------------------------------------
// Burnt ground and ash: skins over the ground, chunk by chunk.

void UFireSubsystem::StartGroundCheck(const USimSubsystem& Source)
{
	const colony::Sim& Sim = *Source.GetSim();
	const colony::Snapshot& Snapshot = Source.GetCurrentSnapshot();
	const colony::Map& Map = Sim.map();
	GroundChunksX = FireChunksFor(Map.width());
	GroundChunksY = FireChunksFor(Map.height());
	if (GroundChunks.Num() != GroundChunksX * GroundChunksY)
	{
		for (FGroundChunk& Chunk : GroundChunks)
		{
			if (Chunk.Mesh)
			{
				Chunk.Mesh->DestroyComponent();
			}
		}
		GroundChunks.Reset();
		GroundChunks.SetNum(GroundChunksX * GroundChunksY);
	}
	BurntBuckets.Reset();
	BurntBuckets.SetNum(GroundChunks.Num());
	AshBuckets.Reset();
	AshBuckets.SetNum(GroundChunks.Num());
	const auto ChunkOf = [this](const colony::CellPos& Cell)
	{
		return (Cell.y / TerrainDraw::ChunkCells) * GroundChunksX + Cell.x / TerrainDraw::ChunkCells;
	};
	for (const colony::CellPos& Cell : Snapshot.burnt)
	{
		if (Map.inBounds(Cell))
		{
			BurntBuckets[ChunkOf(Cell)].Add(Cell.y * 65536 + Cell.x);
		}
	}
	const colony::DefId Ash = Sim.defs().idOf("Filth", "Ash");
	for (const colony::FilthView& Filth : Snapshot.filth)
	{
		if (Filth.def == Ash && Map.inBounds(Filth.cell))
		{
			AshBuckets[ChunkOf(Filth.cell)].Add(Filth.cell.y * 65536 + Filth.cell.x);
		}
	}
	GroundCursor = 0;
	bGroundChecking = true;
}

bool UFireSubsystem::BuildGroundChunk(const USimSubsystem& Source, int32 ChunkX, int32 ChunkY)
{
	const int32 Index = ChunkY * GroundChunksX + ChunkX;
	FGroundChunk& Chunk = GroundChunks[Index];
	const TArray<int32>& Burnt = BurntBuckets[Index];
	const TArray<int32>& Ash = AshBuckets[Index];
	const auto Signature = [](const TArray<int32>& Cells)
	{
		uint64 Hash = Cells.IsEmpty() ? 0 : 1469598103934665603ULL;
		for (const int32 Cell : Cells)
		{
			Hash = (Hash ^ static_cast<uint64>(static_cast<uint32>(Cell))) * 1099511628211ULL;
		}
		return Hash;
	};
	const uint64 BurntHash = Signature(Burnt);
	const uint64 AshHash = Signature(Ash);
	if (BurntHash == Chunk.BurntHash && AshHash == Chunk.AshHash)
	{
		return false;
	}
	const double Start = FPlatformTime::Seconds();
	Chunk.BurntHash = BurntHash;
	Chunk.AshHash = AshHash;
	BurntDrawn += Burnt.Num() - Chunk.Burnt;
	AshDrawn += Ash.Num() - Chunk.Ash;
	Chunk.Burnt = Burnt.Num();
	Chunk.Ash = Ash.Num();

	const colony::Map& Map = Source.GetSim()->map();
	const int32 MapW = Map.width();
	const int32 MapH = Map.height();
	const auto Height = [&Map, MapW, MapH](int32 CX, int32 CY)
	{
		return static_cast<double>(Map.groundHeight(FMath::Clamp(CX, 0, MapW - 1), FMath::Clamp(CY, 0, MapH - 1))) * 100.0;
	};
	TArray<FVector> Positions;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> Colors;
	TArray<int32> Triangles;
	const auto AddTriangle3 = [&](const FVector& P, const FVector& Q, const FVector& R, const FColor& CP, const FColor& CQ, const FColor& CR)
	{
		FVector Normal = FVector::CrossProduct(R - P, Q - P).GetSafeNormal();
		if (Normal.Z < 0.0)
		{
			Normal = -Normal;
		}
		const int32 First = Positions.Num();
		Positions.Add(P);
		Positions.Add(Q);
		Positions.Add(R);
		for (int32 I = 0; I < 3; ++I)
		{
			Normals.Add(Normal);
		}
		Colors.Add(CP);
		Colors.Add(CQ);
		Colors.Add(CR);
		UVs.Add(FVector2D(P.X * 0.01, P.Y * 0.01));
		UVs.Add(FVector2D(Q.X * 0.01, Q.Y * 0.01));
		UVs.Add(FVector2D(R.X * 0.01, R.Y * 0.01));
		Triangles.Append({First, First + 1, First + 2});
	};
	const auto AddTriangle = [&](const FVector& P, const FVector& Q, const FVector& R, const FColor& Color) { AddTriangle3(P, Q, R, Color, Color, Color); };
	// The charcoal is a little lighter or darker at each corner, so the patches have no cell edges.
	const auto Shade = [](int32 CX, int32 CY)
	{
		return (FireBurntColor * (0.8f + 0.45f * static_cast<float>(FireHash(CX, CY) & 255u) / 255.0f)).ToFColor(false);
	};
	for (const int32 Packed : Burnt)
	{
		const int32 X = Packed & 0xFFFF;
		const int32 Y = Packed >> 16;
		if (Map.terrain(X, Y) == colony::Terrain::Rock || Map.terrain(X, Y) == colony::Terrain::Water)
		{
			continue;
		}
		const FVector A(X * 100.0, Y * 100.0, Height(X, Y) + FireBurntLiftCm);
		const FVector B((X + 1) * 100.0, Y * 100.0, Height(X + 1, Y) + FireBurntLiftCm);
		const FVector C((X + 1) * 100.0, (Y + 1) * 100.0, Height(X + 1, Y + 1) + FireBurntLiftCm);
		const FVector E(X * 100.0, (Y + 1) * 100.0, Height(X, Y + 1) + FireBurntLiftCm);
		AddTriangle3(A, E, B, Shade(X, Y), Shade(X, Y + 1), Shade(X + 1, Y));
		AddTriangle3(B, E, C, Shade(X + 1, Y), Shade(X, Y + 1), Shade(X + 1, Y + 1));
	}
	for (const int32 Packed : Ash)
	{
		const int32 X = Packed & 0xFFFF;
		const int32 Y = Packed >> 16;
		if (Map.terrain(X, Y) == colony::Terrain::Rock || Map.terrain(X, Y) == colony::Terrain::Water)
		{
			continue;
		}
		// A grey patch a little smaller than the cell, turned and moved a little by the cell.
		const uint32 H = FireHash(X + 17, Y + 91);
		const float Yaw = static_cast<float>(H & 1023u) / 1023.0f * 2.0f * PI;
		const FVector Centre((X + 0.5 + (static_cast<float>((H >> 10) & 255u) / 255.0f - 0.5f) * 0.2) * 100.0, (Y + 0.5 + (static_cast<float>((H >> 18) & 255u) / 255.0f - 0.5f) * 0.2) * 100.0,
			(Height(X, Y) + Height(X + 1, Y + 1)) * 0.5 + FireAshLiftCm);
		const double Half = 20.0;
		const FVector Axis1(FMath::Cos(Yaw) * Half, FMath::Sin(Yaw) * Half, 0.0);
		const FVector Axis2(-FMath::Sin(Yaw) * Half, FMath::Cos(Yaw) * Half, 0.0);
		const float Variation = static_cast<float>((H >> 26) & 63u) / 63.0f;
		const FColor Color = (FireAshColor * (0.8f + 0.4f * Variation)).ToFColor(false);
		AddTriangle(Centre - Axis1 - Axis2, Centre - Axis1 + Axis2, Centre + Axis1 - Axis2, Color);
		AddTriangle(Centre + Axis1 - Axis2, Centre - Axis1 + Axis2, Centre + Axis1 + Axis2, Color);
	}

	if (!Chunk.Mesh)
	{
		Chunk.Mesh = NewObject<UProceduralMeshComponent>(Holder);
		Chunk.Mesh->bUseAsyncCooking = false;
		Chunk.Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Chunk.Mesh->SetCanEverAffectNavigation(false);
		Chunk.Mesh->SetCastShadow(false);
		Chunk.Mesh->bReceivesDecals = false;
		Chunk.Mesh->SetupAttachment(Holder->GetRootComponent());
		Chunk.Mesh->RegisterComponent();
		Chunk.Mesh->SetWorldLocation(BuiltOrigin);
	}
	if (Positions.IsEmpty())
	{
		Chunk.Mesh->ClearMeshSection(0);
	}
	else
	{
		Chunk.Mesh->CreateMeshSection(0, Positions, Triangles, Normals, UVs, Colors, TArray<FProcMeshTangent>(), false);
		if (LitMaterial)
		{
			Chunk.Mesh->SetMaterial(0, LitMaterial);
		}
	}
	++GroundChunksBuilt;
	SlowestChunkMs = FMath::Max(SlowestChunkMs, (FPlatformTime::Seconds() - Start) * 1000.0);
	return true;
}

void UFireSubsystem::CheckGround(const USimSubsystem& Source, int32 Budget)
{
	const int32 Total = GroundChunksX * GroundChunksY;
	int32 Done = 0;
	while (GroundCursor < Total && Done < Budget)
	{
		BuildGroundChunk(Source, GroundCursor % GroundChunksX, GroundCursor / GroundChunksX);
		++GroundCursor;
		++Done;
	}
	if (GroundCursor >= Total)
	{
		bGroundChecking = false;
	}
}

void UFireSubsystem::SyncGround()
{
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim() || !Holder)
	{
		return;
	}
	StartGroundCheck(*Source);
	CheckGround(*Source, GroundChunksX * GroundChunksY);
	const colony::Snapshot& Snapshot = Source->GetCurrentSnapshot();
	SeenBurntChanges = Snapshot.burntChanges;
	SeenFilthChanges = Snapshot.filthChanges;
}

// ---------------------------------------------------------------------------

void UFireSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim() || GFireEnable.GetValueOnGameThread() == 0)
	{
		if (Holder)
		{
			Teardown();
		}
		return;
	}
	const colony::Sim& Sim = *Source->GetSim();
	if (!Holder || BuiltGeneration != Source->GetSimGeneration() || BuiltSim != &Sim)
	{
		Teardown();
		EnsureScene();
		if (!Holder)
		{
			return;
		}
		BuiltSim = &Sim;
		BuiltGeneration = Source->GetSimGeneration();
		BuiltOrigin = Source->GetWorldOrigin();
	}
	if (Source->GetWorldOrigin() != BuiltOrigin)
	{
		BuiltOrigin = Source->GetWorldOrigin();
		for (FGroundChunk& Chunk : GroundChunks)
		{
			if (Chunk.Mesh)
			{
				Chunk.Mesh->SetWorldLocation(BuiltOrigin);
			}
		}
		// Everything else is in world coordinates and is remade from the sim.
		ResetFire();
	}

	// The lamps the sim has lit: read again when its light layer was rebuilt.
	if (Sim.lights().rebuilds() != SeenLightRebuilds)
	{
		SeenLightRebuilds = Sim.lights().rebuilds();
		ReadLamps(Sim, *Source);
	}
	SyncFires(*Source);
	const float Delta = Source->GetSpeed() > 0 ? FMath::Min(DeltaTime, 0.1f) : 0.0f;
	for (auto It = FireCells.CreateIterator(); It; ++It)
	{
		FFireCell& Cell = It.Value();
		Cell.Shown = FMath::FInterpConstantTo(Cell.Shown, Cell.Target, Delta, FireSizeRate);
		if (Cell.Shown <= 0.0f && Cell.Target <= 0.0f)
		{
			It.RemoveCurrent();
		}
	}

	APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	const FVector Camera = CameraManager ? CameraManager->GetCameraLocation() : FVector::ZeroVector;
	TArray<FEmitter> Emitters;
	GatherEmitters(*Source, Emitters);
	Emit(Emitters, Delta, Camera);
	MoveParticles(Delta);
	DrawParticles();
	UpdateLights(Sim, *Source, Delta);

	// The burnt ground and ash: looked at again, a few chunks a frame, when the sim's lists changed.
	const colony::Snapshot& Snapshot = Source->GetCurrentSnapshot();
	if (Snapshot.burntChanges != SeenBurntChanges || Snapshot.filthChanges != SeenFilthChanges)
	{
		SeenBurntChanges = Snapshot.burntChanges;
		SeenFilthChanges = Snapshot.filthChanges;
		StartGroundCheck(*Source);
	}
	if (bGroundChecking)
	{
		CheckGround(*Source, 12);
	}
}
