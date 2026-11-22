#include "BlastFxSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"

#include "BlastFxData.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Weather.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlastFx, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GBlastEnable(TEXT("Blast.Enable"), 1,
		TEXT("1 draws blasts with the realistic look (needs -run=BlastArt), 0 falls back to the simple look."), ECVF_Default);
	TAutoConsoleVariable<float> GBlastFireBrightness(TEXT("Blast.FireBrightness"), 2.0f,
		TEXT("How brightly a blast's fire glows, against the camera's exposure (1: white-hot reads as plain white; same at noon and night)."), ECVF_Default);
	TAutoConsoleVariable<float> GBlastLightScale(TEXT("Blast.LightScale"), 1.0f,
		TEXT("Scales a blast's flash of light (400,000 cd peak and 60,000 cd glow for a frag grenade)."), ECVF_Default);
	TAutoConsoleVariable<float> GBlastSmokeSeconds(TEXT("Blast.SmokeSeconds"), 14.0f,
		TEXT("How long a blast's smoke hangs in the air, seconds (each puff 70% to 120% of it)."), ECVF_Default);
	TAutoConsoleVariable<float> GBlastScorchSeconds(TEXT("Blast.ScorchSeconds"), 300.0f,
		TEXT("How long a blast's scorch mark takes to fade away, seconds."), ECVF_Default);
	TAutoConsoleVariable<int32> GBlastMaxSprites(TEXT("Blast.MaxSprites"), 1500,
		TEXT("At most this many blast sprites (fire, smoke, sparks) at once; new ones are skipped past it."), ECVF_Default);
	TAutoConsoleVariable<int32> GBlastFlipV(TEXT("Blast.FlipV"), 0,
		TEXT("1 turns the fire and smoke pictures upside down (if the plane mesh's V runs the other way)."), ECVF_Default);

	const TCHAR* const BlastPlanePath = TEXT("/Engine/BasicShapes/Plane.Plane");
	constexpr int32 BlastMaxScorches = 40;
	// Past this the sprites are not made (the light and the sound still are).
	constexpr double BlastDrawMetres = 600.0;

	// Smoke colours (linear): soot from the charge, dust and soil from the ground.
	const FLinearColor BlastSoot(0.085f, 0.08f, 0.075f);
	const FLinearColor BlastDust(0.26f, 0.23f, 0.19f);
	const FLinearColor BlastSoil(0.17f, 0.13f, 0.09f);

	FVector BlastSimToWorld(const USimSubsystem& Source, float X, float Y, float Z)
	{
		return Source.GetWorldOrigin() + FVector(X * 100.0, Y * 100.0, Z * 100.0);
	}
}

bool UBlastFxSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UBlastFxSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBlastFxSubsystem, STATGROUP_Tickables);
}

void UBlastFxSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Source = Collection.InitializeDependency<USimSubsystem>())
	{
		Bridge = Source;
		EventHandle = Source->OnSimEvent.AddUObject(this, &UBlastFxSubsystem::OnSimEvent);
	}
}

void UBlastFxSubsystem::Deinitialize()
{
	if (USimSubsystem* Source = Bridge.Get())
	{
		Source->OnSimEvent.Remove(EventHandle);
	}
	Teardown();
	Super::Deinitialize();
}

void UBlastFxSubsystem::Teardown()
{
	Scorches.Reset();
	ScorchInstances = nullptr;
	bLoggedScorchDraw = false;
	Sprites.Reset();
	if (Holder)
	{
		Holder->Destroy();
	}
	Holder = nullptr;
	Instances.Reset();
	FlashLight = nullptr;
	FlashAge = 99.0f;
	FlashCandelas = 0.0f;
}

bool UBlastFxSubsystem::IsReady()
{
	if (GBlastEnable.GetValueOnGameThread() == 0)
	{
		return false;
	}
	if (!bTriedLoad)
	{
		bTriedLoad = true;
		bReady = LoadObject<UMaterialInterface>(nullptr, BlastFxData::FireMaterial, nullptr, LOAD_NoWarn | LOAD_Quiet) != nullptr
			&& LoadObject<UMaterialInterface>(nullptr, BlastFxData::SmokeMaterial, nullptr, LOAD_NoWarn | LOAD_Quiet) != nullptr
			&& LoadObject<UStaticMesh>(nullptr, BlastPlanePath) != nullptr;
		if (!bReady)
		{
			UE_LOG(LogBlastFx, Display, TEXT("No blast art in %s (run -run=BlastArt): blasts use the simple look"), BlastFxData::Folder);
		}
	}
	return bReady;
}

bool UBlastFxSubsystem::EnsureScene()
{
	if (Holder)
	{
		return true;
	}
	if (!IsReady() || !GetWorld())
	{
		return false;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("BlastRoot"));
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	Holder->SetActorLabel(TEXT("BlastFx"));
#endif
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, BlastPlanePath);
	struct FKindArt
	{
		const TCHAR* Material;
		int32 Floats;
		int32 SortPriority;
	};
	const FKindArt Arts[] = {
		{BlastFxData::FireMaterial, BlastFxData::SpriteFloats, 2},
		{BlastFxData::SmokeMaterial, BlastFxData::SpriteFloats, 1},
		{BlastFxData::SparkMaterial, BlastFxData::SparkFloats, 3},
		{BlastFxData::HazeMaterial, BlastFxData::HazeFloats, 0},
	};
	Instances.Reset();
	for (const FKindArt& Art : Arts)
	{
		UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(Holder);
		Component->SetStaticMesh(Plane);
		if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, Art.Material, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			Component->SetMaterial(0, Material);
		}
		else
		{
			// Sparks or the haze missing: that kind is simply not drawn.
			Component->SetVisibility(false);
		}
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCanEverAffectNavigation(false);
		Component->SetCastShadow(false);
		Component->bReceivesDecals = false;
		Component->bUseAsOccluder = false;
		Component->TranslucencySortPriority = Art.SortPriority;
		Component->NumCustomDataFloats = Art.Floats;
		Component->SetupAttachment(Root);
		Component->RegisterComponent();
		Instances.Add(Component);
	}
	if (UMaterialInterface* Scorch = LoadObject<UMaterialInterface>(nullptr, BlastFxData::ScorchMaterial, nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		ScorchInstances = NewObject<UInstancedStaticMeshComponent>(Holder);
		ScorchInstances->SetStaticMesh(Plane);
		ScorchInstances->SetMaterial(0, Scorch);
		ScorchInstances->SetMobility(EComponentMobility::Movable);
		ScorchInstances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ScorchInstances->SetGenerateOverlapEvents(false);
		ScorchInstances->SetCanEverAffectNavigation(false);
		ScorchInstances->SetCastShadow(false);
		ScorchInstances->bReceivesDecals = false;
		ScorchInstances->bUseAsOccluder = false;
		// Under the smoke and everything else see-through.
		ScorchInstances->TranslucencySortPriority = -1;
		ScorchInstances->NumCustomDataFloats = 2;  // fade, age (for the embers)
		ScorchInstances->SetupAttachment(Root);
		ScorchInstances->RegisterComponent();
		// The scorch never showed on the PC (rounds 3 to 7): say what it is drawn with, to tell a stale or wrong material.
		const UMaterial* Base = Scorch->GetMaterial();
		UE_LOG(LogBlastFx, Display, TEXT("Scorch material %s: domain %d, blend %d, two-sided %d, ISM usage %d"), *Scorch->GetPathName(),
			Base ? static_cast<int32>(Base->MaterialDomain) : -1, static_cast<int32>(Scorch->GetBlendMode()), Scorch->IsTwoSided() ? 1 : 0,
			Base && Base->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes) ? 1 : 0);
	}
	else
	{
		UE_LOG(LogBlastFx, Warning, TEXT("No %s: blasts leave no scorch mark"), BlastFxData::ScorchMaterial);
	}

	FlashLight = NewObject<UPointLightComponent>(Holder);
	FlashLight->SetMobility(EComponentMobility::Movable);
	FlashLight->SetIntensityUnits(ELightUnits::Candelas);
	FlashLight->SetIntensity(0.0f);
	FlashLight->SetAttenuationRadius(3500.0f);
	FlashLight->SetSourceRadius(60.0f);
	FlashLight->SetCastShadows(true);
	FlashLight->SetupAttachment(Root);
	FlashLight->RegisterComponent();
	FlashLight->SetVisibility(false);
	return true;
}

// ---------------------------------------------------------------------------
// Blasts.

void UBlastFxSubsystem::OnSimEvent(const colony::Event& Event)
{
	if (Event.type != colony::EventType::Explosion)
	{
		return;
	}
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim())
	{
		return;
	}
	const float RadiusM = FMath::Max(Event.amount, 0.5f);
	const FVector Centre = BlastSimToWorld(*Source, Event.x, Event.y, Event.z);
	const colony::DefId Type = Event.def;
	const bool bFire = Type.valid() && Source->GetDefs() && Source->GetDefs()->get(Type).text("mix.sub") == "Fire";
	// Ragdolls (corpses, the knocked out) are thrown by it (sneed 2026-11-20: "punt and ragdoll corpses, not cartoonishly
	// but a decent amount"): a frag grenade throws a body at about 5 m/s, a rocket about 7, a doomsday rocket 9, nothing at
	// the edge. The sim moves the corpse's place the same way (CombatSystem::explode).
	if (!bFire)
	{
		if (const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
		{
			const float Speed = FMath::Clamp(250.0f + 120.0f * RadiusM, 300.0f, 900.0f);
			const double Reach = RadiusM * 120.0;
			for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Pair : Pawns->GetPawns())
			{
				ASimPawnActor* Actor = Pair.Value.Get();
				if (!Actor)
				{
					continue;
				}
				// Where the body lies (the ragdoll's pelvis), not where the actor stood when it fell.
				FVector Lying = Actor->GetActorLocation();
				Actor->GetRagdollPelvis(Lying);
				const double Away = FVector::Dist(Lying, Centre);
				if (Away < Reach + 300.0)
				{
					UE_LOG(LogTemp, Display, TEXT("Blast throw: pawn %u ragdoll %d, %.0f cm from the blast (reach %.0f)"), Pair.Key, Actor->IsRagdoll() ? 1 : 0, Away, Reach + 100.0);
				}
				if (Actor->IsRagdoll() && Away < Reach + 100.0)
				{
					Actor->PushRagdoll(Centre, static_cast<float>(Reach + 100.0), Speed);
					++RagdollsThrown;
				}
			}
		}
	}

	// The rest is art: without it (-run=BlastArt not run) the bodies are still thrown, above.
	if (!EnsureScene())
	{
		return;
	}
	const FVector Ground = Source->SimToWorld(Event.x, Event.y, 0);
	++BlastsDrawn;

	// The light first: even a blast too far to draw lights the sky.
	FlashAt = Centre + FVector(0.0, 0.0, 120.0);
	FlashAge = 0.0f;
	FlashScale = FMath::Clamp(RadiusM / (bFire ? 1.1f : 1.9f), 0.5f, 4.0f);
	bFlashFire = bFire;
	if (FlashLight)
	{
		FlashLight->SetWorldLocation(FlashAt);
	}

	APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	if (Camera && FVector::Dist(Camera->GetCameraLocation(), Centre) > BlastDrawMetres * 100.0)
	{
		return;
	}
	// The ground the dust and dirt come off: the terrain under it, or just below the blast if it went off up on something.
	const double Above = Centre.Z - Ground.Z;
	const FVector Floor(Centre.X, Centre.Y, Above >= 0.0 && Above < 150.0 ? Ground.Z : Centre.Z - 30.0);
	SpawnBlast(Centre, Floor, RadiusM, bFire);
	if (!bFire)
	{
		// The ground leans with the map there (as the debris floor).
		const colony::Map& Map = Source->GetSim()->map();
		const float MX = Event.x;
		const float MY = Event.y;
		const FVector Normal = FVector((Map.heightAt(MX - 1.0f, MY) - Map.heightAt(MX + 1.0f, MY)) * 0.5f, (Map.heightAt(MX, MY - 1.0f) - Map.heightAt(MX, MY + 1.0f)) * 0.5f, 1.0f).GetSafeNormal();
		SpawnScorch(Floor, Normal, RadiusM);
	}
}

UBlastFxSubsystem::FSprite& UBlastFxSubsystem::Add(EKind Kind)
{
	FSprite& Sprite = Sprites.AddDefaulted_GetRef();
	Sprite.Kind = Kind;
	Sprite.Roll = Random.FRandRange(-180.0f, 180.0f);
	Sprite.Flip = Random.FRand() < 0.5f ? 0.0f : 1.0f;
	return Sprite;
}

void UBlastFxSubsystem::SpawnBlast(const FVector& Centre, const FVector& Ground, float RadiusM, bool bFire)
{
	const int32 Budget = FMath::Max(0, GBlastMaxSprites.GetValueOnGameThread() - Sprites.Num());
	if (Budget < 20)
	{
		return;
	}
	const float FireGain = FMath::Max(0.0f, GBlastFireBrightness.GetValueOnGameThread());
	const float SmokeSeconds = FMath::Clamp(GBlastSmokeSeconds.GetValueOnGameThread(), 1.0f, 120.0f);

	if (bFire)
	{
		// A molotov: a wide low splash of burning fuel, black smoke, a few sparks (the sim lights the ground).
		const float S = FMath::Clamp(RadiusM / 1.1f, 0.6f, 3.0f);
		for (int32 I = 0; I < 9; ++I)
		{
			FSprite& Fire = Add(EKind::Fire);
			const FVector Out = FVector(Random.FRandRange(-1.0f, 1.0f), Random.FRandRange(-1.0f, 1.0f), 0.0f).GetSafeNormal();
			Fire.Position = Ground + Out * Random.FRandRange(0.0f, 90.0f * S) + FVector(0.0, 0.0, 30.0);
			Fire.Velocity = Out * Random.FRandRange(100.0f, 250.0f) * S + FVector(0.0, 0.0, Random.FRandRange(80.0f, 200.0f));
			Fire.Drag = 3.0f;
			Fire.Gravity = -150.0f;
			Fire.StartCm = 110.0f * S;
			Fire.EndCm = 220.0f * S;
			Fire.Life = Random.FRandRange(0.9f, 1.4f);
			Fire.FadeIn = 0.03f;
			// Burning fuel stays hot: only the flipbook's fiery first half, a little brighter than the frag's (PC round 6: the
			// whole flipbook read as a smoky blob).
			Fire.FrameTo = 32.0f;
			Fire.FireGain = FireGain * 1.3f;
			Fire.Roll = Random.FRandRange(-25.0f, 25.0f);
			Fire.Tint = BlastSoot;
			Fire.Wind = 0.3f;
		}
		for (int32 I = 0; I < 6; ++I)
		{
			FSprite& Smoke = Add(EKind::Smoke);
			Smoke.Age = -Random.FRandRange(0.2f, 0.8f);
			Smoke.Position = Ground + FVector(Random.FRandRange(-60.0f, 60.0f) * S, Random.FRandRange(-60.0f, 60.0f) * S, Random.FRandRange(80.0f, 160.0f));
			Smoke.Velocity = FVector(0.0, 0.0, Random.FRandRange(60.0f, 110.0f));
			Smoke.Drag = 0.4f;
			Smoke.Gravity = -10.0f;
			Smoke.Wind = 1.0f;
			Smoke.StartCm = 120.0f * S;
			Smoke.EndCm = Random.FRandRange(380.0f, 520.0f) * S;
			Smoke.Life = SmokeSeconds * Random.FRandRange(0.45f, 0.65f);
			Smoke.FadeIn = 0.5f;
			Smoke.Opacity = 0.8f;
			Smoke.Glow = 0.3f;
			Smoke.Spin = Random.FRandRange(-8.0f, 8.0f);
			Smoke.Tint = FLinearColor(0.03f, 0.028f, 0.026f);
		}
		for (int32 I = 0; I < 14; ++I)
		{
			FSprite& Spark = Add(EKind::Spark);
			FVector Dir = Random.VRand();
			Dir.Z = FMath::Abs(Dir.Z) * 0.8f + 0.2f;
			Spark.Position = Ground + FVector(0.0, 0.0, 20.0);
			Spark.Velocity = Dir.GetSafeNormal() * Random.FRandRange(300.0f, 900.0f);
			Spark.Gravity = 980.0f;
			Spark.Drag = 0.8f;
			Spark.Life = Random.FRandRange(0.5f, 1.2f);
			Spark.StartCm = Spark.EndCm = 2.5f;
			Spark.FireGain = 3.0f;
			Spark.FadeIn = 0.0f;
		}
		return;
	}

	const float S = FMath::Clamp(RadiusM / 1.9f, 0.4f, 4.0f);
	const float Root = FMath::Sqrt(S);

	// The flash: a white-hot ball for a tenth of a second.
	{
		FSprite& Flash = Add(EKind::Fire);
		Flash.Position = Centre + FVector(0.0, 0.0, 50.0);
		Flash.StartCm = 380.0f * S;
		Flash.EndCm = 500.0f * S;
		Flash.Life = 0.14f;
		Flash.FadeIn = 0.0f;
		Flash.FrameFrom = 0.0f;
		Flash.FrameTo = 2.0f;
		Flash.FireGain = FireGain * 3.0f;
		Flash.Roll = Random.FRandRange(-20.0f, 20.0f);
		Flash.Tint = BlastSoot;
	}
	// The fireball: a few lumps thrown out and up, each a flipbook from white-hot to dark smoke.
	for (int32 I = 0; I < 5; ++I)
	{
		FSprite& Fire = Add(EKind::Fire);
		const FVector Out = Random.VRand();
		Fire.Position = Centre + Out * Random.FRandRange(0.0f, 50.0f * S) + FVector(0.0, 0.0, 50.0);
		Fire.Velocity = FVector(Out.X, Out.Y, FMath::Abs(Out.Z)) * 160.0f * S + FVector(0.0, 0.0, 120.0f);
		Fire.Drag = 3.0f;
		Fire.Gravity = -60.0f;
		Fire.Wind = 0.3f;
		// The flipbook's ball fills about 40% of its square at first, so this makes a fireball about 2.5 m across that
		// swells to 4 m (PC round 3: 2 m squares read as a head-sized glow).
		Fire.StartCm = Random.FRandRange(300.0f, 370.0f) * S;
		Fire.EndCm = Random.FRandRange(450.0f, 570.0f) * S;
		Fire.Life = Random.FRandRange(1.3f, 1.8f);
		Fire.FadeIn = 0.02f;
		Fire.FireGain = FireGain;
		Fire.Roll = Random.FRandRange(-20.0f, 20.0f);
		Fire.Spin = Random.FRandRange(-10.0f, 10.0f);
		Fire.Tint = BlastSoot;
	}
	// The smoke: a cloud of soot and dust that lifts, swells and drifts with the wind for 10 to 20 seconds.
	for (int32 I = 0; I < 8; ++I)
	{
		FSprite& Smoke = Add(EKind::Smoke);
		const FVector Out = FVector(Random.FRandRange(-1.0f, 1.0f), Random.FRandRange(-1.0f, 1.0f), 0.0f).GetSafeNormal();
		Smoke.Age = -Random.FRandRange(0.1f, 0.6f);
		Smoke.Position = Centre + Out * Random.FRandRange(0.0f, 80.0f * S) + FVector(0.0, 0.0, Random.FRandRange(30.0f, 150.0f) * S);
		Smoke.Velocity = Out * Random.FRandRange(30.0f, 90.0f) * S + FVector(0.0, 0.0, Random.FRandRange(40.0f, 90.0f));
		Smoke.Drag = 0.6f;
		Smoke.Gravity = -15.0f;
		Smoke.Wind = 1.0f;
		// A bigger blast makes more of a cloud, but not one as many times wider (PC round 2: a 5 m blast hid the sky).
		Smoke.StartCm = 180.0f * Root;
		Smoke.EndCm = Random.FRandRange(420.0f, 600.0f) * Root;
		Smoke.Life = SmokeSeconds * Random.FRandRange(0.7f, 1.2f);
		Smoke.FadeIn = 0.4f;
		Smoke.Opacity = 0.65f;
		Smoke.Glow = 0.35f;
		Smoke.Spin = Random.FRandRange(-8.0f, 8.0f);
		const float Mix = Random.FRand();
		Smoke.Tint = FLinearColor::LerpUsingHSV(BlastSoot, BlastDust, Mix * 0.8f);
	}
	// Dirt thrown up from the ground under the blast, falling back as a brown haze.
	for (int32 I = 0; I < 7; ++I)
	{
		FSprite& Dirt = Add(EKind::Smoke);
		Dirt.Position = Ground + FVector(Random.FRandRange(-30.0f, 30.0f), Random.FRandRange(-30.0f, 30.0f), 20.0f);
		Dirt.Velocity = FVector(Random.FRandRange(-150.0f, 150.0f), Random.FRandRange(-150.0f, 150.0f), Random.FRandRange(500.0f, 900.0f) * Root);
		Dirt.Gravity = 600.0f;
		Dirt.Drag = 1.5f;
		Dirt.Wind = 0.4f;
		Dirt.StartCm = 60.0f * S;
		Dirt.EndCm = 240.0f * S;
		Dirt.Life = Random.FRandRange(2.0f, 3.5f);
		Dirt.FadeIn = 0.05f;
		Dirt.Opacity = 0.9f;
		Dirt.FrameFrom = 0.0f;
		Dirt.FrameTo = 63.0f;
		Dirt.Tint = BlastSoil;
	}
	// The shock wave: a ring of dust racing out along the ground and stopping a few metres out.
	const int32 Ring = FMath::Clamp(FMath::RoundToInt(20.0f * Root), 12, 40);
	for (int32 I = 0; I < Ring; ++I)
	{
		FSprite& Dust = Add(EKind::Smoke);
		const float Angle = (I + Random.FRandRange(-0.3f, 0.3f)) / Ring * 2.0f * PI;
		const FVector Out(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
		Dust.Position = Ground + Out * 40.0f * S + FVector(0.0, 0.0, 25.0);
		Dust.Velocity = Out * Random.FRandRange(1100.0f, 1600.0f) * Root + FVector(0.0, 0.0, Random.FRandRange(0.0f, 60.0f));
		Dust.Drag = 3.5f;
		Dust.Gravity = -20.0f;
		Dust.Wind = 0.6f;
		Dust.StartCm = 70.0f * S;
		Dust.EndCm = Random.FRandRange(250.0f, 350.0f) * S;
		Dust.Life = Random.FRandRange(2.5f, 4.0f);
		Dust.FadeIn = 0.05f;
		Dust.Opacity = 0.55f;
		Dust.Tint = BlastDust;
	}
	// Heat shimmer: a ring of bent light swelling out in a third of a second.
	{
		FSprite& Haze = Add(EKind::Haze);
		Haze.Position = Centre + FVector(0.0, 0.0, 60.0);
		Haze.StartCm = 120.0f * S;
		Haze.EndCm = 1100.0f * S;
		Haze.Life = 0.35f;
		Haze.FadeIn = 0.0f;
		Haze.Opacity = 0.6f;
	}
	// Hot fragments: fast glowing streaks, and slower embers that fall.
	const int32 Fragments = FMath::Clamp(FMath::RoundToInt(40.0f * S), 15, 120);
	for (int32 I = 0; I < Fragments + 10; ++I)
	{
		const bool bEmber = I >= Fragments;
		FSprite& Spark = Add(EKind::Spark);
		FVector Dir = Random.VRand();
		Dir.Z = FMath::Abs(Dir.Z) * 0.8f + 0.1f;
		Spark.Position = Centre + FVector(0.0, 0.0, 30.0);
		Spark.Velocity = Dir.GetSafeNormal() * (bEmber ? Random.FRandRange(300.0f, 800.0f) : Random.FRandRange(1500.0f, 3500.0f));
		Spark.Gravity = 980.0f;
		Spark.Drag = bEmber ? 1.2f : 0.6f;
		Spark.Life = bEmber ? Random.FRandRange(1.2f, 2.2f) : Random.FRandRange(0.25f, 0.7f);
		Spark.StartCm = Spark.EndCm = bEmber ? 2.0f : 3.0f;
		Spark.FireGain = bEmber ? 2.0f : 5.0f;
		Spark.FadeIn = 0.0f;
	}
}

void UBlastFxSubsystem::SpawnScorch(const FVector& Ground, const FVector& GroundNormal, float RadiusM)
{
	if (!ScorchInstances)
	{
		return;
	}
	if (Scorches.Num() >= BlastMaxScorches)
	{
		Scorches.RemoveAt(0);
	}
	// The drawn ground can stand above the sim's height there; find the surface that is really drawn (visible static or
	// dynamic geometry, not a pawn and not the debris' hidden floor), as the blood's stains do. PC rounds 3 to 6: the
	// mark was never seen.
	FVector At = Ground;
	FVector Normal = GroundNormal;
	if (UWorld* World = GetWorld())
	{
		TArray<FHitResult> Hits;
		FCollisionObjectQueryParams Objects;
		Objects.AddObjectTypesToQuery(ECC_WorldStatic);
		Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(BlastScorchGround), true);
		World->LineTraceMultiByObjectType(Hits, Ground + FVector(0.0, 0.0, 300.0), Ground - FVector(0.0, 0.0, 400.0), Objects, Params);
		for (const FHitResult& Hit : Hits)
		{
			const UPrimitiveComponent* Component = Hit.GetComponent();
			if (Component && Component->IsVisible() && !Component->bHiddenInGame && !Cast<APawn>(Hit.GetActor()))
			{
				At = Hit.ImpactPoint;
				Normal = Hit.ImpactNormal.Z > 0.5 ? FVector(Hit.ImpactNormal) : GroundNormal;
				break;
			}
		}
		UE_LOG(LogBlastFx, Display, TEXT("Scorch at %s (sim ground %.0f cm, %d hits below)"), *At.ToString(), Ground.Z, Hits.Num());
	}
	const float SizeCm = RadiusM * 200.0f * Random.FRandRange(0.7f, 0.9f);
	// Flat on the ground (the plane's +Z along the ground's normal), turned at random, a few centimetres up so it never
	// sinks into the terrain between its corners.
	const float Turn = Random.FRand() * 2.0f * PI;
	const FVector Along = FVector::VectorPlaneProject(FVector(FMath::Cos(Turn), FMath::Sin(Turn), 0.0f), Normal).GetSafeNormal();
	const FQuat Facing = FRotationMatrix::MakeFromZX(Normal, Along).ToQuat();
	FScorch& Mark = Scorches.AddDefaulted_GetRef();
	Mark.Where = FTransform(Facing, At + Normal * 4.0, FVector(SizeCm / 100.0f, SizeCm / 100.0f, 1.0f));
	Mark.Strength = Random.FRandRange(0.85f, 1.0f);
}

void UBlastFxSubsystem::ClearForTest()
{
	Teardown();
}

void UBlastFxSubsystem::AddScorchForTest(float MapX, float MapY, float RadiusM)
{
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim() || !EnsureScene())
	{
		return;
	}
	SpawnScorch(Source->SimToWorld(MapX, MapY, 0), FVector::UpVector, RadiusM);
}

void UBlastFxSubsystem::AddDebrisTrail(const FVector& At, float SizeCm)
{
	if (!Holder || Sprites.Num() >= GBlastMaxSprites.GetValueOnGameThread())
	{
		return;
	}
	FSprite& Dust = Add(EKind::Smoke);
	Dust.Position = At;
	Dust.Velocity = FVector(0.0, 0.0, 20.0);
	Dust.Drag = 1.0f;
	Dust.Wind = 0.5f;
	Dust.StartCm = SizeCm;
	Dust.EndCm = SizeCm * 3.5f;
	Dust.Life = Random.FRandRange(0.9f, 1.5f);
	Dust.FadeIn = 0.05f;
	Dust.Opacity = 0.45f;
	Dust.FrameFrom = 10.0f;
	Dust.FrameTo = 63.0f;
	Dust.Tint = BlastSoil;
}

int32 UBlastFxSubsystem::GetSmokeAlive() const
{
	int32 Count = 0;
	for (const FSprite& Sprite : Sprites)
	{
		Count += Sprite.Kind == EKind::Smoke && Sprite.Age >= 0.0f ? 1 : 0;
	}
	return Count;
}

// ---------------------------------------------------------------------------
// Each frame.

void UBlastFxSubsystem::UpdateSprites(float DeltaTime, float Wind)
{
	APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	const FVector CameraAt = Camera ? Camera->GetCameraLocation() : FVector::ZeroVector;
	const FVector CameraRight = Camera ? FRotationMatrix(Camera->GetCameraRotation()).GetUnitAxis(EAxis::Y) : FVector::RightVector;
	const float FlipV = GBlastFlipV.GetValueOnGameThread() != 0 ? 2.0f : 0.0f;

	struct FDrawn
	{
		int32 Index = 0;
		double Distance = 0.0;
	};
	TArray<FDrawn> ByKind[static_cast<int32>(EKind::Count)];
	for (int32 Index = Sprites.Num() - 1; Index >= 0; --Index)
	{
		FSprite& Sprite = Sprites[Index];
		Sprite.Age += DeltaTime;
		if (Sprite.Age >= Sprite.Life)
		{
			Sprites.RemoveAtSwap(Index);
			continue;
		}
		if (Sprite.Age < 0.0f)
		{
			continue;
		}
		Sprite.Velocity.Z -= Sprite.Gravity * DeltaTime;
		Sprite.Velocity *= FMath::Max(0.0f, 1.0f - Sprite.Drag * DeltaTime);
		// The wind carries it along +Y (as the fire's smoke), easing in.
		Sprite.Velocity.Y += (Wind * 60.0f - Sprite.Velocity.Y) * FMath::Min(1.0f, Sprite.Wind * 0.4f * DeltaTime);
		Sprite.Position += Sprite.Velocity * DeltaTime;
		Sprite.Roll += Sprite.Spin * DeltaTime;
	}
	for (int32 Index = 0; Index < Sprites.Num(); ++Index)
	{
		if (Sprites[Index].Age >= 0.0f)
		{
			ByKind[static_cast<int32>(Sprites[Index].Kind)].Add(FDrawn{Index, FVector::DistSquared(Sprites[Index].Position, CameraAt)});
		}
	}

	for (int32 Kind = 0; Kind < static_cast<int32>(EKind::Count); ++Kind)
	{
		UInstancedStaticMeshComponent* Component = Instances.IsValidIndex(Kind) ? Instances[Kind].Get() : nullptr;
		if (!Component)
		{
			continue;
		}
		TArray<FDrawn>& Drawn = ByKind[Kind];
		if (Drawn.IsEmpty() && Component->GetInstanceCount() == 0)
		{
			continue;
		}
		// Far to near, so see-through layers blend right.
		Drawn.Sort([](const FDrawn& A, const FDrawn& B) { return A.Distance > B.Distance; });
		TArray<FTransform> Transforms;
		Transforms.Reserve(Drawn.Num());
		const int32 Floats = Component->NumCustomDataFloats;
		TArray<float> Data;
		Data.SetNumZeroed(Drawn.Num() * Floats);
		for (int32 I = 0; I < Drawn.Num(); ++I)
		{
			const FSprite& Sprite = Sprites[Drawn[I].Index];
			const float T = FMath::Clamp(Sprite.Age / Sprite.Life, 0.0f, 1.0f);
			const FVector ToCamera = (CameraAt - Sprite.Position).GetSafeNormal();
			const float FadeIn = Sprite.FadeIn > 0.0f ? FMath::Min(1.0f, Sprite.Age / Sprite.FadeIn) : 1.0f;
			float* Out = Data.GetData() + I * Floats;
			if (Sprite.Kind == EKind::Spark)
			{
				// A streak along the way it flies, as long as it travels in about 1/25 s.
				FVector Along = FVector::VectorPlaneProject(Sprite.Velocity, ToCamera);
				const float Length = FMath::Max(6.0f, static_cast<float>(Along.Size()) * 0.04f);
				Along = Along.GetSafeNormal();
				if (Along.IsNearlyZero())
				{
					Along = CameraRight;
				}
				const FQuat Facing = FRotationMatrix::MakeFromZX(ToCamera, Along).ToQuat();
				Transforms.Add(FTransform(Facing, Sprite.Position, FVector(Length / 100.0f, Sprite.StartCm / 100.0f, 1.0f)));
				Out[0] = Sprite.FireGain * (1.0f - T * T);
				Out[1] = 1.0f;
				continue;
			}
			const float SizeCm = FMath::Lerp(Sprite.StartCm, Sprite.EndCm, 1.0f - FMath::Square(1.0f - T));
			const FQuat Facing = FQuat(ToCamera, FMath::DegreesToRadians(Sprite.Roll)) * FRotationMatrix::MakeFromZX(ToCamera, CameraRight).ToQuat();
			Transforms.Add(FTransform(Facing, Sprite.Position, FVector(SizeCm / 100.0f, SizeCm / 100.0f, 1.0f)));
			if (Sprite.Kind == EKind::Haze)
			{
				Out[0] = Sprite.Opacity * (1.0f - T);
				continue;
			}
			// Smoke fades out over the last 40% of its life; the fireball's own flipbook thins out, so only its last 15%.
			const float FadeFrom = Sprite.Kind == EKind::Fire ? 0.85f : 0.6f;
			const float FadeOut = T > FadeFrom ? (1.0f - T) / (1.0f - FadeFrom) : 1.0f;
			Out[BlastFxData::DataFrame] = FMath::Lerp(Sprite.FrameFrom, Sprite.FrameTo, T);
			Out[BlastFxData::DataOpacity] = Sprite.Opacity * FadeIn * FadeOut;
			Out[BlastFxData::DataTintR] = Sprite.Tint.R;
			Out[BlastFxData::DataTintG] = Sprite.Tint.G;
			Out[BlastFxData::DataTintB] = Sprite.Tint.B;
			Out[BlastFxData::DataFireGain] = Sprite.FireGain;
			Out[BlastFxData::DataFlip] = Sprite.Flip + FlipV;
			Out[BlastFxData::DataGlow] = Sprite.Glow * FMath::Max(0.0f, 1.0f - Sprite.Age / 1.2f);
		}
		Component->ClearInstances();
		if (!Transforms.IsEmpty())
		{
			Component->AddInstances(Transforms, false, true);
			for (int32 I = 0; I < Transforms.Num(); ++I)
			{
				Component->SetCustomData(I, TArrayView<const float>(Data.GetData() + I * Floats, Floats), false);
			}
			Component->MarkRenderStateDirty();
		}
	}
}

void UBlastFxSubsystem::UpdateLight(float DeltaTime)
{
	FlashAge += DeltaTime;
	if (!FlashLight)
	{
		return;
	}
	// A white-hot peak gone in a twentieth of a second, under it an orange glow fading over a second (a molotov has only
	// the glow).
	// A grenade's fireball is a couple of square metres at a few hundred thousand nits: about a million candelas. Less here so
	// a night blast doesn't white out the screen; at noon it still lights the ground a few percent of the sun (PC round 2:
	// 14,000 cd did nothing visible in daylight).
	const float Scale = FlashScale * FMath::Max(GBlastLightScale.GetValueOnGameThread(), 0.0f);
	const float Peak = bFlashFire ? 0.0f : 400000.0f * Scale * FMath::Exp(-FlashAge / 0.05f);
	const float Glow = (bFlashFire ? 50000.0f : 60000.0f) * Scale * FMath::Exp(-FlashAge / 0.45f);
	FlashCandelas = FlashAge < 3.0f ? Peak + Glow : 0.0f;
	const bool bOn = FlashCandelas > 5.0f;
	if (FlashLight->IsVisible() != bOn)
	{
		FlashLight->SetVisibility(bOn);
	}
	if (bOn)
	{
		const float White = Peak / FMath::Max(Peak + Glow, 1.0f);
		FlashLight->SetIntensity(FlashCandelas);
		FlashLight->SetLightColor(FMath::Lerp(FLinearColor(1.0f, 0.48f, 0.16f), FLinearColor(1.0f, 0.9f, 0.75f), White));
		// Smoke rising over the fire carries the glow up a little.
		FlashLight->SetWorldLocation(FlashAt + FVector(0.0, 0.0, FMath::Min(FlashAge, 1.0f) * 80.0f));
	}
}

void UBlastFxSubsystem::UpdateScorches(float DeltaTime)
{
	if (!ScorchInstances)
	{
		return;
	}
	const float Seconds = FMath::Max(GBlastScorchSeconds.GetValueOnGameThread(), 1.0f);
	for (int32 Index = Scorches.Num() - 1; Index >= 0; --Index)
	{
		Scorches[Index].Age += DeltaTime;
		if (Scorches[Index].Age >= Seconds)
		{
			Scorches.RemoveAt(Index);
		}
	}
	if (Scorches.IsEmpty() && ScorchInstances->GetInstanceCount() == 0)
	{
		return;
	}
	TArray<FTransform> Transforms;
	Transforms.Reserve(Scorches.Num());
	for (const FScorch& Mark : Scorches)
	{
		Transforms.Add(Mark.Where);
	}
	ScorchInstances->ClearInstances();
	if (!Transforms.IsEmpty())
	{
		ScorchInstances->AddInstances(Transforms, false, true);
		for (int32 I = 0; I < Scorches.Num(); ++I)
		{
			// Fades slowly at first, quicker at the end (as soot is scuffed and rained off).
			const float Left = 1.0f - Scorches[I].Age / Seconds;
			ScorchInstances->SetCustomDataValue(I, 0, Scorches[I].Strength * FMath::Sqrt(Left), false);
			ScorchInstances->SetCustomDataValue(I, 1, Scorches[I].Age, false);
		}
		ScorchInstances->MarkRenderStateDirty();
		if (!bLoggedScorchDraw)
		{
			bLoggedScorchDraw = true;
			const FBoxSphereBounds Bounds = ScorchInstances->CalcBounds(ScorchInstances->GetComponentTransform());
			UE_LOG(LogBlastFx, Display, TEXT("Scorch drawn: %d instance(s), visible %d, bounds centre %s extent %s, fade %.2f"), ScorchInstances->GetInstanceCount(),
				ScorchInstances->IsVisible() ? 1 : 0, *Bounds.Origin.ToString(), *Bounds.BoxExtent.ToString(), Scorches[0].Strength);
		}
	}
}

void UBlastFxSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim())
	{
		return;
	}
	if (Source->GetSimGeneration() != LastGeneration)
	{
		LastGeneration = Source->GetSimGeneration();
		Teardown();
	}
	if (!Holder)
	{
		return;
	}
	// Frozen while the game is paused, real time otherwise (as the fire).
	const float Step = Source->GetSpeed() > 0 ? DeltaTime : 0.0f;
	const float Wind = static_cast<float>(Source->GetSim()->weather().wind());
	UpdateSprites(Step, Wind);
	UpdateLight(Step);
	UpdateScorches(Step);
}
