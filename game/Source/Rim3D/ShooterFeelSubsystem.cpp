#include "ShooterFeelSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"

#include <imgui.h>

#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "VisualMesh.h"

#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

DEFINE_LOG_CATEGORY_STATIC(LogShooterFeel, Log, All);

namespace
{
	TAutoConsoleVariable<float> GFeelShake(
		TEXT("Feel.Shake"), 1.0f, TEXT("How much the camera shakes from shots, hits and blasts: 0 none, 1 normal, 2 double."), ECVF_Default);
	TAutoConsoleVariable<float> GFeelBlur(
		TEXT("Feel.Blur"), 1.0f, TEXT("How much motion blur there is, with the extra pulses near blasts and in rolls: 0 none, 1 normal."), ECVF_Default);
	TAutoConsoleVariable<float> GFeelKick(
		TEXT("Feel.Kick"), 1.0f, TEXT("How far the gun's kick lifts the view: 0 not at all, 1 as far as the sim's kick, 2 double. The shots go where the crosshair was."), ECVF_Default);
	TAutoConsoleVariable<int32> GFeelAimView(
		TEXT("Feel.AimView"), 0, TEXT("Holding aim (right mouse button with a gun): 0 over the shoulder, 1 first person."), ECVF_Default);
	TAutoConsoleVariable<float> GFeelAimFov(
		TEXT("Feel.AimFov"), 62.0f, TEXT("The field of view of the aimed view (the normal one is 80)."), ECVF_Default);
	TAutoConsoleVariable<int32> GFeelDebris(
		TEXT("Feel.Debris"), 1, TEXT("1 throws debris from blasts (Chaos rigid bodies, for a few seconds), 0 none."), ECVF_Default);

	const TCHAR* const FeelSolidMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");
	const TCHAR* const FeelCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* const FeelConfigSection = TEXT("Rim3DFeel");

	// The settings file keeps the sliders.
	constexpr int32 FeelMaxParticles = 1200;
	constexpr int32 FeelMaxDebris = 140;
	constexpr float FeelDebrisSeconds = 5.0f;
	// How long a blast's invisible floor stays (a little after its last piece).
	constexpr float FeelFloorSeconds = FeelDebrisSeconds + 0.5f;
	constexpr float FeelFloorSize = 30.0f;
	constexpr float FeelFloorThickness = 0.5f;
	constexpr int32 FeelFlashLights = 3;

	// A shot's shake in degrees, and more for a heavier kick.
	constexpr float FeelShotShake = 0.22f;
	constexpr float FeelShotShakePerKick = 0.07f;
	constexpr float FeelHitShake = 1.1f;
	constexpr float FeelKnockShake = 3.5f;
	// A blast of radius 1 m shakes this much at its edge, falling off to nothing at this many radii plus 30 m.
	constexpr float FeelBlastShake = 5.5f;
	constexpr float FeelBlastReachMetres = 30.0f;

	float FeelSmooth(float Value)
	{
		const float T = FMath::Clamp(Value, 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	FVector FeelSimToWorld(const USimSubsystem& Source, float X, float Y, float Z)
	{
		return Source.GetWorldOrigin() + FVector(X * 100.0, Y * 100.0, Z * 100.0);
	}
}

float UShooterFeelSubsystem::GetShakeScale() { return FMath::Clamp(GFeelShake.GetValueOnGameThread(), 0.0f, 3.0f); }
float UShooterFeelSubsystem::GetBlurScale() { return FMath::Clamp(GFeelBlur.GetValueOnGameThread(), 0.0f, 2.0f); }
float UShooterFeelSubsystem::GetKickScale() { return FMath::Clamp(GFeelKick.GetValueOnGameThread(), 0.0f, 3.0f); }
bool UShooterFeelSubsystem::IsAimFirstPerson() { return GFeelAimView.GetValueOnGameThread() != 0; }
float UShooterFeelSubsystem::GetAimFov() { return FMath::Clamp(GFeelAimFov.GetValueOnGameThread(), 30.0f, 90.0f); }

bool UShooterFeelSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UShooterFeelSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UShooterFeelSubsystem, STATGROUP_Tickables);
}

void UShooterFeelSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Source = Collection.InitializeDependency<USimSubsystem>())
	{
		Bridge = Source;
		EventHandle = Source->OnSimEvent.AddUObject(this, &UShooterFeelSubsystem::OnSimEvent);
	}
	Collection.InitializeDependency<UProtagonistSubsystem>();
	// The sliders as they were left.
	struct FSetting { const TCHAR* Key; TAutoConsoleVariable<float>* Var; };
	for (const FSetting& Setting : {FSetting{TEXT("Shake"), &GFeelShake}, FSetting{TEXT("Blur"), &GFeelBlur}, FSetting{TEXT("Kick"), &GFeelKick}, FSetting{TEXT("AimFov"), &GFeelAimFov}})
	{
		float Value = 0.0f;
		if (GConfig && GConfig->GetFloat(FeelConfigSection, Setting.Key, Value, GGameUserSettingsIni))
		{
			Setting.Var->AsVariable()->Set(Value, ECVF_SetByCode);
		}
	}
	int32 AimView = 0;
	if (GConfig && GConfig->GetInt(FeelConfigSection, TEXT("AimView"), AimView, GGameUserSettingsIni))
	{
		GFeelAimView.AsVariable()->Set(AimView, ECVF_SetByCode);
	}
}

void UShooterFeelSubsystem::Deinitialize()
{
	if (USimSubsystem* Source = Bridge.Get())
	{
		Source->OnSimEvent.Remove(EventHandle);
	}
	Teardown();
	Super::Deinitialize();
}

void UShooterFeelSubsystem::SaveSettings()
{
	if (!GConfig)
	{
		return;
	}
	GConfig->SetFloat(FeelConfigSection, TEXT("Shake"), GFeelShake.GetValueOnGameThread(), GGameUserSettingsIni);
	GConfig->SetFloat(FeelConfigSection, TEXT("Blur"), GFeelBlur.GetValueOnGameThread(), GGameUserSettingsIni);
	GConfig->SetFloat(FeelConfigSection, TEXT("Kick"), GFeelKick.GetValueOnGameThread(), GGameUserSettingsIni);
	GConfig->SetFloat(FeelConfigSection, TEXT("AimFov"), GFeelAimFov.GetValueOnGameThread(), GGameUserSettingsIni);
	GConfig->SetInt(FeelConfigSection, TEXT("AimView"), GFeelAimView.GetValueOnGameThread(), GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

void UShooterFeelSubsystem::Teardown()
{
	for (FFeelDebris& Piece : Debris)
	{
		if (Piece.Piece.IsValid())
		{
			Piece.Piece->DestroyComponent();
		}
	}
	Debris.Reset();
	for (UStaticMeshComponent* Floor : Floors)
	{
		if (Floor)
		{
			Floor->DestroyComponent();
		}
	}
	Floors.Reset();
	FloorAges.Reset();
	if (Holder)
	{
		Holder->Destroy();
		Holder = nullptr;
	}
	ParticleInstances.Reset();
	FlashLights.Reset();
	KeepMeshes.Reset();
	Particles.Reset();
}

colony::Entity UShooterFeelSubsystem::ControlledPawn() const
{
	const UProtagonistSubsystem* Protagonist = GetWorld() ? GetWorld()->GetSubsystem<UProtagonistSubsystem>() : nullptr;
	return Protagonist && Protagonist->IsDirectMode() ? Protagonist->GetControlled() : colony::kNoEntity;
}

// ---------------------------------------------------------------------------
// The scene (made when it is first needed).

UStaticMesh* UShooterFeelSubsystem::MakeSolid(int32 Shape, float X, float Y, float Z, const FLinearColor& Color)
{
	VisualMesh::FPart Part;
	Part.Shape = static_cast<VisualMesh::EShape>(Shape);
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
}

UInstancedStaticMeshComponent* UShooterFeelSubsystem::MakeInstanced(UStaticMesh* Mesh)
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

void UShooterFeelSubsystem::EnsureScene()
{
	if (Holder || !GetWorld())
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("FeelRoot"));
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	Holder->SetActorLabel(TEXT("ShooterFeel"));
#endif
	SolidMaterial = LoadObject<UMaterialInterface>(nullptr, FeelSolidMaterialPath);
	CubeMesh = LoadObject<UStaticMesh>(nullptr, FeelCubePath);
	if (!SolidMaterial)
	{
		UE_LOG(LogShooterFeel, Error, TEXT("Could not load %s; blasts cannot be drawn"), FeelSolidMaterialPath);
		return;
	}
	const int32 Sphere = static_cast<int32>(VisualMesh::EShape::Sphere);
	const int32 Box = static_cast<int32>(VisualMesh::EShape::Box);
	ParticleInstances.Reset();
	ParticleInstances.Add(MakeInstanced(MakeSolid(Sphere, 10.0f, 10.0f, 20.0f, FLinearColor(1.0f, 0.7f, 0.2f))));    // FireCore
	ParticleInstances.Add(MakeInstanced(MakeSolid(Sphere, 10.0f, 10.0f, 20.0f, FLinearColor(0.9f, 0.25f, 0.03f))));  // FireOuter
	ParticleInstances.Add(MakeInstanced(MakeSolid(Sphere, 10.0f, 10.0f, 20.0f, FLinearColor(0.012f, 0.012f, 0.013f))));  // Smoke
	ParticleInstances.Add(MakeInstanced(MakeSolid(Box, 2.0f, 2.0f, 2.0f, FLinearColor(1.0f, 0.62f, 0.15f))));        // Spark
	ParticleInstances.Add(MakeInstanced(MakeSolid(Sphere, 10.0f, 10.0f, 20.0f, FLinearColor(0.16f, 0.12f, 0.08f))));  // Dust
	ParticleInstances.Add(MakeInstanced(MakeSolid(Sphere, 4.0f, 4.0f, 8.0f, FLinearColor(0.07f, 0.09f, 0.04f))));     // Grenade
	DebrisMesh = MakeSolid(Box, 10.0f, 10.0f, 10.0f, FLinearColor(0.12f, 0.1f, 0.08f));
	// A mesh made at run time has no collision: give the debris a box to be a rigid body with (the mesh is a 10 cm cube on z = 0).
	if (DebrisMesh)
	{
		if (!DebrisMesh->GetBodySetup())
		{
			DebrisMesh->CreateBodySetup();
		}
		if (UBodySetup* Setup = DebrisMesh->GetBodySetup())
		{
			Setup->RemoveSimpleCollision();
			FKBoxElem Cube(10.0f, 10.0f, 10.0f);
			Cube.Center = FVector(0.0, 0.0, 5.0);
			Setup->AggGeom.BoxElems.Add(Cube);
			Setup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
			Setup->InvalidatePhysicsData();
			Setup->CreatePhysicsMeshes();
		}
	}

	FlashLights.Reset();
	for (int32 I = 0; I < FeelFlashLights; ++I)
	{
		UPointLightComponent* Light = NewObject<UPointLightComponent>(Holder);
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetIntensity(0.0f);
		Light->SetLightColor(FLinearColor(1.0f, 0.75f, 0.42f));
		Light->SetAttenuationRadius(2500.0f);
		Light->SetSourceRadius(40.0f);
		Light->SetCastShadows(false);
		Light->SetupAttachment(Root);
		Light->RegisterComponent();
		Light->SetVisibility(false);
		FlashLights.Add(Light);
	}
}

// ---------------------------------------------------------------------------
// The sim's events.

void UShooterFeelSubsystem::AddShake(float Degrees, float Seconds, float Frequency)
{
	if (Degrees <= 0.0f || Seconds <= 0.0f)
	{
		return;
	}
	FShake Shake;
	Shake.Degrees = Degrees;
	Shake.Duration = Seconds;
	Shake.Frequency = Frequency;
	Shake.Seed = Random.FRand() * 100.0f;
	if (Shakes.Num() >= 24)
	{
		Shakes.RemoveAt(0);
	}
	Shakes.Add(Shake);
}

void UShooterFeelSubsystem::OnSimEvent(const colony::Event& Event)
{
	const colony::Entity Me = ControlledPawn();
	switch (Event.type)
	{
		case colony::EventType::ShotFired:
		{
			if (Me != colony::kNoEntity && Event.thing == Me)
			{
				++ShotsFelt;
				float FirstKick = 1.0f;
				if (const USimSubsystem* Source = Bridge.Get(); Source && Source->GetDefs() && Event.def.valid())
				{
					FirstKick = static_cast<float>(Source->GetDefs()->get(Event.def).number("recoilKicks.0.up", 1.0));
				}
				AddShake(FeelShotShake + FeelShotShakePerKick * FirstKick, 0.13f, 31.0f);
			}
			break;
		}
		case colony::EventType::PawnInjured:
		case colony::EventType::PawnPartLost:
			if (Me != colony::kNoEntity && Event.thing == Me)
			{
				++HitsFelt;
				AddShake(FeelHitShake, 0.3f, 24.0f);
				VignettePulse = FMath::Max(VignettePulse, 0.45f);
			}
			break;
		case colony::EventType::PawnKnockedDown:
			if (Me != colony::kNoEntity && Event.thing == Me)
			{
				AddShake(FeelKnockShake, 0.9f, 15.0f);
				VignettePulse = FMath::Max(VignettePulse, 0.7f);
			}
			break;
		case colony::EventType::Explosion:
			OnExplosion(Event);
			break;
		default:
			break;
	}
}

void UShooterFeelSubsystem::OnExplosion(const colony::Event& Event)
{
	USimSubsystem* Source = Bridge.Get();
	if (!Source)
	{
		return;
	}
	++ExplosionsSeen;
	EnsureScene();
	const float RadiusM = FMath::Max(Event.amount, 0.5f);
	const FVector At = FeelSimToWorld(*Source, Event.x, Event.y, Event.z);

	// The camera feels it by distance: the nearer the harder, none past the blast's reach.
	APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	const double Distance = CameraManager ? FVector::Dist(CameraManager->GetCameraLocation(), At) * 0.01 : 100.0;
	const double Reach = RadiusM * 6.0 + FeelBlastReachMetres;
	if (Distance < Reach)
	{
		const float Closeness = static_cast<float>(1.0 - Distance / Reach);
		const float Strength = FMath::Clamp(0.45f + 0.55f * FMath::Sqrt(RadiusM), 0.5f, 2.5f);
		AddShake(FeelBlastShake * Strength * Closeness * Closeness, 0.5f + 0.6f * Closeness, 17.0f);
		BlurPulse = FMath::Max(BlurPulse, 0.9f * Closeness);
		VignettePulse = FMath::Max(VignettePulse, 0.6f * Closeness);
	}

	// Fireball, smoke, sparks and dust.
	const float Scale = RadiusM / 1.9f;
	Burst(EBlastParticle::FireCore, At + FVector(0.0, 0.0, 30.0), FMath::RoundToInt(5 + 3 * Scale), 140.0f * Scale, 0.8f, 55.0f * Scale, 0.35f + 0.1f * Scale, 0.0f);
	Burst(EBlastParticle::FireOuter, At + FVector(0.0, 0.0, 30.0), FMath::RoundToInt(8 + 5 * Scale), 230.0f * Scale, 0.6f, 75.0f * Scale, 0.55f + 0.15f * Scale, 0.0f);
	Burst(EBlastParticle::Smoke, At + FVector(0.0, 0.0, 40.0), FMath::RoundToInt(12 + 6 * Scale), 150.0f * Scale, 1.0f, 55.0f * Scale, 2.6f + 0.6f * Scale, -40.0f);
	Burst(EBlastParticle::Spark, At + FVector(0.0, 0.0, 20.0), FMath::RoundToInt(24 + 12 * Scale), 700.0f * Scale, 0.5f, 1.8f, 0.9f, 980.0f);
	Burst(EBlastParticle::Dust, At + FVector(0.0, 0.0, 8.0), FMath::RoundToInt(10 + 6 * Scale), 260.0f * Scale, 0.15f, 45.0f * Scale, 1.6f, -10.0f);

	// A flash of light that dies in half a second.
	FlashAt = At + FVector(0.0, 0.0, 120.0);
	FlashPeak = 9000.0f * FMath::Clamp(Scale, 0.6f, 3.0f);
	FlashAge = 0.0f;
	for (UPointLightComponent* Light : FlashLights)
	{
		if (Light)
		{
			Light->SetWorldLocation(FlashAt);
		}
	}

	if (GFeelDebris.GetValueOnGameThread() != 0 && CubeMesh && DebrisMesh)
	{
		// The ground under the blast leans with the map there.
		const colony::Map& Map = Source->GetSim()->map();
		const float MX = Event.x;
		const float MY = Event.y;
		const FVector Normal = FVector((Map.heightAt(MX - 1.0f, MY) - Map.heightAt(MX + 1.0f, MY)) * 0.5f, (Map.heightAt(MX, MY - 1.0f) - Map.heightAt(MX, MY + 1.0f)) * 0.5f, 1.0f).GetSafeNormal();
		const FVector Ground = Source->SimToWorld(MX, MY, 0);
		SpawnDebris(Ground, RadiusM, Normal);
	}
}

void UShooterFeelSubsystem::Burst(EBlastParticle Kind, const FVector& At, int32 Count, float Speed, float UpBias, float SizeCm, float Life, float Gravity)
{
	for (int32 I = 0; I < Count && Particles.Num() < FeelMaxParticles; ++I)
	{
		FParticle P;
		P.Kind = Kind;
		FVector Direction = Random.VRand();
		Direction.Z = FMath::Abs(Direction.Z) * UpBias + (UpBias > 0.7f ? 0.2f : 0.0f);
		Direction.Normalize();
		P.Position = At + Direction * Random.FRandRange(0.0f, 12.0f);
		P.Velocity = Direction * Speed * Random.FRandRange(0.5f, 1.0f);
		P.Age = 0.0f;
		P.Life = Life * Random.FRandRange(0.8f, 1.2f);
		P.SizeCm = SizeCm * Random.FRandRange(0.7f, 1.2f);
		P.Gravity = Gravity;
		P.Seed = Random.FRand();
		Particles.Add(P);
	}
}

void UShooterFeelSubsystem::SpawnDebris(const FVector& Centre, float RadiusM, const FVector& GroundNormal)
{
	// An invisible floor under the blast: the ground is drawn from the sim and has no collision of its own.
	UStaticMeshComponent* Floor = NewObject<UStaticMeshComponent>(Holder);
	Floor->SetStaticMesh(CubeMesh);
	Floor->SetMobility(EComponentMobility::Movable);
	Floor->SetCollisionProfileName(TEXT("BlockAll"));
	Floor->SetCollisionObjectType(ECC_WorldStatic);
	Floor->SetCanEverAffectNavigation(false);
	Floor->SetCastShadow(false);
	Floor->SetVisibility(false);
	Floor->SetHiddenInGame(true);
	Floor->SetWorldScale3D(FVector(FeelFloorSize, FeelFloorSize, FeelFloorThickness));
	Floor->RegisterComponent();
	Floor->SetWorldLocationAndRotation(Centre - GroundNormal * (FeelFloorThickness * 50.0f), FRotationMatrix::MakeFromZ(GroundNormal).ToQuat());
	Floors.Add(Floor);
	FloorAges.Add(0.0f);

	const int32 Want = FMath::Clamp(FMath::RoundToInt(10.0f + 8.0f * RadiusM), 8, 60);
	for (int32 I = 0; I < Want && Debris.Num() < FeelMaxDebris; ++I)
	{
		UStaticMeshComponent* Piece = NewObject<UStaticMeshComponent>(Holder);
		Piece->SetStaticMesh(DebrisMesh);
		Piece->SetMobility(EComponentMobility::Movable);
		Piece->SetCollisionProfileName(TEXT("PhysicsActor"));
		// Pieces fall on the floor and nothing else: not on each other, not on pawns.
		Piece->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Ignore);
		Piece->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		Piece->SetCollisionResponseToChannel(ECC_Vehicle, ECR_Ignore);
		Piece->SetCanEverAffectNavigation(false);
		Piece->SetCastShadow(true);
		const float Size = Random.FRandRange(0.4f, 1.3f);
		Piece->SetWorldScale3D(FVector(Size, Size * Random.FRandRange(0.7f, 1.3f), Size * Random.FRandRange(0.5f, 1.1f)));
		const FVector Start = Centre + FVector(Random.FRandRange(-25.0f, 25.0f), Random.FRandRange(-25.0f, 25.0f), Random.FRandRange(10.0f, 40.0f));
		Piece->SetWorldLocationAndRotation(Start, FRotator(Random.FRand() * 360.0f, Random.FRand() * 360.0f, Random.FRand() * 360.0f));
		Piece->RegisterComponent();
		Piece->SetSimulatePhysics(true);
		Piece->SetEnableGravity(true);
		Piece->SetLinearDamping(0.9f);
		Piece->SetAngularDamping(0.4f);
		Piece->SetMassOverrideInKg(NAME_None, Random.FRandRange(0.3f, 2.0f), true);
		// Outward and upward, harder the bigger the blast.
		FVector Out = (Start - Centre).GetSafeNormal2D();
		if (Out.IsNearlyZero())
		{
			Out = Random.VRand().GetSafeNormal2D();
		}
		const float Speed = Random.FRandRange(250.0f, 650.0f) * FMath::Clamp(0.6f + 0.25f * RadiusM, 0.7f, 2.0f);
		const FVector Velocity = Out * Speed + FVector(0.0, 0.0, Random.FRandRange(300.0f, 750.0f));
		Piece->SetPhysicsLinearVelocity(Velocity);
		Piece->SetPhysicsAngularVelocityInDegrees(Random.VRand() * Random.FRandRange(200.0f, 900.0f));
		FFeelDebris Entry;
		Entry.Piece = Piece;
		Entry.Start = Centre;
		Debris.Add(Entry);
		++DebrisMade;
	}
}

int32 UShooterFeelSubsystem::GetDebrisAlive() const
{
	int32 Count = 0;
	for (const FFeelDebris& Piece : Debris)
	{
		Count += Piece.Piece.IsValid() ? 1 : 0;
	}
	return Count;
}

// ---------------------------------------------------------------------------
// Per frame.

void UShooterFeelSubsystem::UpdateKick(float DeltaTime, const colony::Sim& Sim, const USimSubsystem& Source)
{
	(void)Sim;
	const colony::Entity Me = ControlledPawn();
	const colony::ThingView* View = Me != colony::kNoEntity ? Source.GetCurrentSnapshot().find(Me) : nullptr;
	const float Scale = GetKickScale();
	// The sim's kick is in degrees: the view lifts and swings by it (up is positive pitch, right positive yaw).
	const float WantPitch = View ? View->kickUp * Scale : 0.0f;
	const float WantYaw = View ? View->kickSide * Scale : 0.0f;
	KickPitch = FMath::FInterpTo(KickPitch, WantPitch, DeltaTime, 40.0f);
	KickYaw = FMath::FInterpTo(KickYaw, WantYaw, DeltaTime, 40.0f);

	// A dodge roll ducks the camera under it and kicks the blur: from where the sim has the roll (27 steps in all).
	float Dip = 0.0f;
	float RollBlur = 0.0f;
	if (View && View->rollPhase > 0)
	{
		const float Progress = FMath::Clamp(1.0f - static_cast<float>(View->rollStepsLeft) / 27.0f, 0.0f, 1.0f);
		Dip = FMath::Sin(PI * Progress);
		RollBlur = View->rollPhase == 2 ? 0.7f : 0.25f;
	}
	RollDip = FMath::FInterpTo(RollDip, Dip, DeltaTime, 18.0f);
	BlurPulse = FMath::Max(BlurPulse - DeltaTime * 1.4f, RollBlur);
	VignettePulse = FMath::Max(0.0f, VignettePulse - DeltaTime * 1.1f);
}

void UShooterFeelSubsystem::Advance(float DeltaTime)
{
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim())
	{
		return;
	}
	UpdateKick(DeltaTime, *Source->GetSim(), *Source);

	// Shake: the sum of the live shakes, each a few sines at its own pitch that die away over its time.
	FRotator Shake = FRotator::ZeroRotator;
	FVector ShakeMove = FVector::ZeroVector;
	for (int32 I = Shakes.Num() - 1; I >= 0; --I)
	{
		FShake& S = Shakes[I];
		S.Age += DeltaTime;
		if (S.Age >= S.Duration)
		{
			Shakes.RemoveAtSwap(I);
			continue;
		}
		const float Envelope = FMath::Square(1.0f - S.Age / S.Duration);
		const float T = S.Age * S.Frequency + S.Seed;
		const float Amount = S.Degrees * Envelope;
		Shake.Pitch += Amount * (0.65f * FMath::Sin(T) + 0.35f * FMath::Sin(T * 2.3f + 1.7f));
		Shake.Yaw += Amount * (0.65f * FMath::Sin(T * 0.83f + 2.1f) + 0.35f * FMath::Sin(T * 1.9f));
		Shake.Roll += Amount * 0.5f * FMath::Sin(T * 1.27f + 0.6f);
		ShakeMove += FVector(0.0, Amount * 0.9f * FMath::Sin(T * 1.1f + 0.3f), Amount * 0.9f * FMath::Sin(T * 0.9f + 1.1f));
	}
	const float ShakeScale = GetShakeScale();
	Shake *= ShakeScale;
	ShakeMove *= ShakeScale;
	ShakeNow = FMath::Max3(FMath::Abs(Shake.Pitch), FMath::Abs(Shake.Yaw), FMath::Abs(Shake.Roll));
	LargestShake = FMath::Max(LargestShake, ShakeNow);

	Feel.Rotation = FRotator(KickPitch + Shake.Pitch - RollDip * 9.0f, KickYaw + Shake.Yaw, Shake.Roll);
	Feel.Location = ShakeMove + FVector(0.0, 0.0, -RollDip * 55.0f);
	const float Blur = GetBlurScale();
	Feel.MotionBlur = Blur <= 0.0f ? 0.0f : FMath::Clamp(0.5f * Blur + BlurPulse * 0.8f * Blur, 0.0f, 1.0f);
	Feel.Vignette = VignettePulse * FMath::Min(Blur, 1.0f);
	Feel.Fringe = FMath::Min(1.5f, BlurPulse * 1.2f + VignettePulse) * FMath::Min(Blur, 1.0f);
}

const FCameraFeel& UShooterFeelSubsystem::GetCameraFeel()
{
	if (FeelFrame != GFrameCounter)
	{
		FeelFrame = GFrameCounter;
		Advance(GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.016f);
	}
	return Feel;
}

void UShooterFeelSubsystem::UpdateFlash(float DeltaTime)
{
	FlashAge += DeltaTime;
	const float Life = 0.55f;
	FlashIntensity = FlashAge < Life ? FlashPeak * FMath::Pow(1.0f - FlashAge / Life, 3.0f) : 0.0f;
	for (int32 I = 0; I < FlashLights.Num(); ++I)
	{
		UPointLightComponent* Light = FlashLights[I];
		if (!Light)
		{
			continue;
		}
		const bool bOn = I == 0 && FlashIntensity > 5.0f;
		if (Light->IsVisible() != bOn)
		{
			Light->SetVisibility(bOn);
		}
		if (bOn)
		{
			Light->SetIntensity(FlashIntensity);
		}
	}
}

void UShooterFeelSubsystem::UpdateParticles(float DeltaTime, const USimSubsystem& Source)
{
	TArray<TArray<FTransform>> ByKind;
	ByKind.SetNum(static_cast<int32>(EBlastParticle::Count));
	for (int32 Index = Particles.Num() - 1; Index >= 0; --Index)
	{
		FParticle& P = Particles[Index];
		P.Age += DeltaTime;
		if (P.Age >= P.Life)
		{
			Particles.RemoveAtSwap(Index);
			continue;
		}
		P.Velocity.Z -= P.Gravity * DeltaTime;
		P.Velocity *= FMath::Max(0.0f, 1.0f - (P.Kind == EBlastParticle::Spark ? 0.8f : 1.6f) * DeltaTime);
		P.Position += P.Velocity * DeltaTime;
		const float T = P.Age / P.Life;
		float Scale = P.SizeCm / 10.0f;
		switch (P.Kind)
		{
			case EBlastParticle::FireCore:
			case EBlastParticle::FireOuter:
				Scale *= (0.4f + 1.1f * FMath::Sqrt(T)) * (1.0f - FMath::Square(T));
				break;
			case EBlastParticle::Smoke:
				Scale *= (0.5f + 1.7f * T) * (T > 0.7f ? (1.0f - T) / 0.3f : 1.0f);
				break;
			case EBlastParticle::Dust:
				Scale *= (0.5f + 1.2f * T) * (1.0f - T);
				break;
			default:
				Scale = P.SizeCm / 2.0f * (1.0f - T);
				break;
		}
		ByKind[static_cast<int32>(P.Kind)].Add(FTransform(FRotator(P.Age * 400.0f, P.Age * 700.0f, 0.0f), P.Position, FVector(FMath::Max(Scale, 0.01f))));
	}

	// Grenades the sim has in the air or lying with their fuse lit (a Projectiles def that explodes).
	GrenadesDrawn = 0;
	const colony::Snapshot& Now = Source.GetCurrentSnapshot();
	const colony::Snapshot& Before = Source.GetPreviousSnapshot();
	const float Alpha = Source.GetSpeed() > 0 ? Source.GetBlendAlpha() : 1.0f;
	for (const colony::ProjectileView& View : Now.projectiles)
	{
		if (!View.def.valid() || !Source.GetDefs() || !Source.GetDefs()->get(View.def).has("explosion"))
		{
			continue;
		}
		FVector At = FeelSimToWorld(Source, View.x, View.y, View.z);
		for (const colony::ProjectileView& Old : Before.projectiles)
		{
			if (Old.id == View.id)
			{
				At = FMath::Lerp(FeelSimToWorld(Source, Old.x, Old.y, Old.z), At, Alpha);
				break;
			}
		}
		// A lit fuse blinks quicker as it runs out.
		const float Blink = View.fuse >= 0 ? 1.0f + 0.35f * FMath::Sin(static_cast<float>(View.fuse) * (0.6f + 6.0f / (1.0f + View.fuse))) : 1.0f;
		ByKind[static_cast<int32>(EBlastParticle::Grenade)].Add(FTransform(FRotator(static_cast<float>(Now.gameSeconds) * 20.0f, static_cast<float>(Now.gameSeconds) * 35.0f, 0.0f), At + FVector(0.0, 0.0, 4.0), FVector(Blink)));
		++GrenadesDrawn;
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

void UShooterFeelSubsystem::UpdateDebris(float DeltaTime)
{
	for (int32 Index = Debris.Num() - 1; Index >= 0; --Index)
	{
		FFeelDebris& Piece = Debris[Index];
		if (!Piece.Piece.IsValid())
		{
			Debris.RemoveAtSwap(Index);
			continue;
		}
		Piece.Age += DeltaTime;
		const FVector Where = Piece.Piece->GetComponentLocation();
		DebrisLowestCm = FMath::Min(DebrisLowestCm, static_cast<float>(Where.Z - Piece.Start.Z));
		DebrisHighestCm = FMath::Max(DebrisHighestCm, static_cast<float>(Where.Z - Piece.Start.Z));
		DebrisFarthestCm = FMath::Max(DebrisFarthestCm, static_cast<float>(FVector::Dist2D(Where, Piece.Start)));
		if (Piece.Age > FeelDebrisSeconds || Where.Z < Piece.Start.Z - 400.0)
		{
			Piece.Piece->DestroyComponent();
			Debris.RemoveAtSwap(Index);
		}
	}
	for (int32 Index = Floors.Num() - 1; Index >= 0; --Index)
	{
		FloorAges[Index] += DeltaTime;
		if (FloorAges[Index] > FeelFloorSeconds)
		{
			if (Floors[Index])
			{
				Floors[Index]->DestroyComponent();
			}
			Floors.RemoveAt(Index);
			FloorAges.RemoveAt(Index);
		}
	}
}

void UShooterFeelSubsystem::Tick(float DeltaTime)
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
		Shakes.Reset();
		KickPitch = KickYaw = RollDip = BlurPulse = VignettePulse = 0.0f;
	}
	GetCameraFeel();
	if (Holder)
	{
		UpdateParticles(DeltaTime, *Source);
		UpdateFlash(DeltaTime);
		UpdateDebris(DeltaTime);
	}
	else if (!Source->GetCurrentSnapshot().projectiles.empty())
	{
		EnsureScene();
	}
}

// ---------------------------------------------------------------------------
// The settings panel.

void UShooterFeelSubsystem::DrawSettingsPanel(float ViewportWidth, float Scale)
{
	ImGui::SetNextWindowPos(ImVec2(ViewportWidth - 12.0f * Scale, 70.0f * Scale), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.7f);
	ImGui::SetNextWindowCollapsed(true, ImGuiCond_Once);
	if (!ImGui::Begin("Feel (direct mode)##Feel", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
	{
		ImGui::End();
		return;
	}
	bool bChanged = false;
	float Shake = GetShakeScale();
	float Blur = GetBlurScale();
	float Kick = GetKickScale();
	float Fov = GetAimFov();
	bool bFirst = IsAimFirstPerson();
	if (ImGui::SliderFloat("Camera shake", &Shake, 0.0f, 2.0f, "%.2f"))
	{
		GFeelShake.AsVariable()->Set(Shake, ECVF_SetByCode);
		bChanged = true;
	}
	if (ImGui::SliderFloat("Motion blur", &Blur, 0.0f, 1.5f, "%.2f"))
	{
		GFeelBlur.AsVariable()->Set(Blur, ECVF_SetByCode);
		bChanged = true;
	}
	if (ImGui::SliderFloat("Gun kick", &Kick, 0.0f, 2.0f, "%.2f"))
	{
		GFeelKick.AsVariable()->Set(Kick, ECVF_SetByCode);
		bChanged = true;
	}
	if (ImGui::SliderFloat("Aim field of view", &Fov, 40.0f, 80.0f, "%.0f"))
	{
		GFeelAimFov.AsVariable()->Set(Fov, ECVF_SetByCode);
		bChanged = true;
	}
	if (ImGui::Checkbox("Hold aim goes to first person", &bFirst))
	{
		GFeelAimView.AsVariable()->Set(bFirst ? 1 : 0, ECVF_SetByCode);
		bChanged = true;
	}
	ImGui::TextUnformatted("0 turns shake, blur or kick off. Hold the right mouse");
	ImGui::TextUnformatted("button with a gun to aim; Left Shift rolls; G throws a grenade.");
	if (bChanged)
	{
		SaveSettings();
	}
	ImGui::End();
}
