#include "WeaponDrawSubsystem.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"

#include "FightSubsystem.h"
#include "GunRangeSubsystem.h"
#include "HoldDraw.h"
#include "OrdersSubsystem.h"
#include "PawnActor.h"
#include "PawnAnimInstance.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "VisualMesh.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Hold.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/WeaponSlot.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeaponDraw, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GWeaponDraw(
		TEXT("Weapons.Draw"), 1, TEXT("1 draws the weapons pawns carry and their swings, 0 draws none."), ECVF_Default);

	TAutoConsoleVariable<int32> GWeaponHands(
		TEXT("Weapons.Hands"), 1, TEXT("1 puts held weapons in the hands by their grip points (docs/design/weapon-hold.md), 0 floats them as before."), ECVF_Default);
	TAutoConsoleVariable<int32> GWeaponStances(
		TEXT("Weapons.Stances"), 1, TEXT("1 plays the held weapon's upper-body stance, aim, block and reload clips, 0 none."), ECVF_Default);
	TAutoConsoleVariable<int32> GWeaponHolsterIdle(
		TEXT("Weapons.HolsterIdle"), 1, TEXT("1 holsters the weapon while a pawn is not drafted, fighting or the player's, 0 keeps it in the hands."), ECVF_Default);
	TAutoConsoleVariable<int32> GWeaponGunArt(
		TEXT("Weapons.GunModels"), 1, TEXT("1 draws guns with a real model (assets/guns.json) in third person too, 0 the defs' plain shapes."), ECVF_Default);

	// How fast a weapon comes out of its holster or goes back (per real second: about 0.4 s), and how far a body on the
	// back bends its carrier forward (degrees).
	constexpr float WeaponReadyRate = 2.5f;
	constexpr float WeaponCarryBend = 26.0f;

	const TCHAR* const WeaponSolidMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");

	// A sidearm is drawn at half the rifle-sized gun the defs share.
	constexpr float WeaponSidearmScale = 0.55f;
	// The colonist's view must be this close to the line to a pawn for a swing to pick it (cosine of 70 degrees).
	constexpr float WeaponSwingCone = 0.34f;
	// Hit-stop, real seconds: a hit, armor that turned the blow, a blow a weapon took.
	constexpr float WeaponHitStopBase = 0.05f;
	constexpr float WeaponHitStopMax = 0.12f;
	constexpr float WeaponHitStopDeflected = 0.04f;
	constexpr float WeaponHitStopBlocked = 0.08f;
	// The shield, actor space (cm, X forward, +Y right, from the feet): hung on the left arm facing out, raised in front
	// of the chest, slung on the back facing behind. How fast it comes up and goes down (per real second).
	const FVector WeaponShieldLowered(12.0, -30.0, 96.0);
	const FRotator WeaponShieldLoweredRotation(0.0, -70.0, 0.0);
	const FVector WeaponShieldRaised(40.0, -6.0, 118.0);
	const FRotator WeaponShieldRaisedRotation(0.0, -8.0, 0.0);
	const FVector WeaponShieldSlungAt(-26.0, 0.0, 112.0);
	const FRotator WeaponShieldSlungRotation(0.0, 180.0, 0.0);
	constexpr float WeaponShieldRaiseRate = 7.0f;
	constexpr float WeaponShieldSlingRate = 2.5f;
	// The right-mouse shield follows the camera: sent again when the view turns more than 15 degrees, at most 5 times a second.
	constexpr float WeaponShieldResendCosine = 0.966f;
	constexpr double WeaponShieldResendSeconds = 0.2;

	FVector WeaponSimToWorld(const USimSubsystem& Bridge, float X, float Y, float Z)
	{
		return Bridge.GetWorldOrigin() + FVector(X * 100.0, Y * 100.0, Z * 100.0);
	}
}

bool UWeaponDrawSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UWeaponDrawSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWeaponDrawSubsystem, STATGROUP_Tickables);
}

bool UWeaponDrawSubsystem::DrawOn() const
{
	return GWeaponDraw.GetValueOnGameThread() != 0;
}

void UWeaponDrawSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		BridgeSource = Bridge;
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &UWeaponDrawSubsystem::OnSimEvent);
	}
	Collection.InitializeDependency<USimPawnSubsystem>();
	Collection.InitializeDependency<UFightSubsystem>();
	Collection.InitializeDependency<UProtagonistSubsystem>();
}

void UWeaponDrawSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = BridgeSource.Get())
	{
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------
// Sim events.

void UWeaponDrawSubsystem::OnSimEvent(const colony::Event& Event)
{
	switch (Event.type)
	{
		case colony::EventType::MeleeSwing:
			OnMeleeSwing(Event);
			break;
		case colony::EventType::MeleeHit:
			OnMeleeHit(Event);
			break;
		case colony::EventType::PawnAction:
			OnPawnAction(Event);
			break;
		case colony::EventType::ShieldBlocked:
			OnShieldBlocked(Event);
			break;
		case colony::EventType::WeaponSwitched:
			++SwitchesDone;
			if (FPawnWeapons* Found = Weapons.Find(entt::to_integral(Event.thing)))
			{
				Found->SwitchTotal = 0;
			}
			break;
		default:
			break;
	}
}

void UWeaponDrawSubsystem::OnMeleeSwing(const colony::Event& Event)
{
	++SwingsSeen;
	const USimSubsystem* Bridge = BridgeSource.Get();
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	ASimPawnActor* Actor = Pawns ? Pawns->FindPawn(Event.thing) : nullptr;
	if (!Bridge || !Bridge->GetDefs() || !Actor || !DrawOn())
	{
		return;
	}
	// A swing with the weapon in the hands (the event's def is the weapon) or with the body (fists, teeth).
	const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Event.thing);
	const bool bWeapon = View && View->weapon.valid() && View->weapon == Event.def && !IsGun(Event.def);
	EMeleeStyle Style = EMeleeStyle::Punch;
	if (bWeapon)
	{
		const std::string& Name = Bridge->GetDefs()->get(Event.def).defName();
		Style = (Name == "Melee_Spear" || Name == "Melee_Knife") ? EMeleeStyle::Thrust : EMeleeStyle::Chop;
	}
	Actor->BeginMeleeSwing(Style, Event.amount);
}

void UWeaponDrawSubsystem::OnMeleeHit(const colony::Event& Event)
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	if (!Bridge)
	{
		return;
	}
	LastHitStep = Bridge->GetGameSeconds();
	const bool bHit = Event.message == "hit";
	const bool bDeflected = Event.message == "deflected";
	const bool bBlocked = Event.message == "blocked";
	MeleeHits += bHit ? 1 : 0;
	MeleeDeflected += bDeflected ? 1 : 0;
	MeleeBlocked += bBlocked ? 1 : 0;
	MeleeDodged += Event.message == "dodged" ? 1 : 0;
	MeleeMissed += Event.message == "missed" ? 1 : 0;
	if (!DrawOn())
	{
		return;
	}

	ASimPawnActor* Attacker = Pawns ? Pawns->FindPawn(Event.thing) : nullptr;
	ASimPawnActor* Victim = Pawns ? Pawns->FindPawn(Event.target) : nullptr;
	if (Attacker)
	{
		// The blow should have landed on screen about now: how far behind (in steps) is the picture?
		if (Attacker->GetSwing().IsWinding())
		{
			const float Behind = Attacker->GetSwing().StepsToHit();
			MaxStepsBehind = FMath::Max(MaxStepsBehind, Behind);
			(Behind <= 2.5f ? HitsOnTime : HitsLate) += 1;
		}
		Attacker->MeleeHitMoment();
	}

	const FVector At = WeaponSimToWorld(*Bridge, Event.x, Event.y, Event.z);
	FVector Direction = Attacker ? At - Attacker->GetActorLocation() : FVector::ForwardVector;
	Direction.Z = 0.0;
	Direction = Direction.GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		Direction = Attacker ? Attacker->GetActorForwardVector() : FVector::ForwardVector;
	}

	UFightSubsystem* Fight = GetWorld()->GetSubsystem<UFightSubsystem>();
	float Stop = 0.0f;
	if (bHit)
	{
		if (Fight)
		{
			Fight->SpawnMeleeImpact(Event.hit == colony::ProjectileHitKind::None ? colony::ProjectileHitKind::Pawn : Event.hit, Event.thing, Event.target, At,
				Direction, Event.part, Event.amount);
		}
		Stop = FMath::Min(WeaponHitStopMax, WeaponHitStopBase + Event.amount * 0.004f);
	}
	else if (bDeflected)
	{
		if (Fight)
		{
			Fight->SpawnMeleeImpact(colony::ProjectileHitKind::Armor, Event.thing, Event.target, At, Direction, Event.part, Event.amount);
		}
		Stop = WeaponHitStopDeflected;
	}
	else if (bBlocked)
	{
		// The blow rings off the blocker's weapon: sparks where it met, and the blocker staggers.
		if (Fight)
		{
			Fight->SpawnMeleeImpact(colony::ProjectileHitKind::Armor, Event.thing, Event.target, At, Direction, -1, 4.0f);
		}
		Stop = WeaponHitStopBlocked;
	}
	else if (Event.hit == colony::ProjectileHitKind::Building && Fight)
	{
		Fight->SpawnMeleeImpact(colony::ProjectileHitKind::Building, Event.thing, Event.target, At, Direction, -1, 0.0f);
	}

	if (Stop > 0.0f && Bridge->GetSpeed() > 0)
	{
		if (Attacker)
		{
			Attacker->HitStop(Stop);
		}
		if (Victim && Victim != Attacker)
		{
			Victim->HitStop(Stop);
		}
		++HitStopsPlayed;
	}
}

void UWeaponDrawSubsystem::OnPawnAction(const colony::Event& Event)
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	switch (Event.action)
	{
		case colony::PawnAction::Dodge:
		{
			++DodgeActions;
			if (ASimPawnActor* Actor = Pawns && Bridge && DrawOn() ? Pawns->FindPawn(Event.thing) : nullptr)
			{
				Actor->PlayDodge(Bridge->SimToWorld(static_cast<float>(Event.cell.x) + 0.5f, static_cast<float>(Event.cell.y) + 0.5f, Event.cell.level));
			}
			break;
		}
		case colony::PawnAction::Block:
			++BlockActions;
			break;
		case colony::PawnAction::Switch:
		{
			++SwitchesStarted;
			Weapons.FindOrAdd(entt::to_integral(Event.thing)).SwitchTotal = FMath::Max(1, FMath::RoundToInt(Event.amount));
			break;
		}
		default:
			break;
	}
}

void UWeaponDrawSubsystem::OnShieldBlocked(const colony::Event& Event)
{
	++ShieldBlocksSeen;
	USimSubsystem* Bridge = BridgeSource.Get();
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	UFightSubsystem* Fight = GetWorld()->GetSubsystem<UFightSubsystem>();
	ASimPawnActor* Actor = Pawns ? Pawns->FindPawn(Event.thing) : nullptr;
	if (!Bridge || !Fight || !Actor || !DrawOn())
	{
		return;
	}
	// Sparks off the face of the shield, thrown back toward whoever hit it.
	const UStaticMeshComponent* Shield = GetShieldComponent(Event.thing);
	const FVector Forward = Actor->GetActorForwardVector();
	const FVector At = Shield && Shield->IsVisible() ? Shield->GetComponentLocation() + Forward * 4.0 : Actor->GetActorLocation() + Forward * 40.0 + FVector(0.0, 0.0, 110.0);
	Fight->SpawnMeleeImpact(colony::ProjectileHitKind::Armor, colony::kNoEntity, Event.thing, At, -Forward, -1, FMath::Max(2.0f, Event.amount));
}

// ---------------------------------------------------------------------------
// Meshes and poses.

bool UWeaponDrawSubsystem::LoadSolidMaterial()
{
	if (!SolidMaterial)
	{
		SolidMaterial = LoadObject<UMaterialInterface>(nullptr, WeaponSolidMaterialPath);
		if (!SolidMaterial)
		{
			UE_LOG(LogWeaponDraw, Error, TEXT("Could not load %s; weapons cannot be drawn"), WeaponSolidMaterialPath);
		}
	}
	return SolidMaterial != nullptr;
}

UStaticMesh* UWeaponDrawSubsystem::ShieldMesh(colony::DefId Shield, colony::DefId Stuff)
{
	const uint64 Key = (static_cast<uint64>(Shield.value) << 32) | static_cast<uint64>(Stuff.value);
	if (const TObjectPtr<UStaticMesh>* Found = ShieldMeshes.Find(Key))
	{
		return *Found;
	}
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetDefs() || !LoadSolidMaterial())
	{
		return nullptr;
	}
	const colony::DefDatabase& Defs = *Bridge->GetDefs();
	TArray<VisualMesh::FPart> Parts;
	VisualMesh::ReadParts(Defs.get(Shield), "visual", FVector2f(100.0f, 100.0f), Parts);
	if (Parts.IsEmpty())
	{
		VisualMesh::FPart Fallback;
		Fallback.Size = FVector3f(3.0f, 55.0f, 70.0f);
		Fallback.At = FVector3f(0.0f, 0.0f, -35.0f);
		Fallback.bStuff = true;
		Parts.Add(Fallback);
	}
	// The material's colour; plasteel and uranium have none in their defs.
	FLinearColor Color(0.42f, 0.46f, 0.53f);
	if (Stuff.valid() && Stuff.value < Defs.size())
	{
		const colony::Def& Material = Defs.get(Stuff);
		const std::string& Name = Material.defName();
		const FLinearColor Fallback = Name == "Plasteel" ? FLinearColor(0.62f, 0.68f, 0.72f)
			: Name == "Uranium"                         ? FLinearColor(0.30f, 0.36f, 0.30f)
														: Color;
		Color = VisualMesh::ReadColor(Material, "visual.color", Fallback);
	}
	UStaticMesh* Mesh = VisualMesh::MakeMesh(this, SolidMaterial, Parts, VisualMesh::EPartSet::All, &Color, false);
	ShieldMeshes.Add(Key, Mesh);
	return Mesh;
}

float UWeaponDrawSubsystem::GetReady(colony::Entity Pawn) const
{
	const FPawnWeapons* Found = Weapons.Find(entt::to_integral(Pawn));
	return Found ? Found->Ready : 0.0f;
}

UStaticMesh* UWeaponDrawSubsystem::GetWeaponMesh(colony::DefId Weapon)
{
	return Weapon.valid() ? WeaponMesh(Weapon) : nullptr;
}

UStaticMesh* UWeaponDrawSubsystem::GetShieldMesh(colony::DefId Shield, colony::DefId Stuff)
{
	return Shield.valid() ? ShieldMesh(Shield, Stuff) : nullptr;
}

bool UWeaponDrawSubsystem::IsGun(colony::DefId Weapon) const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	return Weapon.valid() && Bridge && Bridge->GetDefs() && Bridge->GetDefs()->get(Weapon).has("verbs");
}

UStaticMesh* UWeaponDrawSubsystem::WeaponMesh(colony::DefId Weapon)
{
	if (const TObjectPtr<UStaticMesh>* Found = Meshes.Find(Weapon.value))
	{
		return *Found;
	}
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetDefs())
	{
		return nullptr;
	}
	if (!LoadSolidMaterial())
	{
		return nullptr;
	}
	TArray<VisualMesh::FPart> Parts;
	VisualMesh::ReadParts(Bridge->GetDefs()->get(Weapon), "visual", FVector2f(100.0f, 100.0f), Parts);
	if (Parts.IsEmpty())
	{
		VisualMesh::FPart Fallback;
		Fallback.Size = FVector3f(60.0f, 6.0f, 10.0f);
		Fallback.Color = FLinearColor(0.12f, 0.12f, 0.14f);
		Parts.Add(Fallback);
	}
	UStaticMesh* Mesh = VisualMesh::MakeMesh(this, SolidMaterial, Parts, VisualMesh::EPartSet::All, nullptr, false);
	Meshes.Add(Weapon.value, Mesh);
	return Mesh;
}

UStaticMeshComponent* UWeaponDrawSubsystem::MakeComponent(ASimPawnActor& Actor, UStaticMesh* Mesh)
{
	UStaticMeshComponent* Weapon = NewObject<UStaticMeshComponent>(&Actor);
	Weapon->SetStaticMesh(Mesh);
	Weapon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Weapon->SetGenerateOverlapEvents(false);
	Weapon->SetCanEverAffectNavigation(false);
	Weapon->SetCastShadow(false);
	Weapon->bReceivesDecals = false;
	Weapon->SetupAttachment(Actor.GetRootComponent());
	Weapon->RegisterComponent();
	return Weapon;
}

void UWeaponDrawSubsystem::DestroyDisplayed(FDisplayed& Shown)
{
	for (const TWeakObjectPtr<UStaticMeshComponent>& Part : Shown.Parts)
	{
		if (Part.IsValid())
		{
			Part->DestroyComponent();
		}
	}
	if (Shown.Component.IsValid())
	{
		Shown.Component->DestroyComponent();
	}
	Shown = FDisplayed();
}

// The weapon's meshes: a real gun model's parts (assets/guns.json: the body, then its moving parts on it), else the
// def's own shapes.
bool UWeaponDrawSubsystem::MakeDisplayed(ASimPawnActor& Actor, FDisplayed& Shown, colony::DefId Def, const FGunRangeGun* Art)
{
	UGunRangeSubsystem* Range = GetWorld()->GetSubsystem<UGunRangeSubsystem>();
	UStaticMesh* Body = nullptr;
	if (Art && Range && !Art->Parts.IsEmpty())
	{
		Body = Range->FindMesh(Art->ArtName, Art->Parts[0].Match);
	}
	UStaticMesh* Mesh = Body ? Body : WeaponMesh(Def);
	if (!Mesh)
	{
		return false;
	}
	Shown.Component = MakeComponent(Actor, Mesh);
	Shown.Def = Def;
	Shown.bArt = Body != nullptr;
	if (Body)
	{
		const FTransform MeshFix(Art->MeshRotation, Art->MeshOffset);
		Shown.Component->SetCastShadow(true);
		for (int32 i = 1; i < Art->Parts.Num(); ++i)
		{
			if (UStaticMesh* PartMesh = Range->FindMesh(Art->ArtName, Art->Parts[i].Match))
			{
				UStaticMeshComponent* Part = MakeComponent(Actor, PartMesh);
				Part->SetCastShadow(true);
				Part->AttachToComponent(Shown.Component.Get(), FAttachmentTransformRules::KeepRelativeTransform);
				Part->SetRelativeTransform(FTransform::Identity);
				Shown.Parts.Add(Part);
			}
		}
		// The model's own fix-up (zero for the guns so far): the body carries it, the parts ride on the body.
		(void)MeshFix;
	}
	return true;
}

// Where a carried weapon rides on the body (actor space, cm). `Height` is 1 standing, less crouching and prone.
void UWeaponDrawSubsystem::HolsterPose(int32 Slot, float Height, FVector& OutLocation, FRotator& OutRotation) const
{
	switch (Slot)
	{
		case static_cast<int32>(colony::WeaponSlot::Sidearm):
			OutLocation = FVector(4.0, 23.0, 80.0 * Height);
			OutRotation = FRotator(-82.0, 0.0, 0.0);
			break;
		case static_cast<int32>(colony::WeaponSlot::Melee):
			OutLocation = FVector(-2.0, -23.0, 86.0 * Height);
			OutRotation = FRotator(-62.0, 180.0, 0.0);
			break;
		default:
			// Slung across the back, muzzle up over the shoulder.
			OutLocation = FVector(-14.0, 6.0, 104.0 * Height);
			OutRotation = FRotator(62.0, 0.0, 0.0);
			break;
	}
}

// Where it is when it is in the hands. A gun floats at the chest pointing where the pawn faces, as U11 had it;
// a melee weapon follows the pawn's swing and block.
void UWeaponDrawSubsystem::HoldPose(int32 Slot, float Height, FVector& OutLocation, FRotator& OutRotation) const
{
	OutLocation = FVector(26.0, 14.0, 118.0 * Height);
	OutRotation = FRotator::ZeroRotator;
	(void)Slot;
}

// ---------------------------------------------------------------------------
// Per frame.

void UWeaponDrawSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	FrameDelta = DeltaTime;
	USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	if (Bridge->GetSimGeneration() != LastGeneration)
	{
		LastGeneration = Bridge->GetSimGeneration();
		Weapons.Reset();
		LastOrderStep = -1;
		bShieldWanted = false;
		ShieldPawn = colony::kNoEntity;
	}
	UpdatePlayerInput();

	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	if (!Pawns)
	{
		return;
	}
	TSet<uint32> Seen;
	const bool bDraw = DrawOn();
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
		Seen.Add(entt::to_integral(View.id));
		if (bDraw)
		{
			UpdatePawn(*Actor, View);
		}
		else if (FPawnWeapons* Found = Weapons.Find(entt::to_integral(View.id)))
		{
			for (FDisplayed& Slot : Found->Slots)
			{
				if (Slot.Component.IsValid())
				{
					Slot.Component->SetVisibility(false);
				}
			}
			if (Found->Shield.IsValid())
			{
				Found->Shield->SetVisibility(false);
			}
		}
	}
	for (auto It = Weapons.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
}

void UWeaponDrawSubsystem::UpdatePawn(ASimPawnActor& Actor, const colony::ThingView& View)
{
	FPawnWeapons& Pawn = Weapons.FindOrAdd(entt::to_integral(View.id));
	const bool bShow = !View.downed && !View.dead && !Actor.IsRagdoll();
	const float Height = Actor.GetStanceHeight();
	// The body's own height (docs/design/gear-size.md): every place below was measured on the 1.80 m man, so it is
	// scaled to this body, and the gear itself grows or shrinks part of the way (LooksRules gearSizeFollow).
	const USimSubsystem* Bridge = BridgeSource.Get();
	const colony::DefDatabase* Defs = Bridge ? Bridge->GetDefs().get() : nullptr;
	const colony::GearSize Gear = Bridge && Bridge->GetSim() ? colony::gearSizeFor(Bridge->GetSim()->defs(), Actor.GetBodyScale())
																: colony::GearSize{};
	const double Place = Gear.place;
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const bool bControlled = Protagonist && Protagonist->IsDirectMode() && Protagonist->GetControlled() == View.id;
	USkeletalMeshComponent* Mesh = HoldDraw::VisibleMesh(Actor);
	UGunRangeSubsystem* Range = GetWorld()->GetSubsystem<UGunRangeSubsystem>();

	// What sits in each display slot, and which one is in the hands.
	colony::DefId Wanted[GWeaponDisplaySlots];
	for (int32 Slot = 0; Slot < colony::kWeaponSlotCount; ++Slot)
	{
		Wanted[Slot] = View.carried[Slot];
	}
	int32 InHands = -1;
	if (View.weapon.valid())
	{
		for (int32 Slot = 0; Slot < colony::kWeaponSlotCount; ++Slot)
		{
			if (Wanted[Slot].valid() && Wanted[Slot] == View.weapon)
			{
				InHands = Slot;
			}
		}
		if (InHands < 0)
		{
			InHands = colony::kWeaponSlotCount;
			Wanted[InHands] = View.weapon;
		}
	}

	// A switch in progress: the weapon in the hands goes to its holster in the first half, the new one comes up in the second.
	int32 Incoming = -1;
	float Progress = 0.0f;
	if (View.switchSlot < colony::kWeaponSlotCount)
	{
		Incoming = View.switchSlot;
		const int32 Total = FMath::Max(Pawn.SwitchTotal, View.switchStepsLeft);
		Progress = Total > 0 ? 1.0f - static_cast<float>(View.switchStepsLeft) / static_cast<float>(Total) : 1.0f;
		Progress = FMath::Clamp(Progress, 0.0f, 1.0f);
	}

	// Ready (docs/design/weapon-hold.md): the weapon is in the hands while the pawn is drafted, fighting, blocking,
	// swinging, reloading or switching, or is the player's own; otherwise it rides in its holster, so a colonist
	// at work has free hands. Carrying a body on the back always holsters it.
	const bool bCarrying = View.carryingPawn != colony::kNoEntity;
	const bool bReady = !bCarrying && (GWeaponHolsterIdle.GetValueOnGameThread() == 0 || View.drafted || View.fighting || View.blocking ||
		View.shieldUp || bControlled || View.reloading || Incoming >= 0 || Actor.IsInMotionClip() || Actor.GetSwing().IsActive());
	Pawn.Ready = FMath::FInterpConstantTo(Pawn.Ready, bReady ? 1.0f : 0.0f, FrameDelta, WeaponReadyRate);
	const float ReadyEase = MeleePose::EaseInOut(Pawn.Ready);
	const bool bHands = GWeaponHands.GetValueOnGameThread() != 0 && Mesh && Height > 0.4f && !View.swimming;

	// The frame the holsters ride in: the actor's, carried along with the hips (a lunge), and through a roll, stagger or
	// knockdown clip turned and moved with the pelvis as well (so holsters and a slung shield stay on the body), eased in
	// and out with the clip.
	FTransform BodyFrame = Actor.GetActorTransform();
	BodyFrame.SetScale3D(FVector::OneVector);
	if (Mesh && Mesh->GetBoneIndex(TEXT("pelvis")) != INDEX_NONE)
	{
		FTransform PelvisWorld = Mesh->GetSocketTransform(TEXT("pelvis"));
		PelvisWorld.SetScale3D(FVector::OneVector);
		const bool bClip = Actor.IsInMotionClip();
		if (!bClip)
		{
			Pawn.PelvisRest = PelvisWorld.GetRelativeTransform(BodyFrame);
			Pawn.bPelvisRest = true;
		}
		Pawn.PelvisFollow = FMath::FInterpConstantTo(Pawn.PelvisFollow, bClip && Pawn.bPelvisRest ? 1.0f : 0.0f, FrameDelta, 6.0f);
		const FVector Off = PelvisWorld.GetLocation() - BodyFrame.GetLocation();
		FTransform Flat = BodyFrame;
		if (Off.Size2D() < 120.0)
		{
			Flat.AddToTranslation(FVector(Off.X, Off.Y, 0.0));
		}
		BodyFrame = Flat;
		if (Pawn.PelvisFollow > 0.0f && Pawn.bPelvisRest)
		{
			const FTransform Follow = Pawn.PelvisRest.Inverse() * PelvisWorld;
			if (FVector::Dist(Follow.GetLocation(), Flat.GetLocation()) < 150.0)
			{
				const float A = MeleePose::EaseInOut(Pawn.PelvisFollow);
				BodyFrame.SetLocation(FMath::Lerp(Flat.GetLocation(), Follow.GetLocation(), static_cast<double>(A)));
				BodyFrame.SetRotation(FQuat::Slerp(Flat.GetRotation(), Follow.GetRotation(), A));
			}
		}
	}
	Pawn.BodyFrame = BodyFrame;

	colony::WeaponHold HandsHold;
	bool bHandsHold = false;
	FTransform HandsWeapon;
	float HandsSize = Gear.size;
	for (int32 Slot = 0; Slot < GWeaponDisplaySlots; ++Slot)
	{
		FDisplayed& Shown = Pawn.Slots[Slot];
		const colony::DefId Def = Wanted[Slot];
		if (!Def.valid())
		{
			DestroyDisplayed(Shown);
			continue;
		}
		const FString DefName = Defs ? FString(UTF8_TO_TCHAR(Defs->get(Def).defName().c_str())) : FString();
		const FGunRangeGun* Art = Range && IsGun(Def) && GWeaponGunArt.GetValueOnGameThread() != 0 ? Range->FindGunByDef(DefName) : nullptr;
		if (!Shown.Component.IsValid() || Shown.Def != Def || Shown.bArt != (Art != nullptr))
		{
			DestroyDisplayed(Shown);
			if (!MakeDisplayed(Actor, Shown, Def, Art))
			{
				continue;
			}
		}
		// How it is held: its hold style and grip points, or the gun model's own (assets/guns.json) for a real model.
		colony::WeaponHold Hold;
		const bool bHold = Defs && colony::holdFor(*Defs, Def, Hold);
		if (Art)
		{
			Hold.main = colony::HoldPoint{static_cast<float>(Art->Grip.X), static_cast<float>(Art->Grip.Y), static_cast<float>(Art->Grip.Z)};
			Hold.hasOff = Art->bOffHand;
			Hold.off = colony::HoldPoint{static_cast<float>(Art->OffHand.X), static_cast<float>(Art->OffHand.Y), static_cast<float>(Art->OffHand.Z)};
			Hold.rake = Art->Rake;
		}
		// The defs' guns share one shape, so a sidearm without a model of its own is drawn at half size.
		const float Size = Gear.size * (!Art && Slot == static_cast<int32>(colony::WeaponSlot::Sidearm) ? WeaponSidearmScale : 1.0f);

		// 0 holstered to 1 in the hands.
		float InHand = Slot == InHands ? 1.0f : 0.0f;
		if (Incoming >= 0)
		{
			if (Slot == InHands)
			{
				InHand = Progress < 0.5f ? 1.0f - MeleePose::EaseInOut(Progress / 0.5f) : 0.0f;
			}
			else if (Slot == Incoming)
			{
				InHand = Progress < 0.5f ? 0.0f : MeleePose::EaseInOut((Progress - 0.5f) / 0.5f);
			}
		}
		// A weapon with no holster of its own (the fourth slot) stays in the hands.
		if (Slot < colony::kWeaponSlotCount)
		{
			InHand *= ReadyEase;
		}

		FVector HolsterLocation;
		FRotator HolsterRotation;
		HolsterPose(Slot, Height, HolsterLocation, HolsterRotation);
		const FTransform Holster = FTransform(HolsterRotation, HolsterLocation * Place) * BodyFrame;

		// In the hands: on the hand's grip (HoldDraw), else floating where the old drawing put it.
		FTransform Frame;
		const bool bInHandFrame = bHands && bHold && HoldDraw::HandFrame(Mesh, Hold, Frame);
		FTransform Hands;
		if (bInHandFrame)
		{
			Hands = HoldDraw::WeaponWorld(Frame, Hold, Size);
		}
		else
		{
			FVector HandsLocation;
			FRotator HandsRotation;
			if (IsGun(Def))
			{
				HoldPose(Slot, Height, HandsLocation, HandsRotation);
				if (bControlled && Protagonist->GetBody())
				{
					HandsRotation.Pitch = Protagonist->GetBody()->GetPitch();
				}
			}
			else
			{
				Actor.GetMeleeWeaponPose(HandsLocation, HandsRotation);
			}
			FTransform ActorFrame = Actor.GetActorTransform();
			ActorFrame.SetScale3D(FVector::OneVector);
			Hands = FTransform(HandsRotation, HandsLocation * Place) * ActorFrame;
		}
		FTransform World;
		World.SetLocation(FMath::Lerp(Holster.GetLocation(), Hands.GetLocation(), static_cast<double>(InHand)));
		World.SetRotation(FQuat::Slerp(Holster.GetRotation(), Hands.GetRotation(), InHand));
		World.SetScale3D(FVector(Size));

		// Hung on the hand bone once fully in it (so it moves with the hand between our updates), else on the actor.
		UStaticMeshComponent* Component = Shown.Component.Get();
		USceneComponent* Parent = InHand >= 0.999f && bInHandFrame ? static_cast<USceneComponent*>(Mesh) : Actor.GetRootComponent();
		const FName Socket = Parent == Mesh ? HoldDraw::HandBone(Hold) : NAME_None;
		if (Component->GetAttachParent() != Parent || Component->GetAttachSocketName() != Socket)
		{
			Component->AttachToComponent(Parent, FAttachmentTransformRules::KeepWorldTransform, Socket);
		}
		Component->SetWorldTransform(World);
		Component->SetVisibility(bShow);
		for (const TWeakObjectPtr<UStaticMeshComponent>& Part : Shown.Parts)
		{
			if (Part.IsValid())
			{
				Part->SetVisibility(bShow);
			}
		}
		if (Slot == InHands && InHand >= 0.999f && bInHandFrame)
		{
			HandsHold = Hold;
			bHandsHold = bHold;
			HandsWeapon = World;
			HandsSize = Size;
		}
	}

	// The other hand on a two-handed grip, while the weapon is held up (not through a reload, which moves the hand itself).
	const bool bOff = bShow && bHandsHold && HandsHold.hasOff && !View.reloading && Height > 0.9f;
	Pawn.OffHand = FMath::FInterpConstantTo(Pawn.OffHand, bOff ? 1.0f : 0.0f, FrameDelta, 5.0f);
	if (Mesh && (Pawn.OffHand > 0.0f || Pawn.bOffHandSet))
	{
		HoldDraw::SetOffHand(Mesh, HandsWeapon, HandsHold, HandsSize, bHandsHold ? Pawn.OffHand : 0.0f);
		Pawn.bOffHandSet = Pawn.OffHand > 0.0f;
	}

	UpdateStance(Actor, View, Pawn, bShow && bHands, bControlled);
	UpdateShield(Actor, View, Pawn, bShow && !View.hideGear, Height, Gear);
}

// The upper body's stance for what is in the hands (docs/design/weapon-hold.md): the hold style's stance clip while
// ready, its aim clip while a gun is on a target, its block clip while blocking (the shield's with a shield on the arm),
// its reload while reloading; none while working, lying, swimming or carrying.
void UWeaponDrawSubsystem::UpdateStance(ASimPawnActor& Actor, const colony::ThingView& View, FPawnWeapons& Pawn, bool bShow, bool bControlled)
{
	USimPawnAnimInstance* Anim = Actor.GetAnim();
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!Anim || !Bridge || !Bridge->GetDefs())
	{
		return;
	}
	const colony::DefDatabase& Defs = *Bridge->GetDefs();
	UAnimSequence* Clip = nullptr;
	float Weight = 0.0f;
	colony::WeaponHold Hold;
	if (bShow && GWeaponStances.GetValueOnGameThread() != 0 && !Actor.IsJumping() && colony::holdFor(Defs, View.weapon, Hold))
	{
		const bool bShieldArm = HasShieldOnArm(View);
		const bool bGun = IsGun(View.weapon);
		bool bAiming = bGun && View.fightTarget != colony::kNoEntity;
		if (bControlled)
		{
			const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
			bAiming = bGun && Protagonist && Protagonist->GetBody() && Protagonist->GetBody()->GetAimBlend() > 0.3f;
		}
		if (View.reloading)
		{
			Clip = HoldDraw::StyleClip(Defs, Hold, "reload", "stance");
		}
		else if (View.blocking || View.shieldUp)
		{
			Clip = bShieldArm ? HoldDraw::StyleClip(Defs, Hold, "shieldBlock", "block") : HoldDraw::StyleClip(Defs, Hold, "block");
			if (!Clip && View.shieldUp)
			{
				colony::WeaponHold Shield;
				if (colony::shieldHoldFor(Defs, View.shield, Shield))
				{
					Clip = HoldDraw::StyleClip(Defs, Shield, "block");
				}
			}
		}
		else if (bAiming)
		{
			Clip = HoldDraw::StyleClip(Defs, Hold, "aim", "stance");
		}
		else
		{
			Clip = bShieldArm ? HoldDraw::StyleClip(Defs, Hold, "shieldStance", "stance") : HoldDraw::StyleClip(Defs, Hold, "stance");
		}
		// Fists show a guard only while blocking; the rest only while ready, held up fully once ready.
		Weight = Hold.hands == 0 && !(View.blocking || View.shieldUp) ? 0.0f : MeleePose::EaseInOut(Pawn.Ready);
		if (View.switchSlot < colony::kWeaponSlotCount)
		{
			Weight *= 0.5f;
		}
		if (Actor.GetStanceHeight() < 0.9f)
		{
			Weight *= 0.6f;  // crouched: the arms only
		}
	}
	Anim->SetUpperStance(Clip, Clip ? Weight : 0.0f);
	// A body on the back: bent forward under it (docs/design/weapon-hold.md, "Carrying a body").
	Anim->SetSpineBend(View.carryingPawn != colony::kNoEntity && !View.downed && !View.dead ? WeaponCarryBend : 0.0f);
}

void UWeaponDrawSubsystem::UpdateShield(ASimPawnActor& Actor, const colony::ThingView& View, FPawnWeapons& Pawn, bool bShow, float Height,
										const colony::GearSize& Gear)
{
	if (!View.shield.valid())
	{
		if (Pawn.Shield.IsValid())
		{
			Pawn.Shield->DestroyComponent();
		}
		Pawn.Shield = nullptr;
		Pawn.ShieldKey = 0;
		return;
	}
	const uint64 Key = (static_cast<uint64>(View.shield.value) << 32) | static_cast<uint64>(View.shieldStuff.value);
	if (!Pawn.Shield.IsValid() || Pawn.ShieldKey != Key)
	{
		if (Pawn.Shield.IsValid())
		{
			Pawn.Shield->DestroyComponent();
		}
		Pawn.Shield = nullptr;
		UStaticMesh* Mesh = ShieldMesh(View.shield, View.shieldStuff);
		if (!Mesh)
		{
			return;
		}
		Pawn.Shield = MakeComponent(Actor, Mesh);
		Pawn.ShieldKey = Key;
		// A new shield starts where it should be, not swinging in from the arm.
		Pawn.ShieldRaise = View.shieldUp ? 1.0f : 0.0f;
		Pawn.ShieldSlung = View.shieldSlung ? 1.0f : 0.0f;
	}
	Pawn.ShieldRaise = FMath::FInterpConstantTo(Pawn.ShieldRaise, View.shieldUp ? 1.0f : 0.0f, FrameDelta, WeaponShieldRaiseRate);
	Pawn.ShieldSlung = FMath::FInterpConstantTo(Pawn.ShieldSlung, View.shieldSlung ? 1.0f : 0.0f, FrameDelta, WeaponShieldSlingRate);

	const float Raise = MeleePose::EaseInOut(Pawn.ShieldRaise);
	const float Slung = MeleePose::EaseInOut(Pawn.ShieldSlung);
	// The old places, actor space: on the arm, raised in front, slung on the back. Placed for this body (its height,
	// then crouched or standing) and sized part of the way to it.
	const FVector Scale = FVector(1.0, 1.0, Height) * static_cast<double>(Gear.place);
	FTransform ActorFrame = Actor.GetActorTransform();
	ActorFrame.SetScale3D(FVector::OneVector);
	const FTransform Lowered = FTransform(WeaponShieldLoweredRotation, WeaponShieldLowered * Scale) * ActorFrame;
	const FTransform Raised = FTransform(WeaponShieldRaisedRotation, WeaponShieldRaised * Scale) * ActorFrame;
	// Slung, it rides the holsters' frame (UpdatePawn), so it stays on the back through a roll or a fall.
	const FTransform Back = FTransform(WeaponShieldSlungRotation, WeaponShieldSlungAt * Scale) * Pawn.BodyFrame;

	// On the forearm by its straps (docs/design/weapon-hold.md): the block clip lifts the arm and the shield with it.
	// Without the arm's bones (or the block clip) it floats where it did.
	USkeletalMeshComponent* Mesh = HoldDraw::VisibleMesh(Actor);
	const USimSubsystem* Bridge = BridgeSource.Get();
	colony::WeaponHold Hold;
	FTransform Frame;
	const bool bArm = GWeaponHands.GetValueOnGameThread() != 0 && Mesh && Height > 0.4f && !View.swimming && Bridge && Bridge->GetDefs() &&
		colony::shieldHoldFor(*Bridge->GetDefs(), View.shield, Hold) && HoldDraw::ShieldFrame(Mesh, Frame);
	const USimPawnAnimInstance* Anim = Actor.GetAnim();
	const bool bClipRaises = Anim && Anim->GetUpperStance() && Anim->GetUpperWeight() > 0.5f;
	FTransform OnArm = Lowered;
	if (bArm)
	{
		OnArm = HoldDraw::WeaponWorld(Frame, Hold, Gear.size);
	}
	const float FloatRaise = bArm && bClipRaises ? 0.0f : Raise;
	FTransform World;
	World.SetLocation(FMath::Lerp(OnArm.GetLocation(), Raised.GetLocation(), static_cast<double>(FloatRaise)));
	World.SetRotation(FQuat::Slerp(OnArm.GetRotation(), Raised.GetRotation(), FloatRaise));
	World.SetLocation(FMath::Lerp(World.GetLocation(), Back.GetLocation(), static_cast<double>(Slung)));
	World.SetRotation(FQuat::Slerp(World.GetRotation(), Back.GetRotation(), Slung));
	World.SetScale3D(FVector(Gear.size));
	UStaticMeshComponent* Shield = Pawn.Shield.Get();
	USceneComponent* Parent = bArm && Slung <= 0.001f && FloatRaise <= 0.001f ? static_cast<USceneComponent*>(Mesh) : Actor.GetRootComponent();
	const FName Socket = Parent == Mesh ? HoldDraw::HandBone(Hold) : NAME_None;
	if (Shield->GetAttachParent() != Parent || Shield->GetAttachSocketName() != Socket)
	{
		Shield->AttachToComponent(Parent, FAttachmentTransformRules::KeepWorldTransform, Socket);
	}
	Shield->SetWorldTransform(World);
	Shield->SetVisibility(bShow);
}

// ---------------------------------------------------------------------------
// What is on screen, for the HUD and the tests.

UStaticMeshComponent* UWeaponDrawSubsystem::GetSlotComponent(colony::Entity Pawn, int32 Slot) const
{
	const FPawnWeapons* Found = Weapons.Find(entt::to_integral(Pawn));
	if (!Found || Slot < 0 || Slot >= GWeaponDisplaySlots)
	{
		return nullptr;
	}
	return Found->Slots[Slot].Component.IsValid() ? Found->Slots[Slot].Component.Get() : nullptr;
}

UStaticMeshComponent* UWeaponDrawSubsystem::GetHandsComponent(colony::Entity Pawn) const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	const colony::ThingView* View = Bridge ? Bridge->GetCurrentSnapshot().find(Pawn) : nullptr;
	if (!View || !View->weapon.valid())
	{
		return nullptr;
	}
	for (int32 Slot = 0; Slot < GWeaponDisplaySlots; ++Slot)
	{
		if (UStaticMeshComponent* Component = GetSlotComponent(Pawn, Slot))
		{
			const FPawnWeapons* Found = Weapons.Find(entt::to_integral(Pawn));
			if (Found && Found->Slots[Slot].Def == View->weapon)
			{
				return Component;
			}
		}
	}
	return nullptr;
}

UStaticMeshComponent* UWeaponDrawSubsystem::GetShieldComponent(colony::Entity Pawn) const
{
	const FPawnWeapons* Found = Weapons.Find(entt::to_integral(Pawn));
	return Found && Found->Shield.IsValid() ? Found->Shield.Get() : nullptr;
}

float UWeaponDrawSubsystem::GetShieldRaise(colony::Entity Pawn) const
{
	const FPawnWeapons* Found = Weapons.Find(entt::to_integral(Pawn));
	return Found ? Found->ShieldRaise : 0.0f;
}

int32 UWeaponDrawSubsystem::CountWeaponsInHands() const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	int32 Count = 0;
	if (!Bridge)
	{
		return 0;
	}
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		if (View.category == colony::ThingCategory::Pawn)
		{
			const UStaticMeshComponent* Component = GetHandsComponent(View.id);
			Count += Component && Component->IsVisible() ? 1 : 0;
		}
	}
	return Count;
}

int32 UWeaponDrawSubsystem::CountVisibleWeapons(colony::Entity Pawn) const
{
	int32 Count = 0;
	for (int32 Slot = 0; Slot < GWeaponDisplaySlots; ++Slot)
	{
		const UStaticMeshComponent* Component = GetSlotComponent(Pawn, Slot);
		Count += Component && Component->IsVisible() ? 1 : 0;
	}
	return Count;
}

FString UWeaponDrawSubsystem::DescribeCarried(const colony::ThingView& View) const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetDefs())
	{
		return FString();
	}
	int32 Carried = 0;
	FString Line;
	for (int32 Slot = 0; Slot < colony::kWeaponSlotCount; ++Slot)
	{
		if (!View.carried[Slot].valid())
		{
			continue;
		}
		++Carried;
		const FString Label = UTF8_TO_TCHAR(Bridge->GetDefs()->get(View.carried[Slot]).label().c_str());
		const bool bHeld = View.weapon.valid() && View.weapon == View.carried[Slot];
		Line += bHeld ? FString::Printf(TEXT("  [%s]"), *Label) : FString::Printf(TEXT("  %s"), *Label);
	}
	if (Carried < 2)
	{
		return FString();
	}
	return FString(TEXT("Q switch:")) + Line;
}

// ---------------------------------------------------------------------------
// The player's keys.

const colony::ThingView* UWeaponDrawSubsystem::ControlledView(USimSubsystem*& OutBridge) const
{
	OutBridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!OutBridge || !OutBridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || OutBridge->GetSpeed() == 0)
	{
		return nullptr;
	}
	const colony::ThingView* View = OutBridge->GetCurrentSnapshot().find(Protagonist->GetControlled());
	return View && !View->downed && !View->dead ? View : nullptr;
}

colony::Entity UWeaponDrawSubsystem::FindSwingTarget() const
{
	USimSubsystem* Bridge = nullptr;
	const colony::ThingView* Me = ControlledView(Bridge);
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Me || !Protagonist || !Protagonist->GetBody())
	{
		return colony::kNoEntity;
	}
	const FVector Look = Protagonist->GetBody()->GetViewDirection().GetSafeNormal2D();
	const colony::MeleeSystem& Melee = Bridge->GetSim()->melee();
	colony::Entity Best = colony::kNoEntity;
	float BestScore = WeaponSwingCone;
	for (const colony::ThingView& Other : Bridge->GetCurrentSnapshot().things)
	{
		if (Other.category != colony::ThingCategory::Pawn || Other.id == Me->id || Other.dead || Other.faction == Me->faction || !Melee.inReach(Me->id, Other.id))
		{
			continue;
		}
		const FVector To = FVector(Other.x - Me->x, Other.y - Me->y, 0.0).GetSafeNormal();
		const float Cosine = static_cast<float>(FVector::DotProduct(To, Look));
		if (Cosine > BestScore)
		{
			BestScore = Cosine;
			Best = Other.id;
		}
	}
	return Best;
}

bool UWeaponDrawSubsystem::Swing()
{
	USimSubsystem* Bridge = nullptr;
	const colony::ThingView* Me = ControlledView(Bridge);
	if (!Me || Bridge->GetGameSeconds() <= LastOrderStep)
	{
		return false;
	}
	if (Me->meleePhase != colony::MeleePhase::Ready || Me->switchSlot < colony::kWeaponSlotCount || Me->reloading)
	{
		return false;
	}
	// A gun in the hands shoots (UFightSubsystem); everything else swings.
	if (IsGun(Me->weapon))
	{
		return false;
	}
	Bridge->Submit(colony::MeleeCommand{Me->id, FindSwingTarget(), colony::MeleeMode::Attack});
	LastOrderStep = Bridge->GetGameSeconds();
	++SwingOrders;
	return true;
}

bool UWeaponDrawSubsystem::Block()
{
	USimSubsystem* Bridge = nullptr;
	const colony::ThingView* Me = ControlledView(Bridge);
	if (!Me || Bridge->GetGameSeconds() <= LastOrderStep || !Me->weapon.valid() || IsGun(Me->weapon))
	{
		return false;
	}
	if (Me->blocking || Me->meleePhase != colony::MeleePhase::Ready || Me->switchSlot < colony::kWeaponSlotCount)
	{
		return false;
	}
	Bridge->Submit(colony::MeleeCommand{Me->id, colony::kNoEntity, colony::MeleeMode::Block});
	LastOrderStep = Bridge->GetGameSeconds();
	++BlockOrders;
	return true;
}

bool UWeaponDrawSubsystem::HasShieldOnArm(const colony::ThingView& View)
{
	return View.shield.valid() && !View.shieldSlung;
}

bool UWeaponDrawSubsystem::HoldShield(bool bUp)
{
	USimSubsystem* Bridge = nullptr;
	const colony::ThingView* Me = ControlledView(Bridge);
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const double Now = GetWorld()->GetRealTimeSeconds();
	if (!bUp)
	{
		if (!bShieldWanted)
		{
			return false;
		}
		bShieldWanted = false;
		USimSubsystem* Sim = BridgeSource.Get();
		if (!Sim || !Sim->GetSim() || ShieldPawn == colony::kNoEntity)
		{
			return false;
		}
		Sim->Submit(colony::ShieldCommand{ShieldPawn, false, false, 0.0f, 0.0f});
		ShieldPawn = colony::kNoEntity;
		ShieldSentAt = Now;
		++ShieldOrders;
		return true;
	}
	if (!Me || !HasShieldOnArm(*Me) || !Protagonist || !Protagonist->GetBody())
	{
		return false;
	}
	const FVector Look = Protagonist->GetBody()->GetViewDirection().GetSafeNormal2D();
	const FVector2D Dir = Look.IsNearlyZero() ? FVector2D(Me->facingX, Me->facingY) : FVector2D(Look.X, Look.Y);
	// Sent when the wish changes, when the camera has turned, or now and then if the sim has not raised it (it was busy).
	const bool bChanged = !bShieldWanted || ShieldPawn != Me->id;
	const bool bTurned = FVector2D::DotProduct(Dir, ShieldDirSent) < WeaponShieldResendCosine;
	const bool bStale = !Me->shieldUp && Now - ShieldSentAt > 0.5;
	if (!bChanged && !bStale && !(bTurned && Now - ShieldSentAt >= WeaponShieldResendSeconds))
	{
		return false;
	}
	Bridge->Submit(colony::ShieldCommand{Me->id, true, true, static_cast<float>(Dir.X), static_cast<float>(Dir.Y)});
	bShieldWanted = true;
	ShieldPawn = Me->id;
	ShieldDirSent = Dir;
	ShieldSentAt = Now;
	++ShieldOrders;
	return true;
}

bool UWeaponDrawSubsystem::SwitchTo(int32 Slot)
{
	USimSubsystem* Bridge = nullptr;
	const colony::ThingView* Me = ControlledView(Bridge);
	if (!Me || Slot < 0 || Slot >= colony::kWeaponSlotCount || !Me->carried[Slot].valid() || Bridge->GetGameSeconds() <= LastOrderStep)
	{
		return false;
	}
	if (Me->switchSlot < colony::kWeaponSlotCount || Me->meleePhase != colony::MeleePhase::Ready || Me->reloading || (Me->weapon.valid() && Me->weapon == Me->carried[Slot]))
	{
		return false;
	}
	Bridge->Submit(colony::SwitchWeaponCommand{Me->id, static_cast<colony::WeaponSlot>(Slot)});
	LastOrderStep = Bridge->GetGameSeconds();
	++SwitchOrders;
	return true;
}

bool UWeaponDrawSubsystem::SwitchNext()
{
	USimSubsystem* Bridge = nullptr;
	const colony::ThingView* Me = ControlledView(Bridge);
	if (!Me)
	{
		return false;
	}
	int32 Current = -1;
	for (int32 Slot = 0; Slot < colony::kWeaponSlotCount; ++Slot)
	{
		if (Me->weapon.valid() && Me->carried[Slot] == Me->weapon)
		{
			Current = Slot;
		}
	}
	// The next carried weapon after the one in the hands: rifle, sidearm, melee weapon, rifle...
	for (int32 Step = 1; Step <= colony::kWeaponSlotCount; ++Step)
	{
		const int32 Slot = (Current + Step + colony::kWeaponSlotCount) % colony::kWeaponSlotCount;
		if (Me->carried[Slot].valid() && !(Me->weapon.valid() && Me->carried[Slot] == Me->weapon))
		{
			return SwitchTo(Slot);
		}
	}
	return false;
}

void UWeaponDrawSubsystem::UpdatePlayerInput()
{
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (!Protagonist || !Protagonist->IsDirectMode() || !PC)
	{
		// Leaving direct mode with the shield held: put it down, so the colonist doesn't carry on behind it.
		HoldShield(false);
		return;
	}
	// With a building in hand the mouse buttons plan it and cancel it (UHandsOnSubsystem).
	const UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>();
	if (Orders && Orders->IsDirectBuilding())
	{
		HoldShield(false);
		return;
	}
	if (PC->IsInputKeyDown(EKeys::LeftMouseButton))
	{
		Swing();
	}
	// The right mouse button holds up the shield when one is on the arm, else the weapon (a gun aims, ProtagonistPawn).
	USimSubsystem* Bridge = nullptr;
	const colony::ThingView* Me = ControlledView(Bridge);
	const bool bRight = bScriptedRight ? bScriptRightHeld : PC->IsInputKeyDown(EKeys::RightMouseButton);
	if (!Me)
	{
		// Paused, downed or dead: leave the shield as it is (the sim lowers it for a downed pawn).
	}
	else if (HasShieldOnArm(*Me))
	{
		if (ShieldPawn != colony::kNoEntity && ShieldPawn != Me->id)
		{
			HoldShield(false); // the player took over another colonist with the old one's shield up
		}
		HoldShield(bRight);
	}
	else
	{
		HoldShield(false);
		if (bRight)
		{
			Block();
		}
	}
	if (PC->WasInputKeyJustPressed(EKeys::Q))
	{
		SwitchNext();
	}
}
