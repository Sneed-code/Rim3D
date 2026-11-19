#include "FightSubsystem.h"
#include "SeveredPieceActor.h"

#include "BloodSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DecalComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "GunRangeSubsystem.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include <imgui.h>

#include "KeyBindings.h"
#include "OrdersSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "VisualMesh.h"
#include "WeaponDrawSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Health.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Explosives.h"
#include "ColonySim/Map.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogFights, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GFightEffects(
		TEXT("Fight.Effects"), 1, TEXT("1 draws tracers, muzzle flashes, impacts, wounds and ragdolls for the sim's fights, 0 draws none."), ECVF_Default);
	TAutoConsoleVariable<int32> GFightHitboxes(
		TEXT("Fight.Hitboxes"), 0, TEXT("1 draws every pawn's hitboxes where the sim has them (colour by body part)."), ECVF_Default);
	TAutoConsoleVariable<int32> GFightDamageNumbers(
		TEXT("Fight.DamageNumbers"), 1, TEXT("1 floats the damage of your own hits over where they landed (playing as a pawn), 0 hides them."), ECVF_Default);
	constexpr float FightDamageNumberSeconds = 1.0f;
	constexpr float FightAlertFlashSeconds = 2.5f;  // how long the "!" stays over a newly alerted enemy

	const float FightMuzzleSmokeMinCameraCm = 150.0f;
	const TCHAR* const FightSolidMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");
	const TCHAR* const FightDecalMaterialPath = TEXT("/Game/Rim3D/Generated/Fights/M_WoundDecal.M_WoundDecal");

	// How far the crosshair reaches, metres.
	constexpr float FightAimRangeM = 150.0f;
	// How far the drawn body may move on between the trigger and the bullet's arrival before a hit on it counts as off
	// the body in the hit check (docs/design/aiming.md "Shots go where the crosshair is").
	constexpr double FightDrawnDriftCm = 5.0;
	// A gun lying this close (metres) is picked up by E.
	constexpr float FightEquipRangeM = 8.0f;
	// Bullets that never report a hit are dropped after this long (real seconds) or this far (cm).
	constexpr float FightTracerMaxAge = 6.0f;
	constexpr float FightTracerMaxDistance = 20000.0f;
	constexpr int32 FightMaxTracers = 400;
	constexpr int32 FightMaxParticles = 1800;
	constexpr int32 FightMaxDecals = 160;
	constexpr int32 FightLightCount = 6;
	constexpr float FightLightSeconds = 0.09f;
	constexpr float FightLightIntensity = 2500.0f;
	// Cm per second of velocity change a hit gives a falling pawn, per point of damage.
	constexpr float FightImpulsePerDamage = 25.0f;
	constexpr float FightImpulseMax = 700.0f;

	struct FFightParticleLook
	{
		FLinearColor Color;
		bool bSphere;
	};

	FFightParticleLook FightParticleLook(EFightParticle Kind)
	{
		switch (Kind)
		{
			case EFightParticle::Dust: return {FLinearColor(0.42f, 0.34f, 0.24f), false};
			case EFightParticle::Chip: return {FLinearColor(0.32f, 0.32f, 0.34f), false};
			case EFightParticle::Spark: return {FLinearColor(1.0f, 0.62f, 0.18f), false};
			case EFightParticle::Blood: return {FLinearColor(0.42f, 0.01f, 0.01f), true};  // drops, stretched along their flight
			case EFightParticle::Smoke: return {FLinearColor(0.36f, 0.36f, 0.38f), true};  // opaque mesh, so kept small and dim
			default: return {FLinearColor(1.0f, 0.92f, 0.62f), true};
		}
	}

	FVector FightSimToWorldUnits(const USimSubsystem& Bridge, float X, float Y, float Z)
	{
		return Bridge.GetWorldOrigin() + FVector(X * 100.0, Y * 100.0, Z * 100.0);
	}

	/** Cm from the point (sim metres) to a hitbox shape (0 on or inside it). */
	float FightHitboxDistanceCm(const colony::HitboxView& H, float X, float Y, float Z)
	{
		const FVector P(X, Y, Z);
		if (!H.box)
		{
			const FVector A(H.ax, H.ay, H.az);
			const FVector AB = FVector(H.bx, H.by, H.bz) - A;
			const double LengthSquared = AB.SizeSquared();
			const double T = LengthSquared > 1.0e-8 ? FMath::Clamp(FVector::DotProduct(P - A, AB) / LengthSquared, 0.0, 1.0) : 0.0;
			return static_cast<float>(FMath::Max(0.0, FVector::Dist(P, A + AB * T) - H.radius) * 100.0);
		}
		const FVector Forward(H.forwardX, H.forwardY, 0.0);
		const FVector Left(-H.forwardY, H.forwardX, 0.0);
		const FVector D = P - FVector(H.ax, H.ay, H.az);
		const FVector Local(FVector::DotProduct(D, Forward), FVector::DotProduct(D, Left), D.Z);
		const FVector Outside(FMath::Max(0.0, FMath::Abs(Local.X) - H.bx), FMath::Max(0.0, FMath::Abs(Local.Y) - H.by), FMath::Max(0.0, FMath::Abs(Local.Z) - H.bz));
		return static_cast<float>(Outside.Size() * 100.0);
	}

	void FightText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}
}

bool UFightSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UFightSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFightSubsystem, STATGROUP_Tickables);
}

void UFightSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		BridgeSource = Bridge;
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &UFightSubsystem::OnSimEvent);
	}
	Collection.InitializeDependency<USimPawnSubsystem>();
	Collection.InitializeDependency<UProtagonistSubsystem>();
}

void UFightSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = BridgeSource.Get())
	{
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	Super::Deinitialize();
}

bool UFightSubsystem::EffectsOn() const
{
	return GFightEffects.GetValueOnGameThread() != 0;
}

// ---------------------------------------------------------------------------
// The things the effects are drawn with.

UInstancedStaticMeshComponent* UFightSubsystem::MakeInstanced(UStaticMesh* Mesh)
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

void UFightSubsystem::EnsureEffects()
{
	if (Holder && SolidMaterial)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (!Holder)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
		USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("Root"));
		Holder->SetRootComponent(Root);
		Root->RegisterComponent();
	}
	SolidMaterial = LoadObject<UMaterialInterface>(nullptr, FightSolidMaterialPath);
	if (!SolidMaterial)
	{
		UE_LOG(LogFights, Error, TEXT("Could not load %s; fight effects cannot be drawn"), FightSolidMaterialPath);
		return;
	}

	const auto MakeBox = [this](float X, float Y, float Z, const FLinearColor& Color, bool bSphere)
	{
		VisualMesh::FPart Part;
		Part.Shape = bSphere ? VisualMesh::EShape::Sphere : VisualMesh::EShape::Box;
		Part.Size = FVector3f(X, Y, Z);
		Part.Color = Color;
		TArray<VisualMesh::FPart> Parts;
		Parts.Add(Part);
		UStaticMesh* Mesh = VisualMesh::MakeMesh(this, SolidMaterial, Parts, VisualMesh::EPartSet::All, nullptr, false);
		if (Mesh)
		{
			KeepMeshes.Add(Mesh);
		}
		return Mesh;
	};

	// A tracer is a long thin box standing on z = 0, stretched to length per instance.
	TracerInstances = MakeInstanced(MakeBox(1.6f, 1.6f, 100.0f, FLinearColor(1.0f, 0.78f, 0.3f), false));
	ParticleInstances.Reset();
	for (int32 Kind = 0; Kind < static_cast<int32>(EFightParticle::Count); ++Kind)
	{
		const FFightParticleLook Look = FightParticleLook(static_cast<EFightParticle>(Kind));
		ParticleInstances.Add(MakeInstanced(Look.bSphere ? MakeBox(5.0f, 5.0f, 10.0f, Look.Color, true) : MakeBox(10.0f, 10.0f, 10.0f, Look.Color, false)));
	}

	Lights.Reset();
	LightAges.Reset();
	for (int32 I = 0; I < FightLightCount; ++I)
	{
		UPointLightComponent* Light = NewObject<UPointLightComponent>(Holder);
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetIntensity(0.0f);
		Light->SetLightColor(FLinearColor(1.0f, 0.72f, 0.38f));
		Light->SetAttenuationRadius(700.0f);
		Light->SetCastShadows(false);
		Light->SetupAttachment(Holder->GetRootComponent());
		Light->RegisterComponent();
		Light->SetVisibility(false);
		Lights.Add(Light);
		LightAges.Add(FightLightSeconds);
	}
}

void UFightSubsystem::ResetEffects()
{
	Tracers.Reset();
	Particles.Reset();
	RecentImpulse.Reset();
	HitLog.Reset();
	for (TWeakObjectPtr<UDecalComponent>& Decal : Decals)
	{
		if (Decal.IsValid())
		{
			Decal->DestroyComponent();
		}
	}
	Decals.Reset();
	if (TracerInstances)
	{
		TracerInstances->ClearInstances();
	}
	for (UInstancedStaticMeshComponent* Component : ParticleInstances)
	{
		if (Component)
		{
			Component->ClearInstances();
		}
	}
	for (UPointLightComponent* Light : Lights)
	{
		if (Light)
		{
			Light->SetVisibility(false);
		}
	}
}

// ---------------------------------------------------------------------------
// Sim events.

void UFightSubsystem::OnSimEvent(const colony::Event& Event)
{
	// Stealth: an enemy that has just been alerted gets a "!" over its head for a moment.
	if (Event.type == colony::EventType::PawnAction && Event.action == colony::PawnAction::Alert)
	{
		++AlertEventsSeen;
		if (Event.amount >= 2.0f)
		{
			AlertFlash.Add(entt::to_integral(Event.thing), 0.0f);
		}
		else
		{
			AlertFlash.Remove(entt::to_integral(Event.thing));
		}
		return;
	}
	if (Event.type == colony::EventType::ShotFired)
	{
		OnShotFired(Event);
	}
	else if (Event.type == colony::EventType::ProjectileHit)
	{
		OnProjectileHit(Event);
	}
	else if (Event.type == colony::EventType::PawnDowned || Event.type == colony::EventType::PawnDied || Event.type == colony::EventType::PawnPartLost)
	{
		// The hit that did this was checked against hitboxes that are already gone or lying down: not a fair check.
		const USimSubsystem* Bridge = BridgeSource.Get();
		for (FFightHitRecord& Record : HitLog)
		{
			if (Bridge && Record.Pawn == Event.thing && Record.GameSeconds == Bridge->GetSteps())
			{
				Record.MissCm = -1.0f;
			}
		}
	}
}

void UFightSubsystem::OnShotFired(const colony::Event& Event)
{
	USimSubsystem* Bridge = BridgeSource.Get();
	++ShotsSeen;
	if (!Bridge || !EffectsOn())
	{
		return;
	}
	EnsureEffects();
	FVector Muzzle = FightSimToWorldUnits(*Bridge, Event.x, Event.y, Event.z);
	// The bullet leaves from the drawn gun's muzzle (sneed): the flash and the tracer start there and fly to where the
	// sim's bullet lands.
	if (FindDrawnMuzzle(Event.thing, Muzzle))
	{
		++MuzzleShots;
	}
	SpawnFlash(Muzzle);
	if (Tracers.Num() < FightMaxTracers)
	{
		FFightTracer Tracer;
		Tracer.Id = Event.target;
		Tracer.Shooter = Event.thing;
		Tracer.Start = Muzzle;
		Tracers.Add(Tracer);
		++TracersSpawned;
	}
}

void UFightSubsystem::OnProjectileHit(const colony::Event& Event)
{
	USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	++HitsSeen;
	const bool bOnPawn = Event.hit == colony::ProjectileHitKind::Pawn || Event.hit == colony::ProjectileHitKind::Armor;
	if (bOnPawn)
	{
		// Does the hit point lie on the body the sim says was hit? (the hitbox debug view's number)
		++PawnHitsSeen;
		FFightHitRecord Record;
		Record.Pawn = Event.target;
		Record.Kind = Event.hit;
		Record.Part = Event.part;
		Record.Amount = Event.amount;
		Record.Where = FVector(Event.x, Event.y, Event.z);
		Record.GameSeconds = Bridge->GetSteps();
		Record.MissCm = DistanceToHitboxesCm(Event.target, Event.x, Event.y, Event.z);
		Record.StockCm = Record.MissCm;
		if (const colony::ThingView* Hit = Bridge->GetCurrentSnapshot().find(Event.target))
		{
			Record.SimAt = FVector(Hit->x, Hit->y, 0.0f);
			Record.SimYaw = FMath::RadiansToDegrees(FMath::Atan2(Hit->facingY, Hit->facingX));
		}
		// The player's shots hit the body as drawn (docs/design/aiming.md "Shots go where the crosshair is"): a hit on the
		// drawn body counts as on the body, whichever is nearer.
		if (const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
		{
			if (const ASimPawnActor* Actor = Pawns->FindPawn(Event.target))
			{
				const double Drawn = Actor->DistanceToDrawnBodyCm(FightSimToWorldUnits(*Bridge, Event.x, Event.y, Event.z));
				Record.DrawnCm = static_cast<float>(Drawn);
				Record.DrawnAt = Actor->GetActorLocation();
				Record.DrawnYaw = Actor->GetActorRotation().Yaw;
				// The spot is on the drawn body when the trigger is pulled; by the time the bullet arrives (a step or two) the
				// body has moved on in its clip and the wobble from the shot before springs back (PC: 7 to 8 cm at worst,
				// drawn and sim positions and yaws equal). That much is allowed before it counts as a miss.
				const float Allowed = static_cast<float>(FMath::Max(0.0, Drawn - FightDrawnDriftCm));
				if (Drawn >= 0.0 && (Record.MissCm < 0.0f || Allowed < Record.MissCm))
				{
					Record.MissCm = Allowed;
				}
			}
		}
		if (HitLog.Num() < 512)
		{
			HitLog.Add(Record);
		}
	}
	if (!EffectsOn())
	{
		return;
	}
	EnsureEffects();

	const FVector Where = FightSimToWorldUnits(*Bridge, Event.x, Event.y, Event.z);
	for (FFightTracer& Tracer : Tracers)
	{
		if (Tracer.Id == Event.thing)
		{
			Tracer.bEnded = true;
			Tracer.End = Where;
			Tracer.HitKind = Event.hit;
			Tracer.Target = Event.target;
			Tracer.Part = Event.part;
			Tracer.Amount = Event.amount;
			return;
		}
	}
	// A bullet nobody drew (a loaded game, too many tracers): the impact happens at once.
	FFightTracer Immediate;
	Immediate.Id = Event.thing;
	Immediate.Start = Where;
	Immediate.Dir = -FVector::UpVector;
	Immediate.End = Where;
	Immediate.bEnded = true;
	Immediate.HitKind = Event.hit;
	Immediate.Target = Event.target;
	Immediate.Part = Event.part;
	Immediate.Amount = Event.amount;
	SpawnImpact(Immediate);
}

FString UFightSubsystem::DescribeWorstHit() const
{
	const FFightHitRecord* Worst = nullptr;
	for (const FFightHitRecord& Record : HitLog)
	{
		if (Record.MissCm >= 0.0f && (!Worst || Record.MissCm > Worst->MissCm))
		{
			Worst = &Record;
		}
	}
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!Worst || !Bridge)
	{
		return FString();
	}
	const FVector HitWorld = FightSimToWorldUnits(*Bridge, Worst->Where.X, Worst->Where.Y, Worst->Where.Z);
	const FVector SimWorld = FightSimToWorldUnits(*Bridge, Worst->SimAt.X, Worst->SimAt.Y, 0.0f);
	FString Part = FString::FromInt(Worst->Part);
	if (const colony::ThingView* Hit = Bridge->GetCurrentSnapshot().find(Worst->Pawn); Hit && Bridge->GetSim() && Worst->Part >= 0)
	{
		const std::vector<colony::BodyPartInfo> Parts = Bridge->GetSim()->health().bodyParts(Hit->def);
		if (Worst->Part < static_cast<int32>(Parts.size()))
		{
			Part = UTF8_TO_TCHAR(Parts[static_cast<size_t>(Worst->Part)].label.c_str());
		}
	}
	return FString::Printf(TEXT("worst hit: pawn %u part %s at sim (%.2f, %.2f, %.2f); %.1f cm from the sim hitboxes, %.1f cm from the drawn body; ")
		TEXT("drawn body at (%.0f, %.0f) yaw %.0f, sim pawn at (%.0f, %.0f) yaw %.0f (world cm): %.1f cm apart, %.0f deg apart; hit %.0f cm above the drawn feet"),
		static_cast<uint32>(entt::to_integral(Worst->Pawn)), *Part, Worst->Where.X, Worst->Where.Y, Worst->Where.Z, Worst->StockCm, Worst->DrawnCm,
		Worst->DrawnAt.X, Worst->DrawnAt.Y, Worst->DrawnYaw, SimWorld.X, SimWorld.Y, Worst->SimYaw, FVector::Dist2D(Worst->DrawnAt, SimWorld),
		FMath::Abs(FRotator::NormalizeAxis(Worst->DrawnYaw - Worst->SimYaw)), HitWorld.Z - Worst->DrawnAt.Z);
}

float UFightSubsystem::DistanceToHitboxesCm(colony::Entity Pawn, float X, float Y, float Z) const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetSim())
	{
		return -1.0f;
	}
	std::vector<colony::HitboxView> Boxes;
	if (!Bridge->GetSim()->combat().hitboxesOf(Pawn, Boxes) || Boxes.empty())
	{
		return -1.0f;
	}
	float Best = TNumericLimits<float>::Max();
	for (const colony::HitboxView& Box : Boxes)
	{
		Best = FMath::Min(Best, FightHitboxDistanceCm(Box, X, Y, Z));
	}
	return Best;
}

// ---------------------------------------------------------------------------
// Making effects.

void UFightSubsystem::SpawnBurst(EFightParticle Kind, const FVector& At, const FVector& Direction, int32 Count, float Speed, float Spread, float SizeCm, float Life, float Gravity)
{
	for (int32 I = 0; I < Count && Particles.Num() < FightMaxParticles; ++I)
	{
		FParticle Particle;
		Particle.Kind = Kind;
		Particle.Position = At;
		Particle.Velocity = (Direction.GetSafeNormal() + Random.VRand() * Spread).GetSafeNormal() * Speed * Random.FRandRange(0.4f, 1.0f);
		Particle.SizeCm = SizeCm * Random.FRandRange(0.6f, 1.2f);
		Particle.Life = Life * Random.FRandRange(0.6f, 1.1f);
		Particle.Gravity = Gravity;
		Particle.Drag = Kind == EFightParticle::Smoke ? 1.5f : 0.6f;
		Particles.Add(Particle);
	}
}

void UFightSubsystem::SpawnFlash(const FVector& Muzzle)
{
	++Flashes;
	// The flash and smoke shapes are opaque meshes, so they stay small, and none are drawn right in front of the camera
	// (a first-person gun's muzzle is a hand's length from the eye, where they fill the screen as solid blobs); the
	// flash light below still lights the scene there.
	const APlayerCameraManager* FlashCamera = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	const bool bNearCamera = FlashCamera && FVector::Dist(FlashCamera->GetCameraLocation(), Muzzle) <= FightMuzzleSmokeMinCameraCm;
	if (!bNearCamera && Particles.Num() + 4 < FightMaxParticles)
	{
		FParticle Big;
		Big.Kind = EFightParticle::Flash;
		Big.Position = Muzzle;
		Big.SizeCm = 9.0f;
		Big.Life = 0.04f;
		Big.Gravity = 0.0f;
		Particles.Add(Big);
		FParticle Small = Big;
		Small.SizeCm = 5.0f;
		Small.Life = 0.08f;
		Particles.Add(Small);
	}
	if (!bNearCamera)
	{
		SpawnBurst(EFightParticle::Smoke, Muzzle, FVector::UpVector, 3, 60.0f, 0.6f, 2.5f, 0.5f, -40.0f);
	}
	if (Lights.Num() > 0)
	{
		const int32 Index = NextLight++ % Lights.Num();
		if (Lights[Index])
		{
			Lights[Index]->SetWorldLocation(Muzzle);
			Lights[Index]->SetVisibility(true);
			Lights[Index]->SetIntensity(FightLightIntensity);
			LightAges[Index] = 0.0f;
		}
	}
}

void UFightSubsystem::SpawnImpact(const FFightTracer& Tracer)
{
	++Impacts;
	++ImpactsByKind[FMath::Clamp(static_cast<int32>(Tracer.HitKind), 0, 7)];
	const FVector Dir = Tracer.Dir.GetSafeNormal().IsNearlyZero() ? -FVector::UpVector : Tracer.Dir.GetSafeNormal();
	const FVector Back = -Dir;
	// The sim's point lies on its hitbox, which a slim drawn body may sit inside: on a body hit, where the bullet's line
	// first meets the drawn body (the animated mannequin's physics shapes, which the MakeHuman body follows), so the
	// wound and the spray come out of the skin where it was shot.
	FVector At = Tracer.End;
	if ((Tracer.HitKind == colony::ProjectileHitKind::Pawn || Tracer.HitKind == colony::ProjectileHitKind::Armor) && Tracer.bDirKnown)
	{
		if (const USimPawnSubsystem* SnapPawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
		{
			FVector OnSkin;
			ASimPawnActor* Victim = SnapPawns->FindPawn(Tracer.Target);
			if (Victim && Victim->TraceDrawnBody(Tracer.End - Dir * 80.0f, Tracer.End + Dir * 60.0f, OnSkin))
			{
				At = OnSkin;
				++SnappedHits;
				LastBodyHits.Add(entt::to_integral(Tracer.Target), At);
			}
		}
	}
	// Your bullet passed through a body lying on the ground (a dead one, which the sim no longer counts, or a downed one
	// whose ragdoll lies off its hitboxes): the blood, a wound and a push, where the line met the body. Cosmetic; the sim
	// decided the bullet's hit, and a dead body has nothing left for it to decide.
	if (Tracer.HitKind != colony::ProjectileHitKind::Pawn && Tracer.HitKind != colony::ProjectileHitKind::Armor)
	{
		SpawnBodyThroughHit(Tracer);
	}
	switch (Tracer.HitKind)
	{
		case colony::ProjectileHitKind::Ground:
			SpawnBurst(EFightParticle::Dust, At, FVector::UpVector, 9, 300.0f, 0.8f, 8.0f, 0.9f, 980.0f);
			SpawnBurst(EFightParticle::Smoke, At, FVector::UpVector, 3, 60.0f, 0.5f, 5.0f, 0.8f, -30.0f);
			break;
		case colony::ProjectileHitKind::Rock:
			SpawnBurst(EFightParticle::Dust, At, Back + FVector::UpVector * 0.5f, 6, 280.0f, 0.8f, 8.0f, 0.8f, 980.0f);
			SpawnBurst(EFightParticle::Chip, At, Back + FVector::UpVector * 0.5f, 5, 380.0f, 0.7f, 5.0f, 0.7f, 980.0f);
			SpawnBurst(EFightParticle::Spark, At, Back, 3, 450.0f, 0.6f, 2.5f, 0.3f, 980.0f);
			break;
		case colony::ProjectileHitKind::Building:
			SpawnBurst(EFightParticle::Spark, At, Back + FVector::UpVector * 0.3f, 8, 480.0f, 0.7f, 2.5f, 0.35f, 980.0f);
			SpawnBurst(EFightParticle::Chip, At, Back + FVector::UpVector * 0.3f, 6, 320.0f, 0.8f, 5.0f, 0.7f, 980.0f);
			SpawnBurst(EFightParticle::Dust, At, Back, 3, 120.0f, 0.6f, 9.0f, 0.7f, 100.0f);
			break;
		case colony::ProjectileHitKind::Armor:
			SpawnBurst(EFightParticle::Spark, At, Back, 12, 560.0f, 0.9f, 3.0f, 0.35f, 980.0f);
			if (const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
			{
				if (ASimPawnActor* Actor = Pawns->FindPawn(Tracer.Target))
				{
					Actor->PlayHitReaction(Dir, 0.3f);
					++HitReactionsPlayed;
					// Stopped by the armour, but the body still feels it: a small wobble where it landed (body-health.md part E).
					if (const USimSubsystem* KickBridge = BridgeSource.Get())
					{
						Actor->KickZone(*KickBridge, Tracer.Part, At, Dir, 0.25f);
						++ZoneKicks;
					}
				}
			}
			if (const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
				Protagonist && Protagonist->IsDirectMode() && (Protagonist->GetControlled() == Tracer.Shooter || Protagonist->GetControlled() == Tracer.Target))
			{
				AddDamageNumber(Tracer, true, Protagonist->GetControlled() == Tracer.Target);
			}
			break;
		case colony::ProjectileHitKind::Pawn:
		{
			// Blood: UBloodSubsystem's spray, wounds and bleeding (docs/design/blood.md); the old bursts and decals if its
			// assets are missing or Blood.Enabled is 0.
			UBloodSubsystem* Blood = GetWorld()->GetSubsystem<UBloodSubsystem>();
			const bool bFullBlood = Blood && Blood->IsReady();
			if (!bFullBlood)
			{
				const float Wound = FMath::Clamp(Tracer.Amount / 15.0f, 0.6f, 2.0f);
				SpawnBurst(EFightParticle::Blood, At, Dir + FVector::UpVector * 0.15f, FMath::RoundToInt(22.0f * Wound), 420.0f, 0.45f, 5.0f, 0.9f, 980.0f);
				SpawnBurst(EFightParticle::Blood, At, Back + FVector::UpVector * 0.3f, FMath::RoundToInt(8.0f * Wound), 200.0f, 0.6f, 4.0f, 0.6f, 980.0f);
				SpawnBurst(EFightParticle::Blood, At, Dir, 14, 110.0f, 0.9f, 6.0f, 0.45f, 120.0f);
			}
			const float Strength = FMath::Clamp(Tracer.Amount / 20.0f, 0.35f, 1.0f);
			const USimSubsystem* StaggerBridge = BridgeSource.Get();
			const colony::ThingView* HitView = StaggerBridge && StaggerBridge->GetSim() ? StaggerBridge->GetCurrentSnapshot().find(Tracer.Target) : nullptr;
			if (const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
			{
				ASimPawnActor* Actor = Pawns->FindPawn(Tracer.Target);
				if (Actor)
				{
					// A react on the upper body (a stumble when the sim staggered it) over legs that keep what they were doing; the lean goes on top.
					if (HitView && !HitView->downed && !HitView->dead && Actor->PlayHitClip(Dir, HitView->staggeredFor > 0))
					{
						++HitClipsPlayed;
					}
					Actor->PlayHitReaction(Dir, Strength);
					++HitReactionsPlayed;
					// The struck zone is knocked along the hit and wobbles back, whatever else the body does: small hits
					// that stagger nothing too (body-health.md part E).
					if (StaggerBridge)
					{
						Actor->KickZone(*StaggerBridge, Tracer.Part, At, Dir, FMath::Clamp(Tracer.Amount / 20.0f, 0.25f, 1.0f));
						++ZoneKicks;
					}
				}
				if (bFullBlood)
				{
					FBloodHit Hit;
					Hit.At = At;
					Hit.Direction = Dir;
					Hit.Kind = EBloodWound::Auto;
					// A bullet unless the sim said otherwise (a blow's MeleeHit comes through here too, from SpawnMeleeImpact).
					Hit.Fallback = Tracer.Id != colony::kNoEntity ? EBloodWound::Gunshot : EBloodWound::Blunt;
					Hit.Damage = Tracer.Amount;
					Hit.Victim = Tracer.Target;
					Hit.Part = Tracer.Part;
					Hit.Mesh = Actor ? Actor->GetDecalMesh() : nullptr;
					Blood->AddHit(Hit);
				}
				else if (Actor)
				{
					SpawnBodyDecal(*Actor, At, Dir, Tracer.Amount);
					SpawnGroundDecal(FVector(At.X, At.Y, Actor->GetActorLocation().Z), FMath::Clamp(14.0f + Tracer.Amount * 1.2f, 16.0f, 40.0f));
				}
			}
			if (const USimSubsystem* Bridge = BridgeSource.Get())
			{
				RecentImpulse.Add(entt::to_integral(Tracer.Target), TPair<FVector, int64>(Dir * FMath::Min(Tracer.Amount * FightImpulsePerDamage, FightImpulseMax), Bridge->GetSteps()));
			}
			if (const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>(); Protagonist && Protagonist->IsDirectMode())
			{
				if (Protagonist->GetControlled() == Tracer.Shooter)
				{
					HitMarkerSeconds = 0.18f;
					++HitMarkers;
					AddDamageNumber(Tracer, false, false);
				}
				else if (Protagonist->GetControlled() == Tracer.Target)
				{
					// What a hit took from you, in red.
					AddDamageNumber(Tracer, false, true);
				}
			}
			break;
		}
		default:
			break;
	}
}

void UFightSubsystem::TrimDecals()
{
	Decals.RemoveAll([](const TWeakObjectPtr<UDecalComponent>& Decal) { return !Decal.IsValid(); });
	while (Decals.Num() > FightMaxDecals)
	{
		if (Decals[0].IsValid())
		{
			Decals[0]->DestroyComponent();
		}
		Decals.RemoveAt(0);
	}
}

void UFightSubsystem::SpawnBodyThroughHit(const FFightTracer& Tracer)
{
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!Protagonist || !Protagonist->IsDirectMode() || Protagonist->GetControlled() != Tracer.Shooter || !Bridge || !Bridge->GetSim() || !Tracer.bDirKnown)
	{
		return;
	}
	const FVector Dir = Tracer.Dir.GetSafeNormal();
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_PhysicsBody);
	Objects.AddObjectTypesToQuery(ECC_Pawn);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FightBodyThrough), false);
	TArray<FHitResult> Hits;
	GetWorld()->LineTraceMultiByObjectType(Hits, Tracer.Start, Tracer.End + Dir * 20.0f, Objects, Params);
	for (const FHitResult& Hit : Hits)
	{
		ASimPawnActor* Actor = Cast<ASimPawnActor>(Hit.GetActor());
		const colony::ThingView* View = Actor ? Bridge->GetCurrentSnapshot().find(Actor->GetThing()) : nullptr;
		if (!View || (!View->dead && !View->downed))
		{
			continue;
		}
		const FVector At = Hit.ImpactPoint;
		SpawnBurst(EFightParticle::Blood, At, Dir + FVector::UpVector * 0.3f, 14, 300.0f, 0.5f, 4.0f, 0.7f, 980.0f);
		SpawnBurst(EFightParticle::Blood, At, -Dir + FVector::UpVector * 0.4f, 6, 160.0f, 0.6f, 3.5f, 0.5f, 980.0f);
		SpawnBodyDecal(*Actor, At, Dir, 10.0f);
		if (UPrimitiveComponent* Body = Hit.GetComponent(); Body && Body->IsSimulatingPhysics(Hit.BoneName))
		{
			Body->AddImpulseAtLocation(Dir * 6000.0f, At, Hit.BoneName);
		}
		++BodyThroughHits;
		return;
	}
}

void UFightSubsystem::SpawnBodyDecal(ASimPawnActor& Actor, const FVector& At, const FVector& Direction, float Amount)
{
	if (!bDecalMaterialTried)
	{
		bDecalMaterialTried = true;
		DecalBase = LoadObject<UMaterialInterface>(nullptr, FightDecalMaterialPath);
		if (DecalBase)
		{
			WoundMaterial = UMaterialInstanceDynamic::Create(DecalBase, this);
			WoundMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.45f, 0.012f, 0.012f));
			PoolMaterial = UMaterialInstanceDynamic::Create(DecalBase, this);
			PoolMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.32f, 0.006f, 0.006f));
		}
		else
		{
			bDecalMaterialMissing = true;
			UE_LOG(LogFights, Warning, TEXT("%s is missing, so there are no wound decals (UnrealEditor-Cmd.exe Rim3D.uproject -run=FightMaterials)"), FightDecalMaterialPath);
		}
	}
	USkeletalMeshComponent* Mesh = Actor.GetDecalMesh();
	if (!WoundMaterial || !Mesh)
	{
		return;
	}
	const float Half = FMath::Clamp(5.0f + Amount * 0.5f, 6.0f, 12.0f);
	const FName Bone = Mesh->FindClosestBone(At);
	// The box straddles the body surface, its X axis along the bullet.
	UDecalComponent* Decal = UGameplayStatics::SpawnDecalAttached(WoundMaterial, FVector(14.0f, Half, Half), Mesh, Bone, At - Direction.GetSafeNormal() * 6.0f,
		Direction.Rotation(), EAttachLocation::KeepWorldPosition, 0.0f);
	if (Decal)
	{
		Decal->SetFadeScreenSize(0.0f);
		Decals.Add(Decal);
		++BodyDecals;
		TrimDecals();
	}
}

void UFightSubsystem::SpawnGroundDecal(const FVector& At, float RadiusCm)
{
	if (!PoolMaterial)
	{
		return;
	}
	// Straight down, a random turn so the blobs don't line up.
	UDecalComponent* Decal = UGameplayStatics::SpawnDecalAtLocation(GetWorld(), PoolMaterial, FVector(40.0f, RadiusCm, RadiusCm), At + FVector(Random.FRandRange(-20.0f, 20.0f), Random.FRandRange(-20.0f, 20.0f), 10.0f),
		FRotator(-90.0f, Random.FRandRange(0.0f, 360.0f), 0.0f), 0.0f);
	if (Decal)
	{
		Decal->SetFadeScreenSize(0.0f);
		Decals.Add(Decal);
		++GroundDecals;
		TrimDecals();
	}
}

int32 UFightSubsystem::GetLiveDecals() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<UDecalComponent>& Decal : Decals)
	{
		Count += Decal.IsValid() ? 1 : 0;
	}
	return Count;
}

// ---------------------------------------------------------------------------
// Per frame.

void UFightSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	if (Bridge->GetSimGeneration() != LastGeneration)
	{
		LastGeneration = Bridge->GetSimGeneration();
		ResetEffects();
	}
	UpdatePlayerInput();
	if (EffectsOn())
	{
		EnsureEffects();
		if (Holder && SolidMaterial)
		{
			const float GameDelta = DeltaTime * static_cast<float>(Bridge->GetSpeed());
			UpdateTracers(DeltaTime, GameDelta);
			UpdateParticles(GameDelta);
			UpdateLights(GameDelta);
			SyncPawns();
		}
	}
	HitMarkerSeconds = FMath::Max(0.0f, HitMarkerSeconds - DeltaTime);
	for (FFightDamageNumber& Number : DamageNumbers)
	{
		Number.Age += DeltaTime;
	}
	DamageNumbers.RemoveAll([](const FFightDamageNumber& Number) { return Number.Age >= FightDamageNumberSeconds; });
	for (auto It = AlertFlash.CreateIterator(); It; ++It)
	{
		It.Value() += DeltaTime;
		if (It.Value() >= FightAlertFlashSeconds)
		{
			It.RemoveCurrent();
		}
	}
	EquipMessageSeconds = FMath::Max(0.0f, EquipMessageSeconds - DeltaTime);
	DrawHitboxes();
	DrawHud();
}

void UFightSubsystem::UpdateTracers(float DeltaTime, float GameDelta)
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	const colony::Snapshot& Snapshot = Bridge->GetCurrentSnapshot();
	TArray<FTransform> Transforms;
	Transforms.Reserve(Tracers.Num());

	for (int32 Index = Tracers.Num() - 1; Index >= 0; --Index)
	{
		FFightTracer& Tracer = Tracers[Index];
		// Game time, so a tracer frozen by the pause stays up however long the pause lasts.
		Tracer.Age += GameDelta;

		// The bullet's own direction and speed, from the snapshot once it shows there.
		if (!Tracer.bDirKnown)
		{
			const colony::ProjectileView* Found = nullptr;
			for (const colony::ProjectileView& View : Snapshot.projectiles)
			{
				if (View.id == Tracer.Id)
				{
					Found = &View;
					break;
				}
			}
			if (Found)
			{
				const FVector Velocity(Found->vx, Found->vy, Found->vz);
				if (Velocity.SizeSquared() > 1.0e-8)
				{
					Tracer.Dir = Velocity.GetSafeNormal();
					Tracer.Speed = static_cast<float>(Velocity.Size() * 100.0 * colony::kStepsPerRealSecondAt1x);
					Tracer.bDirKnown = true;
				}
			}
			else if (Tracer.bEnded)
			{
				// Fired and stopped inside one step: it never showed in a snapshot.
				Tracer.bDirKnown = true;
			}
			else if (GameDelta > 0.0f && ++Tracer.Frames > 6)
			{
				++TracersDropped;
				Tracers.RemoveAtSwap(Index);
				continue;
			}
		}
		if (!Tracer.bDirKnown)
		{
			continue;
		}

		float ChordLength = TNumericLimits<float>::Max();
		if (Tracer.bEnded)
		{
			const FVector Chord = Tracer.End - Tracer.Start;
			ChordLength = static_cast<float>(Chord.Size());
			if (ChordLength > 1.0f)
			{
				Tracer.Dir = Chord / ChordLength;
			}
		}
		Tracer.Distance += Tracer.Speed * GameDelta;

		if (Tracer.bEnded && Tracer.Distance >= ChordLength)
		{
			++TracersArrived;
			const FFightTracer Done = Tracer;
			Tracers.RemoveAtSwap(Index);
			SpawnImpact(Done);
			continue;
		}
		if (Tracer.Age > FightTracerMaxAge || Tracer.Distance > FightTracerMaxDistance)
		{
			++TracersDropped;
			Tracers.RemoveAtSwap(Index);
			continue;
		}

		const float Length = FMath::Clamp(Tracer.Speed * GameDelta * 1.5f, 150.0f, 1800.0f);
		const float TailDistance = FMath::Max(0.0f, Tracer.Distance - Length);
		const float Segment = Tracer.Distance - TailDistance;
		if (Segment > 20.0f)
		{
			Transforms.Add(FTransform(FRotationMatrix::MakeFromZ(Tracer.Dir).ToQuat(), Tracer.Start + Tracer.Dir * TailDistance, FVector(1.0, 1.0, Segment / 100.0f)));
		}
	}

	if (TracerInstances)
	{
		TracerInstances->ClearInstances();
		if (!Transforms.IsEmpty())
		{
			TracerInstances->AddInstances(Transforms, false, true);
		}
	}
}

void UFightSubsystem::UpdateParticles(float GameDelta)
{
	TArray<TArray<FTransform>> ByKind;
	ByKind.SetNum(static_cast<int32>(EFightParticle::Count));
	for (int32 Index = Particles.Num() - 1; Index >= 0; --Index)
	{
		FParticle& Particle = Particles[Index];
		Particle.Age += GameDelta;
		if (Particle.Age >= Particle.Life)
		{
			Particles.RemoveAtSwap(Index);
			continue;
		}
		Particle.Velocity.Z -= Particle.Gravity * GameDelta;
		Particle.Velocity *= FMath::Max(0.0f, 1.0f - Particle.Drag * GameDelta);
		Particle.Position += Particle.Velocity * GameDelta;
		const float T = Particle.Age / Particle.Life;
		float Scale = Particle.SizeCm / 10.0f;
		if (Particle.Kind == EFightParticle::Flash)
		{
			Scale *= 1.0f - T * 0.7f;
		}
		else if (Particle.Kind == EFightParticle::Smoke)
		{
			Scale *= (0.5f + T * 0.8f) * (T > 0.6f ? (1.0f - T) / 0.4f : 1.0f);
		}
		else if (T > 0.7f)
		{
			Scale *= (1.0f - T) / 0.3f;
		}
		if (Particle.Kind == EFightParticle::Blood)
		{
			// The drop mesh is long along Z: point it along the velocity and stretch it with speed (a streak in flight,
			// round once it slows).
			const float Speed = Particle.Velocity.Size();
			const FRotator Along = Speed > 1.0f ? FRotationMatrix::MakeFromZ(Particle.Velocity / Speed).Rotator() : FRotator::ZeroRotator;
			const float Stretch = 0.6f + FMath::Min(Speed / 250.0f, 1.6f);
			ByKind[static_cast<int32>(Particle.Kind)].Add(FTransform(Along, Particle.Position, FVector(Scale, Scale, Scale * Stretch)));
			continue;
		}
		ByKind[static_cast<int32>(Particle.Kind)].Add(FTransform(FRotator(T * 400.0f, T * 700.0f, 0.0f), Particle.Position, FVector(Scale)));
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

void UFightSubsystem::UpdateLights(float GameDelta)
{
	for (int32 Index = 0; Index < Lights.Num(); ++Index)
	{
		if (!Lights[Index] || LightAges[Index] >= FightLightSeconds)
		{
			continue;
		}
		LightAges[Index] += GameDelta;
		if (LightAges[Index] >= FightLightSeconds)
		{
			Lights[Index]->SetVisibility(false);
		}
		else
		{
			Lights[Index]->SetIntensity(FightLightIntensity * FMath::Square(1.0f - LightAges[Index] / FightLightSeconds));
		}
	}
}

bool UFightSubsystem::FindDrawnMuzzle(colony::Entity Shooter, FVector& InOutMuzzle) const
{
	// Farther than this from the sim's muzzle point, the drawn gun is somewhere else (a ragdoll, a stale frame): keep the sim's.
	constexpr double MaxOffCm = 160.0;
	FVector Found = FVector::ZeroVector;
	bool bFound = false;
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const bool bMe = Protagonist && Protagonist->IsDirectMode() && Protagonist->GetControlled() == Shooter;
	if (bMe && Protagonist->GetBody() && Protagonist->GetBody()->IsFirstPerson())
	{
		if (const UGunRangeSubsystem* Range = GetWorld()->GetSubsystem<UGunRangeSubsystem>())
		{
			bFound = Range->GetViewMuzzle(Found);
		}
		if (!bFound)
		{
			return false;  // first person with no view gun drawn: the sim's point is at the eyes, which is right
		}
	}
	if (!bFound)
	{
		const UStaticMeshComponent* Gun = GetHeldGun(Shooter);
		if (!Gun || !Gun->IsVisible() || !Gun->GetStaticMesh())
		{
			return false;
		}
		// Guns are drawn with +X out of the barrel: the front face of the mesh's box, at its middle.
		const FBox Box = Gun->GetStaticMesh()->GetBoundingBox();
		const FVector Local(Box.Max.X, (Box.Min.Y + Box.Max.Y) * 0.5, (Box.Min.Z + Box.Max.Z) * 0.5);
		Found = Gun->GetComponentTransform().TransformPosition(Local);
		bFound = true;
	}
	if (!bFound || FVector::Dist(Found, InOutMuzzle) > MaxOffCm)
	{
		return false;
	}
	InOutMuzzle = Found;
	return true;
}

void UFightSubsystem::DrawShotStore(const colony::ThingView& View, float Scale) const
{
	if (View.shotStoreSize <= 0)
	{
		return;
	}
	ImDrawList* Draw = ImGui::GetWindowDrawList();
	const ImVec2 At = ImGui::GetCursorScreenPos();
	const float Width = 240.0f * Scale;
	const float BarHeight = 4.0f * Scale;
	const float PipHeight = 3.0f * Scale;
	const float Gap = 2.0f * Scale;
	const bool bPips = View.shotStoreSize > 1;
	const ImU32 Back = IM_COL32(25, 22, 10, 200);
	const ImU32 Yellow = IM_COL32(255, 214, 40, 245);
	const ImU32 Dim = IM_COL32(120, 100, 20, 230);
	// The bar: full while the gun is full; after the first shot it empties and fills again over the hidden cooldown,
	// like a stamina bar.
	const float Filled = View.refillSteps > 0 ? FMath::Clamp(1.0f - static_cast<float>(View.refillLeft) / static_cast<float>(View.refillSteps), 0.0f, 1.0f) : 1.0f;
	Draw->AddRectFilled(At, ImVec2(At.x + Width, At.y + BarHeight), Back, BarHeight * 0.5f);
	if (Filled > 0.0f)
	{
		Draw->AddRectFilled(At, ImVec2(At.x + Width * Filled, At.y + BarHeight), View.refillSteps > 0 ? Dim : Yellow, BarHeight * 0.5f);
	}
	// The shots left in the store, one pip each, under it (a one-shot gun has the bar alone).
	if (bPips)
	{
		const int32 Count = View.shotStoreSize;
		const float PipGap = Count > 24 ? 1.0f * Scale : 2.0f * Scale;
		const float PipWidth = FMath::Max(1.0f, (Width - PipGap * static_cast<float>(Count - 1)) / static_cast<float>(Count));
		const float Top = At.y + BarHeight + Gap;
		for (int32 I = 0; I < Count; ++I)
		{
			const float X = At.x + static_cast<float>(I) * (PipWidth + PipGap);
			Draw->AddRectFilled(ImVec2(X, Top), ImVec2(X + PipWidth, Top + PipHeight), I < View.shotStore ? Yellow : Back);
		}
	}
	ImGui::Dummy(ImVec2(Width, BarHeight + (bPips ? Gap + PipHeight : 0.0f) + Gap));
}

UStaticMeshComponent* UFightSubsystem::GetHeldGun(colony::Entity Pawn) const
{
	const UWeaponDrawSubsystem* Weapons = GetWorld()->GetSubsystem<UWeaponDrawSubsystem>();
	return Weapons ? Weapons->GetHandsComponent(Pawn) : nullptr;
}

int32 UFightSubsystem::GetHeldGuns() const
{
	const UWeaponDrawSubsystem* Weapons = GetWorld()->GetSubsystem<UWeaponDrawSubsystem>();
	return Weapons ? Weapons->CountWeaponsInHands() : 0;
}

void UFightSubsystem::SpawnMeleeImpact(colony::ProjectileHitKind Kind, colony::Entity Attacker, colony::Entity Target, const FVector& At,
	const FVector& Direction, int32 Part, float Amount)
{
	if (!EffectsOn())
	{
		return;
	}
	EnsureEffects();
	FFightTracer Blow;
	Blow.Id = colony::kNoEntity;
	Blow.Shooter = Attacker;
	Blow.Start = At - Direction.GetSafeNormal() * 60.0f;
	Blow.Dir = Direction;
	Blow.bDirKnown = true;
	Blow.bEnded = true;
	Blow.End = At;
	Blow.HitKind = Kind;
	Blow.Target = Target;
	Blow.Part = Part;
	Blow.Amount = Amount;
	SpawnImpact(Blow);
}
// Downed and dead pawns fall; pawns that get up are put back on their feet; armed pawns show their gun.
void UFightSubsystem::SyncPawns()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	if (!Bridge || !Pawns)
	{
		return;
	}
	const int64 Now = Bridge->GetSteps();
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		if (View.category != colony::ThingCategory::Pawn)
		{
			continue;
		}
		ASimPawnActor* Actor = Pawns->FindPawn(View.id);
		if (!Actor)
		{
			continue;
		}
		// A pawn downed by its torso and awake crawls instead of going limp (body-health.md parts D and E).
		const bool bDown = (View.downed && !View.crawling) || View.dead;
		if (bDown && !Actor->IsRagdoll())
		{
			FVector Impulse = FVector::ZeroVector;
			if (const TPair<FVector, int64>* Recent = RecentImpulse.Find(entt::to_integral(View.id)); Recent && Now - Recent->Value <= 3)
			{
				Impulse = Recent->Key;
			}
			Actor->StartRagdoll(Impulse);
			if (Actor->IsRagdoll())
			{
				++RagdollsStarted;
			}
		}
		else if (!bDown && Actor->IsRagdoll())
		{
			Actor->StopRagdoll();
		}
	}
	// Severed pieces (body-health.md part F): one thrown from its body when the sim first shows it, gone when the sim
	// has let it rot away. A piece whose body has no actor yet waits for it.
	TSet<uint32> Lying;
	for (const colony::SeveredPieceView& Piece : Bridge->GetCurrentSnapshot().severedPieces)
	{
		Lying.Add(Piece.id);
		if (SeveredPieces.Contains(Piece.id))
		{
			continue;
		}
		if (ASimPawnActor* From = Pawns->FindPawn(Piece.from))
		{
			ASeveredPieceActor* Thrown = From->SpawnSeveredPiece(Piece.bit);
			SeveredPieces.Add(Piece.id, Thrown);
			SeveredPiecesThrown += Thrown ? 1 : 0;
		}
	}
	for (auto It = SeveredPieces.CreateIterator(); It; ++It)
	{
		if (!Lying.Contains(It.Key()))
		{
			if (ASeveredPieceActor* Gone = It.Value().Get())
			{
				Gone->Destroy();
			}
			It.RemoveCurrent();
		}
	}
}

int32 UFightSubsystem::GetSeveredPiecesShown() const
{
	int32 Shown = 0;
	for (const TPair<uint32, TWeakObjectPtr<ASeveredPieceActor>>& Piece : SeveredPieces)
	{
		Shown += Piece.Value.IsValid() ? 1 : 0;
	}
	return Shown;
}

// ---------------------------------------------------------------------------
// The protagonist's gun.

bool UFightSubsystem::ComputeAimPoint(FVector& OutWorldPoint, colony::Entity* OutPawnHit, colony::DrawnAim* OutDrawn) const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (OutPawnHit)
	{
		*OutPawnHit = colony::kNoEntity;
	}
	if (OutDrawn)
	{
		*OutDrawn = colony::DrawnAim{};
	}
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || !Protagonist->GetBody())
	{
		return false;
	}
	const AProtagonistPawn* Body = Protagonist->GetBody();
	const FVector Direction = Body->GetViewDirection().GetSafeNormal();
	// Third person: the ray starts at the colonist's side of the camera, so a wall behind the colonist doesn't count.
	// (The view's distance now: closer while aiming, none in first person.)
	const FVector Start = Body->GetViewLocation() + Direction * Body->GetEffectiveArmLength();
	const FVector A = (Start - Bridge->GetWorldOrigin()) / 100.0;
	const FVector B = A + Direction * FightAimRangeM;
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::Snapshot& Snap = Bridge->GetCurrentSnapshot();
	const colony::ThingView* View = Snap.find(Pawn);
	const colony::CellPos OwnCell{FMath::FloorToInt32(View ? View->x : 0.0f), FMath::FloorToInt32(View ? View->y : 0.0f), 0};
	const colony::CombatSystem& Combat = Bridge->GetSim()->combat();
	const float Ax = static_cast<float>(A.X), Ay = static_cast<float>(A.Y), Az = static_cast<float>(A.Z);
	const float Bx = static_cast<float>(B.X), By = static_cast<float>(B.Y), Bz = static_cast<float>(B.Z);
	// The world: ground, rock and buildings (bodies come next, as they are drawn).
	const colony::TraceHit World = Combat.trace(Ax, Ay, Az, Bx, By, Bz, 0, Pawn, OwnCell, false);
	double BestCm = (World.kind != colony::ProjectileHitKind::None ? static_cast<double>(World.t) : 1.0) * FightAimRangeM * 100.0;
	FVector BestPoint = World.kind != colony::ProjectileHitKind::None ? FightSimToWorldUnits(*Bridge, World.x, World.y, World.z)
	                                                                  : FightSimToWorldUnits(*Bridge, Bx, By, Bz);
	colony::Entity BestPawn = colony::kNoEntity;
	const char* BestPart = nullptr;
	// Bodies as drawn this frame: what the crosshair is on is what gets hit (docs/design/aiming.md).
	TSet<uint32> NoSkeleton;
	if (const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
	{
		for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Entry : Pawns->GetPawns())
		{
			const ASimPawnActor* Actor = Entry.Value.Get();
			if (!Actor || Actor->GetThing() == Pawn)
			{
				continue;
			}
			const colony::ThingView* Other = Snap.find(Actor->GetThing());
			if (!Other || Other->dead)
			{
				continue;  // dead bodies don't stop bullets in the sim
			}
			// Quick test: the body's middle against the ray, with room for a reach or a lying body.
			const FVector Middle = Actor->GetActorLocation() + FVector(0.0, 0.0, 90.0 * Actor->GetBodyScale());
			const double Along = FVector::DotProduct(Middle - Start, Direction);
			if (Along < 0.0 || Along > BestCm + 250.0 || FVector::DistSquared(Start + Direction * Along, Middle) > FMath::Square(250.0 * Actor->GetBodyScale()))
			{
				continue;
			}
			DrawnHit::FResult Hit;
			bool bSkeleton = false;
			if (Actor->TraceDrawnBody(Start, Direction, BestCm, Hit, bSkeleton) && Hit.DistanceCm < BestCm)
			{
				BestCm = Hit.DistanceCm;
				BestPoint = Hit.Point;
				BestPawn = Actor->GetThing();
				BestPart = Hit.Part;
			}
			if (!bSkeleton)
			{
				NoSkeleton.Add(Entry.Key);
			}
		}
	}
	// Animals and other bodies without humanlike bones: the sim's own hitboxes.
	const colony::TraceHit Sims = Combat.trace(Ax, Ay, Az, Bx, By, Bz, 0, Pawn, OwnCell);
	if (Sims.kind == colony::ProjectileHitKind::Pawn && NoSkeleton.Contains(entt::to_integral(Sims.thing)) &&
		static_cast<double>(Sims.t) * FightAimRangeM * 100.0 < BestCm)
	{
		BestPoint = FightSimToWorldUnits(*Bridge, Sims.x, Sims.y, Sims.z);
		BestPawn = Sims.thing;
		BestPart = nullptr;
	}
	OutWorldPoint = BestPoint;
	if (OutPawnHit)
	{
		*OutPawnHit = BestPawn;
	}
	const colony::ThingView* Target = BestPawn != colony::kNoEntity && BestPart ? Snap.find(BestPawn) : nullptr;
	const USimPawnSubsystem* PawnsNow = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	const ASimPawnActor* TargetActor = Target && PawnsNow ? PawnsNow->FindPawn(BestPawn) : nullptr;
	if (OutDrawn && Target && TargetActor)
	{
		const int Part = Combat.hitboxPart(BestPawn, BestPart);
		if (Part >= 0)
		{
			// The spot as offsets on the map from where the sim has the pawn now (not its drawn yaw: the drawn body turns on
			// its own, so a spot in its frame landed tens of cm off on the sim's facing). The sim puts it back on the pawn as
			// it stands when the shot goes.
			const FVector SimPoint = (BestPoint - Bridge->GetWorldOrigin()) / 100.0;
			const float Ground = Bridge->GetSim()->map().heightAt(Target->x, Target->y);
			OutDrawn->pawn = BestPawn;
			OutDrawn->part = Part;
			OutDrawn->dx = static_cast<float>(SimPoint.X) - Target->x;
			OutDrawn->dy = static_cast<float>(SimPoint.Y) - Target->y;
			OutDrawn->up = static_cast<float>(SimPoint.Z) - Ground;
		}
	}
	return true;
}

bool UFightSubsystem::Fire()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || Bridge->GetSpeed() == 0)
	{
		return false;
	}
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Pawn);
	if (!View || View->downed || View->dead)
	{
		return false;
	}
	if (!View->weapon.valid() || !Bridge->GetDefs() || !Bridge->GetDefs()->get(View->weapon).has("verbs"))
	{
		// No gun in hand: nothing to fire (a melee weapon or fists swing instead, UWeaponDrawSubsystem).
		bNoGunMessage = true;
		return false;
	}
	// One order per sim step; and only when the gun can take it (the sim refuses anything else).
	if (Bridge->GetSteps() <= LastFireStep || Bridge->GetSim()->combat().shotReadyIn(Pawn) > 0)
	{
		return false;
	}
	FVector Aim;
	colony::DrawnAim Drawn;
	if (!ComputeAimPoint(Aim, nullptr, &Drawn))
	{
		return false;
	}
	const FVector Sim = (Aim - Bridge->GetWorldOrigin()) / 100.0;
	colony::ShootCommand Shot{Pawn, colony::kNoEntity, static_cast<float>(Sim.X), static_cast<float>(Sim.Y), static_cast<float>(Sim.Z), true};
	Shot.aimPawn = Drawn.pawn;
	Shot.aimPart = Drawn.part;
	Shot.aimDx = Drawn.dx;
	Shot.aimDy = Drawn.dy;
	Shot.aimUp = Drawn.up;
	Bridge->Submit(Shot);
	LastFireStep = Bridge->GetSteps();
	++FireOrders;
	return true;
}

bool UFightSubsystem::Reload()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || Bridge->GetSpeed() == 0)
	{
		return false;
	}
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::CombatSystem& Combat = Bridge->GetSim()->combat();
	if (Combat.magazineSize(Pawn) <= 0 || Combat.roundsLeft(Pawn) >= Combat.magazineSize(Pawn) || Combat.reloading(Pawn))
	{
		return false;
	}
	Bridge->Submit(colony::ReloadCommand{Pawn});
	return true;
}

bool UFightSubsystem::Patch()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || Bridge->GetSpeed() == 0)
	{
		return false;
	}
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Pawn);
	if (!View || View->downed || View->dead || View->patchZone >= 0 || Bridge->GetSim()->health().worstBleedingZone(Pawn) < 0)
	{
		return false;
	}
	Bridge->Submit(colony::PatchCommand{Pawn});
	++PatchOrders;
	return true;
}

bool UFightSubsystem::ToggleCrouch()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const colony::ThingView* View = Bridge && Protagonist && Protagonist->IsDirectMode() ? Bridge->GetCurrentSnapshot().find(Protagonist->GetControlled()) : nullptr;
	if (!View)
	{
		return false;
	}
	Bridge->Submit(colony::SetStanceCommand{View->id, View->stance == colony::Stance::Crouching ? colony::Stance::Standing : colony::Stance::Crouching});
	return true;
}

bool UFightSubsystem::ToggleProne()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const colony::ThingView* View = Bridge && Protagonist && Protagonist->IsDirectMode() ? Bridge->GetCurrentSnapshot().find(Protagonist->GetControlled()) : nullptr;
	if (!View)
	{
		return false;
	}
	Bridge->Submit(colony::SetStanceCommand{View->id, View->stance == colony::Stance::Prone ? colony::Stance::Standing : colony::Stance::Prone});
	return true;
}

bool UFightSubsystem::EquipNearest()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Bridge->GetDefs() || !Protagonist || !Protagonist->IsDirectMode())
	{
		return false;
	}
	const colony::Snapshot& Snapshot = Bridge->GetCurrentSnapshot();
	const colony::ThingView* Me = Snapshot.find(Protagonist->GetControlled());
	if (!Me || Me->downed || Me->dead)
	{
		return false;
	}
	colony::Entity Best = colony::kNoEntity;
	double BestDistance = FightEquipRangeM;
	for (const colony::ThingView& View : Snapshot.things)
	{
		if (View.category != colony::ThingCategory::Item || !View.def.valid())
		{
			continue;
		}
		const colony::Def& Def = Bridge->GetDefs()->get(View.def);
		if (Def.text("equipmentType") != "Primary" || !Def.has("verbs"))
		{
			continue;
		}
		const double Distance = FVector2D::Distance(FVector2D(Me->x, Me->y), FVector2D(View.x, View.y));
		if (Distance < BestDistance)
		{
			Best = View.id;
			BestDistance = Distance;
		}
	}
	if (Best == colony::kNoEntity)
	{
		EquipMessageSeconds = 2.0f;
		return false;
	}
	Bridge->Submit(colony::EquipCommand{Me->id, Best});
	return true;
}

void UFightSubsystem::UpdatePlayerInput()
{
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (!Protagonist || !Protagonist->IsDirectMode() || !PC)
	{
		return;
	}
	// With a building in hand the mouse button plans it and R turns it (UHandsOnSubsystem), so the gun stays quiet.
	if (Protagonist->IsUnstucking())
	{
		return;  // F12 Unstuck: no actions while waiting (docs/design/earthworks.md)
	}
	const UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>();
	const bool bBuilding = Orders && Orders->IsDirectBuilding();
	using Rim3DKeys::IsDown;
	using Rim3DKeys::WasPressed;
	if (!bBuilding && IsDown(PC, ERim3DAction::Attack))
	{
		Fire();
	}
	if (!bBuilding && WasPressed(PC, ERim3DAction::Reload))
	{
		Reload();
	}
	if (WasPressed(PC, ERim3DAction::Patch))
	{
		Patch();
	}
	if (WasPressed(PC, ERim3DAction::Crouch))
	{
		ToggleCrouch();
	}
	if (WasPressed(PC, ERim3DAction::Prone))
	{
		ToggleProne();
	}
	if (WasPressed(PC, ERim3DAction::EquipWeapon))
	{
		EquipNearest();
	}
	// T (sneed 2026-11-17): a dodge roll the way the movement keys point in third person, a dash in first person (the same
	// move in the sim; the first person camera stays level and blurs, UShooterFeelSubsystem). Left Alt no longer rolls.
	if (WasPressed(PC, ERim3DAction::RollDash))
	{
		Roll(FVector2D((IsDown(PC, ERim3DAction::MoveRight) ? 1.0 : 0.0) - (IsDown(PC, ERim3DAction::MoveLeft) ? 1.0 : 0.0),
			(IsDown(PC, ERim3DAction::MoveForward) ? 1.0 : 0.0) - (IsDown(PC, ERim3DAction::MoveBack) ? 1.0 : 0.0)));
	}
	// Grenades (sneed 2026-11-19): R picks the kind. G in third person while locked on throws at the target at once;
	// otherwise G held shows the arc and letting go throws where the crosshair is.
	if (!bBuilding && WasPressed(PC, ERim3DAction::NextGrenade))
	{
		NextGrenade();
	}
	if (bBuilding)
	{
		bAimingGrenade = false;
	}
	else if (WasPressed(PC, ERim3DAction::Grenade))
	{
		const UProtagonistSubsystem* GrenadeProt = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
		const AProtagonistPawn* Body = GrenadeProt ? GrenadeProt->GetBody() : nullptr;
		const bool bThirdPerson = Body && !Body->IsFirstPerson();
		if (bThirdPerson && GrenadeProt->GetFocusTarget() != colony::kNoEntity)
		{
			ThrowGrenade();
		}
		else
		{
			bAimingGrenade = true;
		}
	}
	else if (bAimingGrenade && !IsDown(PC, ERim3DAction::Grenade))
	{
		bAimingGrenade = false;
		ThrowGrenade();
	}
}

bool UFightSubsystem::Roll(const FVector2D& Input)
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || !Protagonist->GetBody() || Bridge->GetSpeed() == 0)
	{
		return false;
	}
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Pawn);
	if (!View || View->downed || View->dead || View->rollPhase != 0 || Bridge->GetSteps() <= LastFireStep)
	{
		return false;
	}
	// The keys are relative to the view; the sim wants a direction on the map (x east, y south as the map has it).
	const double Yaw = FMath::DegreesToRadians(static_cast<double>(Protagonist->GetBody()->GetYaw()));
	const FVector2D Ahead(FMath::Cos(Yaw), FMath::Sin(Yaw));
	const FVector2D Side(-Ahead.Y, Ahead.X);
	FVector2D Direction = Ahead * Input.Y + Side * Input.X;
	if (Direction.IsNearlyZero())
	{
		Direction = -Ahead;  // no key held: a backstep
	}
	Direction.Normalize();
	Bridge->Submit(colony::DodgeRollCommand{Pawn, static_cast<float>(Direction.X), static_cast<float>(Direction.Y)});
	LastFireStep = Bridge->GetSteps();
	return true;
}

colony::DefId UFightSubsystem::GetGrenadeChoice() const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist)
	{
		return colony::DefId{};
	}
	const colony::ExplosiveSystem& Explosives = Bridge->GetSim()->explosives();
	const colony::Entity Pawn = Protagonist->GetControlled();
	if (GrenadeChoice.valid() && Explosives.count(Pawn, GrenadeChoice) > 0)
	{
		return GrenadeChoice;
	}
	return Explosives.firstItem(Pawn);
}

bool UFightSubsystem::NextGrenade()
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode())
	{
		return false;
	}
	const std::vector<colony::BeltStack> Stacks = Bridge->GetSim()->explosives().stacks(Protagonist->GetControlled());
	if (Stacks.size() < 2)
	{
		return false;
	}
	const colony::DefId Now = GetGrenadeChoice();
	size_t Index = 0;
	for (size_t I = 0; I < Stacks.size(); ++I)
	{
		if (Stacks[I].item == Now)
		{
			Index = I;
			break;
		}
	}
	GrenadeChoice = Stacks[(Index + 1) % Stacks.size()].item;
	return true;
}

bool UFightSubsystem::GrenadeAimSim(USimSubsystem& Bridge, colony::Entity Pawn, colony::DefId Item, float& OutX, float& OutY) const
{
	const colony::ThingView* View = Bridge.GetCurrentSnapshot().find(Pawn);
	FVector Aim;
	if (!View || !ComputeAimPoint(Aim))
	{
		return false;
	}
	const FVector Sim = (Aim - Bridge.GetWorldOrigin()) / 100.0;
	OutX = static_cast<float>(Sim.X);
	OutY = static_cast<float>(Sim.Y);
	// Past the arm's reach the throw goes as far as the arm does (the sim does the same), so the arc shows that.
	const float Range = Bridge.GetSim()->explosives().throwRange(Item);
	const float Dx = OutX - View->x;
	const float Dy = OutY - View->y;
	const float Flat = FMath::Sqrt(Dx * Dx + Dy * Dy);
	if (Range > 0.0f && Flat > Range)
	{
		OutX = View->x + Dx / Flat * Range;
		OutY = View->y + Dy / Flat * Range;
	}
	return true;
}

bool UFightSubsystem::ThrowGrenade()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || Bridge->GetSpeed() == 0)
	{
		return false;
	}
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Pawn);
	if (!View || View->downed || View->dead || Bridge->GetSteps() <= LastFireStep || Bridge->GetSim()->explosives().throwReadyIn(Pawn) > 0)
	{
		return false;
	}
	const colony::DefId Item = GetGrenadeChoice();
	if (!Item.valid() || !Bridge->GetDefs())
	{
		return false;
	}
	const std::string ItemName = Bridge->GetDefs()->get(Item).defName();
	// Third person and locked on: at the target (the sim aims at the ground under it as it stands at the release).
	const AProtagonistPawn* Body = Protagonist->GetBody();
	const colony::Entity Focus = Protagonist->GetFocusTarget();
	if (Body && !Body->IsFirstPerson() && Focus != colony::kNoEntity && Bridge->GetCurrentSnapshot().find(Focus))
	{
		Bridge->Submit(colony::ThrowCommand{Pawn, ItemName, Focus, 0.0f, 0.0f, true});
		LastFireStep = Bridge->GetSteps();
		return true;
	}
	float X = 0.0f;
	float Y = 0.0f;
	if (!GrenadeAimSim(*Bridge, Pawn, Item, X, Y))
	{
		return false;
	}
	Bridge->Submit(colony::ThrowCommand{Pawn, ItemName, colony::kNoEntity, X, Y, true});
	LastFireStep = Bridge->GetSteps();
	return true;
}

void UFightSubsystem::DrawGrenadeArc(USimSubsystem& Bridge, colony::Entity Pawn)
{
	const colony::DefId Item = GetGrenadeChoice();
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	float X = 0.0f;
	float Y = 0.0f;
	colony::ThrowPlan Plan;
	if (!Item.valid() || !PC || !GrenadeAimSim(Bridge, Pawn, Item, X, Y) || !Bridge.GetSim()->explosives().planThrow(Pawn, Item, X, Y, Plan))
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	ImDrawList* Draw = ImGui::GetBackgroundDrawList();
	const ImU32 Line = Plan.reaches ? IM_COL32(255, 230, 120, 220) : IM_COL32(255, 120, 80, 220);
	const float Gravity = Bridge.GetSim()->combat().gravity();
	const auto Screen = [&](float SX, float SY, float SZ, ImVec2& Out)
	{
		FVector2D P;
		if (!PC->ProjectWorldLocationToScreen(Bridge.GetWorldOrigin() + FVector(SX, SY, SZ) * 100.0, P, true))
		{
			return false;
		}
		Out = ImVec2(Viewport->Pos.x + static_cast<float>(P.X), Viewport->Pos.y + static_cast<float>(P.Y));
		return true;
	};
	// The flight as the sim steps it, a dash every other step.
	float PX = Plan.startX;
	float PY = Plan.startY;
	float PZ = Plan.startZ;
	float VZ = Plan.vz;
	const int32 Steps = FMath::Clamp(Plan.flightSteps, 1, 600);
	ImVec2 Last;
	bool bLast = Screen(PX, PY, PZ, Last);
	for (int32 Step = 1; Step <= Steps; ++Step)
	{
		PX += Plan.vx;
		PY += Plan.vy;
		PZ += VZ - 0.5f * Gravity;
		VZ -= Gravity;
		if (Step == Steps)
		{
			PX = Plan.landX;
			PY = Plan.landY;
			PZ = Plan.landZ;
		}
		ImVec2 Now;
		const bool bNow = Screen(PX, PY, PZ, Now);
		if (bLast && bNow && (Step % 2) == 0)
		{
			Draw->AddLine(Last, Now, Line, 2.0f);
		}
		Last = Now;
		bLast = bNow;
	}
	// The blast's reach on the ground where it comes to rest.
	const float Radius = Bridge.GetSim()->explosives().blastRadius(Item);
	if (Radius > 0.0f)
	{
		const colony::Map& Map = Bridge.GetSim()->map();
		ImVec2 Ring[33];
		int32 Count = 0;
		for (int32 I = 0; I <= 32; ++I)
		{
			const float A = static_cast<float>(I) / 32.0f * 2.0f * PI;
			const float RX = Plan.landX + Radius * FMath::Cos(A);
			const float RY = Plan.landY + Radius * FMath::Sin(A);
			ImVec2 P;
			if (Screen(RX, RY, Map.heightAt(RX, RY) + 0.05f, P))
			{
				Ring[Count++] = P;
			}
		}
		if (Count > 2)
		{
			Draw->AddPolyline(Ring, Count, Line, 2.0f, 0);
		}
	}
	++GrenadeArcFrames;
}

// ---------------------------------------------------------------------------
// Debug view and HUD.

void UFightSubsystem::SetShowHitboxes(bool bShow)
{
	GFightHitboxes.AsVariable()->Set(bShow ? 1 : 0, ECVF_SetByCode);
}

bool UFightSubsystem::IsShowingHitboxes() const
{
	return GFightHitboxes.GetValueOnGameThread() != 0;
}

void UFightSubsystem::DrawHitboxes()
{
	HitboxesDrawn = 0;
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!IsShowingHitboxes() || !Bridge || !Bridge->GetSim())
	{
		return;
	}
	UWorld* World = GetWorld();
	const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(World, 0);
	const FVector CameraLocation = Camera ? Camera->GetCameraLocation() : FVector::ZeroVector;
	std::vector<colony::HitboxView> Boxes;
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		FVector Drawn;
		if (View.category != colony::ThingCategory::Pawn || !Bridge->GetBlendedLocation(View.id, Drawn) || FVector::Dist(Drawn, CameraLocation) > 7000.0)
		{
			continue;
		}
		if (!Bridge->GetSim()->combat().hitboxesOf(View.id, Boxes))
		{
			continue;
		}
		// The sim's hitboxes are where the pawn is after the newest step; the body is drawn between two steps.
		const FVector Offset = FVector(Drawn.X, Drawn.Y, 0.0) - FVector(Bridge->SimToWorld(View.x, View.y, View.level).X, Bridge->SimToWorld(View.x, View.y, View.level).Y, 0.0);
		for (const colony::HitboxView& H : Boxes)
		{
			const FColor Color = FLinearColor::MakeFromHSV8(static_cast<uint8>((H.part * 37) % 256), 230, 255).ToFColor(true);
			const FVector A = FightSimToWorldUnits(*Bridge, H.ax, H.ay, H.az) + Offset;
			if (H.box)
			{
				const FQuat Rotation(FVector::UpVector, FMath::Atan2(H.forwardY, H.forwardX));
				DrawDebugBox(World, A, FVector(H.bx, H.by, H.bz) * 100.0, Rotation, Color, false, -1.0f, SDPG_Foreground, 1.0f);
			}
			else
			{
				const FVector B = FightSimToWorldUnits(*Bridge, H.bx, H.by, H.bz) + Offset;
				const FVector Axis = B - A;
				const double Length = Axis.Size();
				const FQuat Rotation = Length > 0.01 ? FQuat::FindBetweenNormals(FVector::UpVector, Axis / Length) : FQuat::Identity;
				DrawDebugCapsule(World, (A + B) * 0.5, Length * 0.5 + H.radius * 100.0, H.radius * 100.0, Rotation, Color, false, -1.0f, SDPG_Foreground, 1.0f);
			}
			++HitboxesDrawn;
		}
	}
}

void UFightSubsystem::DrawHud()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode())
	{
		return;
	}
	const ImGui::FScopedContext ScopedContext;
	if (!ScopedContext)
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	if (Viewport->Size.x <= 0.0f || Viewport->Size.y <= 0.0f)
	{
		return;
	}
	const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
	ImGui::GetStyle().FontScaleMain = 1.2f * Scale;
	const float Margin = 12.0f * Scale;
	const colony::Sim& Sim = *Bridge->GetSim();
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Pawn);

	// F12 Unstuck: a countdown in the middle of the screen (docs/design/earthworks.md).
	if (Protagonist->IsUnstucking())
	{
		ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.62f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("##Unstuck", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
		{
			const float Left = Protagonist->GetUnstuckSecondsLeft();
			ImGui::TextUnformatted(TCHAR_TO_UTF8(*FString::Printf(TEXT("Getting unstuck in %d..."), FMath::CeilToInt32(Left))));
			ImGui::ProgressBar(1.0f - Left / 5.0f, ImVec2(220.0f * Scale, 6.0f * Scale), "");
		}
		ImGui::End();
	}

	// The gun, bottom middle.
	const ImGuiWindowFlags PanelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y - Margin), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
	ImGui::SetNextWindowBgAlpha(0.6f);
	if (View && ImGui::Begin("##FightGun", nullptr, PanelFlags))
	{
		const bool bGun = View->weapon.valid() && Bridge->GetDefs() && Bridge->GetDefs()->get(View->weapon).has("verbs");
		const UWeaponDrawSubsystem* Weapons = GetWorld()->GetSubsystem<UWeaponDrawSubsystem>();
		if (bGun)
		{
			const FString Label = UTF8_TO_TCHAR(Bridge->GetDefs()->get(View->weapon).label().c_str());
			if (View->magazineSize > 0)
			{
				FightText(FString::Printf(TEXT("%s   %d / %d%s"), *Label, View->rounds, View->magazineSize, View->reloading ? TEXT("   reloading") : TEXT("")));
				ImGui::TextUnformatted("Left mouse fire   R reload   C crouch   Z lie down");
			}
			else
			{
				FightText(Label);
				DrawShotStore(*View, Scale);
				ImGui::TextUnformatted("Left mouse fire   C crouch   Z lie down");
			}
		}
		else if (View->weapon.valid() && Bridge->GetDefs())
		{
			FightText(UTF8_TO_TCHAR(Bridge->GetDefs()->get(View->weapon).label().c_str()));
			ImGui::TextUnformatted("Left mouse swing   Right mouse block   C crouch   Z lie down");
		}
		else
		{
			ImGui::TextUnformatted("Fists   Left mouse punch   E picks up a gun");
			if (EquipMessageSeconds > 0.0f)
			{
				ImGui::TextUnformatted("No gun within reach");
			}
		}
		// Grenades on the belt (sneed 2026-11-19): the kind G throws, how many, and the keys.
		if (View->grenades > 0 && Bridge->GetDefs())
		{
			const colony::DefId Item = GetGrenadeChoice();
			if (Item.valid())
			{
				const int32 Count = Sim.explosives().count(Pawn, Item);
				const bool bKinds = Sim.explosives().stacks(Pawn).size() > 1;
				const FString Line = FString::Printf(TEXT("%s x%d   %s throw (hold to aim)%s%s"), UTF8_TO_TCHAR(Bridge->GetDefs()->get(Item).label().c_str()), Count,
					*Rim3DKeys::GetKeyLabel(ERim3DAction::Grenade), bKinds ? TEXT("   ") : TEXT(""),
					bKinds ? *FString::Printf(TEXT("%s next kind"), *Rim3DKeys::GetKeyLabel(ERim3DAction::NextGrenade)) : TEXT(""));
				if (View->throwReadyIn > 0)
				{
					ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", TCHAR_TO_UTF8(*Line));
				}
				else
				{
					ImGui::TextUnformatted(TCHAR_TO_UTF8(*Line));
				}
			}
		}
		// Bleeding: the key that patches it (body-health.md part B), or how long the patching still takes.
		if (View->patchZone >= 0)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Patching...  %.1f s", static_cast<float>(View->patchStepsLeft) / static_cast<float>(colony::kStepsPerRealSecondAt1x));
		}
		else if (!View->downed && !View->dead && Sim.health().worstBleedingZone(Pawn) >= 0)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", TCHAR_TO_UTF8(*FString::Printf(TEXT("Bleeding   %s patch the wound"), *Rim3DKeys::GetKeyLabel(ERim3DAction::Patch))));
		}
		if (Weapons)
		{
			const FString Slots = Weapons->DescribeCarried(*View);
			if (!Slots.IsEmpty())
			{
				FightText(Slots);
			}
		}
	}
	ImGui::End();

	if (bAimingGrenade && View && !View->dead)
	{
		DrawGrenadeArc(*Bridge, Pawn);
	}

	// A flash of red lines round the crosshair when a shot of yours hits someone.
	if (HitMarkerSeconds > 0.0f)
	{
		const ImVec2 Centre(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.5f);
		ImDrawList* Draw = ImGui::GetForegroundDrawList();
		const ImU32 Red = IM_COL32(255, 60, 50, 235);
		const float Near = 12.0f * Scale;
		const float Far = 22.0f * Scale;
		for (const float SX : {-1.0f, 1.0f})
		{
			for (const float SY : {-1.0f, 1.0f})
			{
				Draw->AddLine(ImVec2(Centre.x + SX * Near, Centre.y + SY * Near), ImVec2(Centre.x + SX * Far, Centre.y + SY * Far), Red, 2.5f);
			}
		}
	}
	DrawDamageNumbers(Scale);
	DrawAlertMarks(Scale);
	(void)Sim;
	++HudFramesDrawn;
}

void UFightSubsystem::AddDamageNumber(const FFightTracer& Tracer, bool bArmour, bool bOnMe)
{
	if (GFightDamageNumbers.GetValueOnGameThread() == 0)
	{
		return;
	}
	// A little to the side at random so a burst's numbers don't sit on each other.
	const FVector Jitter(Random.FRandRange(-8.0f, 8.0f), Random.FRandRange(-8.0f, 8.0f), Random.FRandRange(0.0f, 10.0f));
	// A hit on you has no place in the world to float over that you could see (in first person it is behind the camera):
	// it shows under the crosshair, and `At` is its offset there in 1080p pixels.
	DamageNumbers.Add({bOnMe ? FVector(Random.FRandRange(-70.0f, 70.0f), Random.FRandRange(0.0f, 24.0f), 0.0f) : Tracer.End + Jitter, Tracer.Amount, 0.0f, bArmour, bOnMe});
	DamageNumbersOnMe += bOnMe ? 1 : 0;
	if (DamageNumbers.Num() > 24)
	{
		DamageNumbers.RemoveAt(0);
	}
	++DamageNumbersShown;
}

// Stealth marks (docs/design/stealth.md): a yellow "?" over an enemy that is suspicious, a red "!" for a moment when
// one is alerted. Nothing over one that is unaware, or over your own side.
void UFightSubsystem::DrawAlertMarks(float Scale)
{
	USimSubsystem* Bridge = BridgeSource.Get();
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (!Bridge || !PC)
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	ImDrawList* Draw = ImGui::GetForegroundDrawList();
	ImFont* Font = ImGui::GetFont();
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		if (View.category != colony::ThingCategory::Pawn || !View.fighting || View.faction == 1 || View.dead || View.downed)
		{
			continue;
		}
		const float* Flash = AlertFlash.Find(entt::to_integral(View.id));
		const bool bSuspicious = View.alert == 1;
		if (!Flash && !bSuspicious)
		{
			continue;
		}
		// Just above the head: 1.85 m over the ground it stands on (the mark is drawn upward from there).
		const FVector Head = FightSimToWorldUnits(*Bridge, View.x, View.y, View.z + 1.85f);
		FVector2D Screen;
		if (!PC->ProjectWorldLocationToScreen(Head, Screen, true))
		{
			continue;
		}
		const float T = Flash ? *Flash / FightAlertFlashSeconds : 0.0f;
		// The "!" pops in big and fades over its last third; the "?" stays as long as the enemy is unsure.
		const float Size = ImGui::GetFontSize() * (Flash ? (T < 0.1f ? 3.4f - T * 8.0f : 2.6f) : 2.2f);
		const float Alpha = Flash ? (T < 0.66f ? 1.0f : 1.0f - (T - 0.66f) / 0.34f) : 1.0f;
		const char* Text = Flash ? "!" : "?";
		const ImVec2 Extent = Font->CalcTextSizeA(Size, FLT_MAX, 0.0f, Text);
		const ImVec2 At(Viewport->Pos.x + static_cast<float>(Screen.X) - Extent.x * 0.5f, Viewport->Pos.y + static_cast<float>(Screen.Y) - Extent.y);
		const ImU32 Colour = Flash ? IM_COL32(255, 60, 50, static_cast<int>(255 * Alpha)) : IM_COL32(255, 215, 60, 255);
		const ImU32 Shadow = IM_COL32(0, 0, 0, static_cast<int>(220 * Alpha));
		Draw->AddText(Font, Size, ImVec2(At.x + 2.0f * Scale, At.y + 2.0f * Scale), Shadow, Text);
		Draw->AddText(Font, Size, At, Colour, Text);
		++AlertMarksDrawn;
	}
}

void UFightSubsystem::DrawDamageNumbers(float Scale)
{
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (!PC || DamageNumbers.IsEmpty())
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	ImDrawList* Draw = ImGui::GetForegroundDrawList();
	ImFont* Font = ImGui::GetFont();
	for (const FFightDamageNumber& Number : DamageNumbers)
	{
		FVector2D Screen;
		if (Number.bOnMe)
		{
			Screen = FVector2D(Viewport->Size.x * 0.5f + Number.At.X * Scale, Viewport->Size.y * 0.64f + Number.At.Y * Scale);
		}
		else if (!PC->ProjectWorldLocationToScreen(Number.At, Screen, true))
		{
			continue;
		}
		const float T = Number.Age / FightDamageNumberSeconds;
		// Pops a little bigger at first, rises about 50 px and fades over the last half.
		const float Size = ImGui::GetFontSize() * (T < 0.1f ? 1.9f - T * 4.0f : 1.5f) * (Number.Amount >= 15.0f ? 1.2f : 1.0f);
		const float Alpha = T < 0.5f ? 1.0f : 1.0f - (T - 0.5f) / 0.5f;
		const FString Text = Number.bArmour && Number.Amount <= 0.0f ? FString(TEXT("blocked")) : FString::Printf(TEXT("%.0f"), FMath::Max(Number.Amount, 0.0f));
		const FTCHARToUTF8 Utf8(*Text);
		const ImVec2 Extent = Font->CalcTextSizeA(Size, FLT_MAX, 0.0f, Utf8.Get());
		const ImVec2 At(Viewport->Pos.x + static_cast<float>(Screen.X) - Extent.x * 0.5f, Viewport->Pos.y + static_cast<float>(Screen.Y) - Extent.y - 50.0f * Scale * T);
		const ImU32 Colour = Number.bOnMe ? IM_COL32(255, 70, 60, static_cast<int>(255 * Alpha))
			: Number.bArmour ? IM_COL32(170, 200, 255, static_cast<int>(255 * Alpha))
			: (Number.Amount >= 15.0f ? IM_COL32(255, 210, 60, static_cast<int>(255 * Alpha)) : IM_COL32(255, 255, 255, static_cast<int>(255 * Alpha)));
		const ImU32 Shadow = IM_COL32(0, 0, 0, static_cast<int>(200 * Alpha));
		Draw->AddText(Font, Size, ImVec2(At.x + 2.0f, At.y + 2.0f), Shadow, Utf8.Get());
		Draw->AddText(Font, Size, At, Colour, Utf8.Get());
	}
}
