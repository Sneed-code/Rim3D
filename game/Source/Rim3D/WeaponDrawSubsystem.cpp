#include "WeaponDrawSubsystem.h"
#include "GunRangeSubsystem.h"
#include "KeyBindings.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"

#include "FightSubsystem.h"
#include "GunRangeSubsystem.h"
#include "HoldDraw.h"
#include "MakeHumanAnimInstance.h"
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
	TAutoConsoleVariable<int32> GWeaponPlaces(
		TEXT("Weapons.Places"), 1, TEXT("1 holds weapons at their style's place (a gun's sight before the eye, its stock in the shoulder, a blade's guard) with both hands on the grips (docs/design/weapon-hold.md), 0 lets the weapon follow the clip's hand."), ECVF_Default);
	TAutoConsoleVariable<int32> GWeaponGunArt(
		TEXT("Weapons.GunModels"), 1, TEXT("1 draws guns with a real model (assets/guns.json) in third person too, 0 the defs' plain shapes."), ECVF_Default);

	// How fast a weapon comes out of its holster or goes back (per real second: about 0.4 s), and how far a body on the
	// back bends its carrier forward (degrees).
	constexpr float WeaponReadyRate = 2.5f;
	constexpr float WeaponCarryBend = 26.0f;

	const TCHAR* const WeaponSolidMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");

	// A sidearm is drawn at half the rifle-sized gun the defs share.
	constexpr float WeaponSidearmScale = 0.55f;
	// A held weapon whose grip is drawn further than this from its hand (cm, for a 1.80 m man) for this long (real
	// seconds) is floating: it goes back on the hand's own grip for WeaponForceHandSeconds.
	constexpr double WeaponFloatCm = 22.0;
	constexpr float WeaponFloatSeconds = 0.1f;
	constexpr float WeaponForceHandSeconds = 0.6f;
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
		else if (float ToHit = 0.0f, Rate = 1.0f; Attacker->GetMotionClipToHit(ToHit, Rate))
		{
			// An attack clip (body-motion.md): the sim times it so its blow falls on this step. What is left of the clip
			// before its blow, at the rate it plays, is how far behind the picture is.
			const float Behind = FMath::Max(0.0f, ToHit) / Rate * colony::kStepsPerRealSecondAt1x;
			MaxStepsBehind = FMath::Max(MaxStepsBehind, Behind);
			(Behind <= 2.5f ? HitsOnTime : HitsLate) += 1;
		}
		else
		{
			HitsUntimed += 1;  // the body shows no swing to time (pushed into a stagger mid-swing, say)
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
	else if ((Event.hit == colony::ProjectileHitKind::Building || Event.hit == colony::ProjectileHitKind::Plant) && Fight)
	{
		Fight->SpawnMeleeImpact(Event.hit, Event.thing, Event.target, At, Direction, -1, Event.amount);
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

FString UWeaponDrawSubsystem::DescribeMaterials(const UStaticMesh* Mesh)
{
	if (!Mesh)
	{
		return TEXT("no mesh");
	}
	TArray<FString> Slots;
	const TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
	for (int32 Index = 0; Index < Materials.Num(); ++Index)
	{
		const UMaterialInterface* Material = Materials[Index].MaterialInterface;
		if (!Material)
		{
			Slots.Add(FString::Printf(TEXT("[%d] none"), Index));
			continue;
		}
		const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material);
		UTexture* Colour = nullptr;
		FLinearColor Factor = FLinearColor(-1.0f, -1.0f, -1.0f, -1.0f);
		float Metallic = -1.0f;
		float Roughness = -1.0f;
		Material->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColorTexture")), Colour);
		Material->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColorFactor")), Factor);
		Material->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("MetallicFactor")), Metallic);
		Material->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("RoughnessFactor")), Roughness);
		const UMaterial* Base = Material->GetMaterial();
		Slots.Add(FString::Printf(TEXT("[%d] %s (parent %s, base %s%s) colour map %s, colour %s, metallic %.2f, roughness %.2f"), Index, *Material->GetName(),
			Instance && Instance->Parent ? *Instance->Parent->GetName() : TEXT("none"), Base ? *Base->GetName() : TEXT("none"),
			Base && Base->IsTwoSided() ? TEXT(", two-sided") : TEXT(""), Colour ? *Colour->GetName() : TEXT("none"), *Factor.ToString(), Metallic, Roughness));
	}
	return FString::Join(Slots, TEXT("; "));
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
	// The real model for this material (visual.models, made by the ShieldArt commandlet), when it has been made.
	if (Stuff.valid() && Stuff.value < Defs.size())
	{
		const std::string ModelPath = Defs.get(Shield).text(("visual.models." + Defs.get(Stuff).defName()).c_str());
		if (!ModelPath.empty())
		{
			if (UStaticMesh* Model = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(ModelPath.c_str()), nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				ShieldMeshes.Add(Key, Model);
				UE_LOG(LogWeaponDraw, Display, TEXT("Shield model %s: %s"), *Model->GetName(), *DescribeMaterials(Model));
				return Model;
			}
			UE_LOG(LogWeaponDraw, Warning, TEXT("Shield model %s is not made (run the ShieldArt commandlet); drawing plain shapes"), UTF8_TO_TCHAR(ModelPath.c_str()));
		}
	}
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
	// The material's colour; plasteel has none in its def.
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
	// A melee weapon with a real model (assets/guns.json "melee" art, GunArt commandlet): that, in the def's frame.
	if (!IsGun(Weapon) && GWeaponGunArt.GetValueOnGameThread() != 0)
	{
		if (UGunRangeSubsystem* Range = GetWorld()->GetSubsystem<UGunRangeSubsystem>())
		{
			if (UStaticMesh* Real = Range->FindMeleeMesh(UTF8_TO_TCHAR(Bridge->GetDefs()->get(Weapon).defName().c_str())))
			{
				Meshes.Add(Weapon.value, Real);
				return Real;
			}
		}
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
	// A thin held plate or blade in the distance-field scene shades itself black under Lumen; it moves too fast to occlude anything anyway.
	Weapon->bAffectDistanceFieldLighting = false;
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
	// Crouched or prone (C, Z) the weapon stays in the hand as the body's clip has it: it is only held at a place with
	// the hands pinned to it standing up. (It used to go to an old lowered spot in the air, sneed 2026-11-20: "I press Z
	// and the weapons float".)
	const bool bStanding = Height > 0.9f;
	const bool bHands = GWeaponHands.GetValueOnGameThread() != 0 && Mesh && !View.swimming && !View.felling;  // felling: the chop clip has the hands
	Pawn.ForceHandSeconds = FMath::Max(0.0f, Pawn.ForceHandSeconds - FrameDelta);

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
	bool bAnyPinned = false;
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
		// Felling a tree: the weapon goes to its holster while the hands hold the axe (UWorkToolSubsystem).
		if (View.felling)
		{
			InHand = 0.0f;
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

		// In the hands: held at its place with both hands on its grips (weapon first, then the hands), else on the
		// hand's grip as the clip has it (HoldDraw), else floating where the old drawing put it.
		const bool bPinSlot = Incoming < 0 ? Slot == InHands : Slot == (Progress < 0.5f ? InHands : Incoming);
		bool bPinned = false;
		FTransform WeaponInHand;
		FTransform WeaponPlaced;
		// A finisher holds the weapon at its place whatever the stance clip or a recent float fix says: its own path
		// puts the weapon on the body with both hands on it.
		const bool bFinisherNow = View.meleeFinisher != 0 && !IsGun(Def);
		if (bControlled && bPinSlot)
		{
			PinFailReason = TEXT("");
			PinDebug = FString::Printf(TEXT("show %d hands %d standing %d hold %d inhand %.2f places %d force %.2f finisher %d"), bShow ? 1 : 0, bHands ? 1 : 0,
				bStanding ? 1 : 0, bHold ? 1 : 0, InHand, GWeaponPlaces.GetValueOnGameThread(), Pawn.ForceHandSeconds, static_cast<int32>(View.meleeFinisher));
		}
		if (bShow && bHands && (bStanding || bFinisherNow) && bHold && bPinSlot && InHand > 0.0f && GWeaponPlaces.GetValueOnGameThread() != 0 &&
			(Pawn.ForceHandSeconds <= 0.0f || bFinisherNow))
		{
			HoldDraw::FPoints Points = HoldDraw::PointsOf(Hold, HasShieldOnArm(View));
			const UStaticMeshComponent* Body = Shown.Component.Get();
			const FBox Bounds = Body && Body->GetStaticMesh() ? Body->GetStaticMesh()->GetBoundingBox() : FBox(ForceInit);
			if (Bounds.IsValid)
			{
				Points.Tip = FVector(Bounds.Max.X, Bounds.GetCenter().Y, Bounds.GetCenter().Z);  // the far end along the blade
			}
			if (Art)
			{
				Points.Sight = Art->Sight;
				Points.Stock = HoldDraw::StockFromBounds(Bounds, Art->Sight);
			}
			bPinned = PinWeapon(Actor, View, Pawn, Hold, Points, Size, static_cast<float>(Place), bControlled, InHand, WeaponInHand, WeaponPlaced);
			bAnyPinned = bAnyPinned || bPinned;
			if (bControlled)
			{
				PinDebug += FString::Printf(TEXT(", pinned %d%s%s, aim %.2f, hand free %.2f, finisher blend %.2f"), bPinned ? 1 : 0, PinFailReason[0] != 0 ? TEXT(" because ") : TEXT(""),
					PinFailReason, Pawn.AimBlend, Pawn.HandFree, Pawn.FinisherBlend);
			}
		}
		FTransform Frame;
		const bool bInHandFrame = bHands && bHold && (bPinned || HoldDraw::HandFrame(Mesh, Hold, Frame));
		FTransform Hands;
		if (bPinned)
		{
			FTransform HandWorld = Mesh->GetSocketTransform(HoldDraw::HandBone(Hold), RTS_World);
			HandWorld.SetScale3D(FVector::OneVector);
			// At its place while the hands hold it there (it never turns with a hand the wrist limits or the reach held
			// back); in the hand while a swing or a full-body clip moves the hand.
			FTransform InHandWorld = WeaponInHand * HandWorld;
			InHandWorld.SetScale3D(FVector::OneVector);
			const float Free = MeleePose::EaseInOut(FMath::Clamp(Pawn.HandFree, 0.0f, 1.0f));
			Hands = FTransform(FQuat::Slerp(WeaponPlaced.GetRotation(), InHandWorld.GetRotation(), Free),
				FMath::Lerp(WeaponPlaced.GetLocation(), InHandWorld.GetLocation(), static_cast<double>(Free)));
		}
		else if (bInHandFrame)
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

		// Never floating (sneed 2026-11-20: "the weapon floats but the hand animations still work", "swap weapons, go
		// unarmed and back and press Z and the weapons float"): a weapon fully in the hands whose grip has been drawn away
		// from its hand for a moment (a hand the arm solver couldn't bring to it, a place the body can't reach, a swing
		// path off the arm) goes back onto the hand's own grip at once and stays there a little while.
		if (bShow && bHands && bHold && Slot == InHands && Incoming < 0 && InHand >= 0.999f && Mesh->GetBoneIndex(HoldDraw::HandBone(Hold)) != INDEX_NONE)
		{
			const FVector Grip = World.TransformPosition(FVector(Hold.main.x, Hold.main.y, Hold.main.z));
			const double Gap = FVector::Dist(Grip, Mesh->GetSocketLocation(HoldDraw::HandBone(Hold)));
			WorstGripGap = FMath::Max(WorstGripGap, static_cast<float>(Gap));
			const bool bAway = Gap > WeaponFloatCm * Place;
			Pawn.FloatSeconds = bAway ? Pawn.FloatSeconds + FrameDelta : 0.0f;
			if (bAway)
			{
				++FloatFrames;
			}
			FTransform Frame2;
			if (Pawn.FloatSeconds >= WeaponFloatSeconds && HoldDraw::HandFrame(Mesh, Hold, Frame2))
			{
				if (!Pawn.bFloatLogged)
				{
					Pawn.bFloatLogged = true;
					UE_LOG(LogTemp, Warning, TEXT("WEAPON FLOAT %s: grip %.0f cm off the hand (pinned %d, hand free %.2f, swing %.2f, clip %d, melee %d, finisher %d, height %.2f); put back in the hand"),
						*DefName, Gap, bPinned ? 1 : 0, Pawn.HandFree, Pawn.SwingBlend, Actor.IsInMotionClip() ? 1 : 0, static_cast<int32>(View.meleePhase),
						static_cast<int32>(View.meleeFinisher), Height);
				}
				World = HoldDraw::WeaponWorld(Frame2, Hold, Size);
				Pawn.ForceHandSeconds = WeaponForceHandSeconds;
				Pawn.FloatSeconds = 0.0f;
				++FloatFixes;
				if (bPinned)
				{
					HoldDraw::ReleaseHands(Mesh);
					bAnyPinned = false;
					Pawn.bOffHandSet = false;
				}
			}
		}

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

	// Hands let go once when the weapon is no longer held at a place (holstered, crouched, lying, downed).
	if (!bAnyPinned && Pawn.bPinned)
	{
		HoldDraw::ReleaseHands(Mesh);
		Pawn.bOffHandSet = false;
	}
	Pawn.bPinned = bAnyPinned;
	// The other hand on a two-handed grip, while the weapon is held up (not through a reload, which moves the hand
	// itself), when it isn't held at a place (that puts both hands on already).
	const bool bOff = !bAnyPinned && bShow && bHandsHold && HandsHold.hasOff && !View.reloading && Height > 0.9f;
	Pawn.OffHand = FMath::FInterpConstantTo(Pawn.OffHand, bOff ? 1.0f : 0.0f, FrameDelta, 5.0f);
	if (Mesh && !bAnyPinned && (Pawn.OffHand > 0.0f || Pawn.bOffHandSet))
	{
		HoldDraw::SetOffHand(Mesh, HandsWeapon, HandsHold, HandsSize, bHandsHold ? Pawn.OffHand : 0.0f);
		Pawn.bOffHandSet = Pawn.OffHand > 0.0f;
	}

	UpdateStance(Actor, View, Pawn, bShow && bHands && bStanding, bControlled);
	UpdateShield(Actor, View, Pawn, bShow && !View.hideGear, Height, Gear);
}

// Weapon first, then the hands (docs/design/weapon-hold.md): the weapon goes to its style's place on the body for what
// the pawn is doing (ready, aimed, blocking), blended as it changes, in the body's facing from between the shoulders or
// from the head (aimed: turned by the aim's pitch); the hands' goals ride on the neck bone so the arm solver reads them
// against this very pose. Both hands then close on its grips through the solver, within human joint limits. Through a melee
// swing or a full-body clip (a roll) the main hand follows its clip instead, the weapon still in it and the other hand
// still on its grip.
bool UWeaponDrawSubsystem::PinWeapon(ASimPawnActor& Actor, const colony::ThingView& View, FPawnWeapons& Pawn, const colony::WeaponHold& Hold,
									 const HoldDraw::FPoints& Points, float Size, float BodyPlace, bool bControlled, float Alpha, FTransform& OutWeaponInHand, FTransform& OutWeaponWorld)
{
	USkeletalMeshComponent* Mesh = HoldDraw::VisibleMesh(Actor);
	if (!Mesh || !Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) || !HoldDraw::HasHand(Mesh, true) || !HoldDraw::HasHand(Mesh, false) ||
		Mesh->GetBoneIndex(TEXT("neck_01")) == INDEX_NONE || Mesh->GetBoneIndex(TEXT("head")) == INDEX_NONE ||
		Mesh->GetBoneIndex(TEXT("upperarm_l")) == INDEX_NONE || Mesh->GetBoneIndex(TEXT("upperarm_r")) == INDEX_NONE)
	{
		PinFailReason = !Mesh ? TEXT("no mesh") : !Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) ? TEXT("not a MakeHuman body") : TEXT("missing hand/arm bones");
		return false;
	}
	const colony::HoldPlace* Ready = colony::holdPlaceFor(Hold, false, false, false);
	if (!Ready || Hold.grip == colony::HoldGrip::None || Hold.grip == colony::HoldGrip::Shield || Hold.grip == colony::HoldGrip::Bow)
	{
		PinFailReason = TEXT("no ready place for this grip");
		return false;
	}
	const colony::HoldPlace* Aim = colony::holdPlaceFor(Hold, false, true, false);
	const colony::HoldPlace* Block = colony::holdPlaceFor(Hold, false, false, true);
	const bool bGun = Hold.grip == colony::HoldGrip::Gun;
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const AProtagonistPawn* Body = bControlled && Protagonist ? Protagonist->GetBody() : nullptr;

	// Aimed (guns) and blocking (melee), eased.
	float AimWant = 0.0f;
	float AimPitch = 0.0f;
	if (bGun && Body)
	{
		// Shooting from the hip raises the gun to the eye too (the cooldown starts at the first shot), so the arms
		// always follow the shots instead of the gun staying at the ready.
		AimWant = FMath::Max(Body->GetAimBlend(), View.refillLeft > 0 ? 1.0f : 0.0f);
		AimPitch = Body->GetPitch();
	}
	else if (bGun && View.fightTarget != colony::kNoEntity)
	{
		AimWant = 1.0f;
		const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
		if (const ASimPawnActor* Target = Pawns ? Pawns->FindPawn(View.fightTarget) : nullptr)
		{
			// At the target's chest, from the eye.
			const FVector From = Mesh->GetSocketLocation(TEXT("head"));
			const FVector To = Target->GetActorLocation() + FVector(0.0, 0.0, 40.0 * Target->GetBodyScale());
			const FVector D = To - From;
			AimPitch = FMath::Clamp(static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(D.Z, D.Size2D()))), -60.0f, 60.0f);
		}
	}
	Pawn.AimBlend = Body && AimWant <= Body->GetAimBlend() ? AimWant : FMath::FInterpConstantTo(Pawn.AimBlend, AimWant, FrameDelta, Body ? 8.0f : 4.0f);
	Pawn.BlockBlend = FMath::FInterpConstantTo(Pawn.BlockBlend, !bGun && (View.blocking || View.shieldUp) ? 1.0f : 0.0f, FrameDelta, 6.0f);
	// A swing whose clip moves the arm frees the main hand (the weapon stays in it). A swing with no clip on the body
	// (no measured clip for it, or a body that can't play one) moves the weapon itself along the procedural swing
	// path, with both hands kept on it, so a weapon always swings and never hangs still in the air.
	// A finisher (docs/design/lock-on.md "Finishers") drives the weapon along its own path with both hands kept on it.
	FTransform FinisherDrawn;
	FTransform FinisherPinned;
	float FinisherWeight = 0.0f;
	const bool bFinisher = !bGun && FinisherPose(Actor, View, Points, Size, FinisherDrawn, FinisherPinned, FinisherWeight);
	Pawn.FinisherBlend = bFinisher ? FinisherWeight : 0.0f;
	Pawn.FinisherShort = bFinisher ? FinisherShort : 0.0f;
	// Short of the body: bend further over it until the blade reaches, eased so it never snaps; only while the spot is
	// still ahead of the shoulder (over it already, more bend only folds the body down into the one on the ground: the
	// club and knife in round 11, 2026-11-21), and back up again once it is not.
	{
		const bool bBendHelps = bFinisher && Pawn.FinisherShort > 0.0f && FinisherAhead > 15.0f;
		const float BendGoal = !bFinisher ? 0.0f
			: bBendHelps ? FMath::Clamp(Pawn.FinisherReachBend + Pawn.FinisherShort * 0.5f, 0.0f, 30.0f)
			: FinisherAhead < 5.0f ? FMath::Max(0.0f, Pawn.FinisherReachBend - 10.0f) : Pawn.FinisherReachBend;
		Pawn.FinisherReachBend = FMath::FInterpTo(Pawn.FinisherReachBend, BendGoal, FrameDelta, 4.0f);
	}
	const bool bClipSwing = !bFinisher && !bGun && Actor.IsInMotionClip() && (View.meleePhase != colony::MeleePhase::Ready || Actor.GetSwing().IsActive());
	const bool bWeaponSwing = !bFinisher && !bGun && !Actor.IsInMotionClip() && Actor.GetSwing().IsActive();
	const bool bFullBodyClip = Actor.IsInMotionClip() && !Actor.IsMotionClipUpper();
	Pawn.HandFree = FMath::FInterpConstantTo(Pawn.HandFree, bClipSwing || bFullBodyClip ? 1.0f : 0.0f, FrameDelta, 8.0f);
	Pawn.SwingBlend = FMath::FInterpConstantTo(Pawn.SwingBlend, bWeaponSwing ? 1.0f : 0.0f, FrameDelta, 10.0f);

	// The anchors in the world from this frame's pose: between the shoulder joints, or the head; the body's facing.
	const FVector Chest = (Mesh->GetSocketLocation(TEXT("upperarm_l")) + Mesh->GetSocketLocation(TEXT("upperarm_r"))) * 0.5;
	const FVector Head = Mesh->GetSocketLocation(TEXT("head"));
	const FQuat Facing = FRotationMatrix::MakeFromXZ(Actor.GetActorForwardVector(), Actor.GetActorUpVector()).ToQuat();
	// Third person, aiming (sneed 2026-11-20: "the weapon doesn't move with my cursor to aim, and arms"): an aimed place on
	// the head is turned to look straight at what the crosshair is on (yaw and pitch, at most 60 degrees round from
	// the body, which turns after the view), so the gun's sight line and both arms follow the cursor.
	FVector CrosshairPoint = FVector::ZeroVector;
	bool bCrosshair = false;
	if (bGun && Body && !Body->IsFirstPerson() && Pawn.AimBlend > 0.01f)
	{
		if (const UFightSubsystem* Fight = GetWorld()->GetSubsystem<UFightSubsystem>())
		{
			bCrosshair = Fight->ComputeAimPoint(CrosshairPoint) && FVector::DistSquared(CrosshairPoint, Head) > FMath::Square(60.0);
		}
	}
	const auto AimAt = [&](const FVector& From)
	{
		FVector Dir = (CrosshairPoint - From).GetSafeNormal();
		const FVector Forward = Actor.GetActorForwardVector().GetSafeNormal2D();
		const FVector Flat = Dir.GetSafeNormal2D();
		const double Yaw = FMath::Atan2(FVector::CrossProduct(Forward, Flat).Z, FVector::DotProduct(Forward, Flat));
		const double Most = FMath::DegreesToRadians(60.0);
		if (FMath::Abs(Yaw) > Most)
		{
			const FVector Turned = Forward.RotateAngleAxisRad(FMath::Sign(Yaw) * Most, FVector::UpVector);
			Dir = (Turned * Dir.Size2D() + FVector(0.0, 0.0, Dir.Z)).GetSafeNormal();
		}
		Dir.Z = FMath::Clamp(Dir.Z, -0.94, 0.94);
		return FRotationMatrix::MakeFromXZ(Dir, FVector::UpVector).ToQuat();
	};
	const auto Placed = [&](const colony::HoldPlace& P)
	{
		const bool bHead = P.anchor == colony::HoldAnchor::Head;
		FQuat Frame = bHead ? Facing * FRotator(AimPitch, 0.0f, 0.0f).Quaternion() : Facing;
		if (bHead && bCrosshair && &P == Aim)
		{
			// Twice: the second time from where the first put the sight, so the sight line itself meets the crosshair.
			Frame = AimAt(Head);
			const FTransform First = HoldDraw::PlaceInAnchor(P, Points, Size, BodyPlace) * FTransform(Frame, Head);
			Frame = AimAt(First.TransformPosition(Points.Sight * Size));
		}
		return HoldDraw::PlaceInAnchor(P, Points, Size, BodyPlace) * FTransform(Frame, bHead ? Head : Chest);
	};
	const auto Blend = [](const FTransform& A, const FTransform& B, float T)
	{
		const float E = MeleePose::EaseInOut(FMath::Clamp(T, 0.0f, 1.0f));
		return FTransform(FQuat::Slerp(A.GetRotation(), B.GetRotation(), E), FMath::Lerp(A.GetLocation(), B.GetLocation(), static_cast<double>(E)));
	};
	FTransform Weapon = Placed(*Ready);
	if (Aim && Pawn.AimBlend > 0.0f)
	{
		Weapon = Blend(Weapon, Placed(*Aim), Pawn.AimBlend);
	}
	if (Block && Pawn.BlockBlend > 0.0f)
	{
		Weapon = Blend(Weapon, Placed(*Block), Pawn.BlockBlend);
	}
	if (Pawn.SwingBlend > 0.0f)
	{
		// The procedural swing's weapon pose (the old drawing's, which swings the weapon round the body), with the
		// main grip where that pose put the hand.
		FVector SwingLocation;
		FRotator SwingRotation;
		Actor.GetMeleeWeaponPose(SwingLocation, SwingRotation);
		FTransform ActorFrame = Actor.GetActorTransform();
		ActorFrame.SetScale3D(FVector::OneVector);
		const FTransform SwingWeapon = FTransform(SwingRotation, SwingLocation * BodyPlace) * ActorFrame;
		Weapon = Blend(Weapon, SwingWeapon, Pawn.SwingBlend);
	}

	FTransform PinAt = Weapon;
	if (bFinisher && Pawn.FinisherBlend > 0.0f)
	{
		PinAt = Blend(Weapon, FinisherPinned, Pawn.FinisherBlend);
		Weapon = Blend(Weapon, FinisherDrawn, Pawn.FinisherBlend);
		++FinisherFrames;
	}

	HoldDraw::FPinned Pinned;
	if (!HoldDraw::PinHands(Mesh, Hold, Points, Size, PinAt, Pinned))
	{
		PinFailReason = TEXT("the hands could not be pinned");
		return false;
	}
	HoldDraw::ApplyPinned(Mesh, Hold, Pinned, static_cast<uint8>(EMakeHumanGoalSpace::Anchor), TEXT("neck_01"), Alpha * (1.0f - Pawn.HandFree), Alpha);
	OutWeaponInHand = Pinned.WeaponInHand;
	OutWeaponWorld = Weapon;
	OutWeaponWorld.SetScale3D(FVector::OneVector);
	return true;
}

namespace
{
	// Finishers (sneed 2026-11-20, docs/design/lock-on.md "Finishers"): where the weapon's point is and which way the
	// weapon points through each one, keyed by the finisher's share of its time. Point: cm from the spot on the body it is
	// driven at, along the attacker's line to it (F), to its right (R) and up (U). Direction: grip to point, (F, R, U).
	// Drawing numbers, ours; the hit key is at the finisher's hitSeconds share (MeleeRules finishers).
	struct FWeaponFinisherKey
	{
		float T;
		FVector Point;
		FVector Dir;
	};
	struct FWeaponFinisherMove
	{
		const TCHAR* Name;
		bool bReverse;      // held point-down under the fist (a knife slammed down, a sword driven in)
		const TCHAR* Bone;  // where on the body on the ground it goes
		float SurfaceCm;    // the spot is this far above that bone (the body's top)
		TArray<FWeaponFinisherKey> Keys;
	};
	const TArray<FWeaponFinisherMove>& WeaponFinisherMoves()
	{
		static const TArray<FWeaponFinisherMove> Moves = {
			// The spear rammed down two-handed into the back (or belly), the shaft slanting back over the shoulder.
			{TEXT("Spear"), false, TEXT("spine_02"), 10.0f,
				{{0.00f, FVector(-5, 0, 30), FVector(0.6, 0, -0.8)}, {0.45f, FVector(-12, 0, 45), FVector(0.55, 0, -0.83)},
					{0.55f, FVector(-12, 0, 48), FVector(0.55, 0, -0.83)}, {0.611f, FVector(2, 0, -14), FVector(0.5, 0, -0.86)},
					{0.78f, FVector(2, 0, -14), FVector(0.5, 0, -0.86)}, {0.92f, FVector(-5, 0, 30), FVector(0.55, 0, -0.83)},
					{1.00f, FVector(-5, 0, 35), FVector(0.6, 0, -0.8)}}},
			// The club raised over the head, cocked back, and smashed down on the head; it bounces off.
			{TEXT("Club"), false, TEXT("head"), 16.0f,
				// On one knee (round 12, 2026-11-21: standing, the floor was out of reach and the body folded onto him), so
				// the raise is lower than a standing one.
				{{0.00f, FVector(-30, 15, 105), FVector(0.5, 0, 0.85)}, {0.25f, FVector(-60, 12, 140), FVector(-0.1, 0, 1)},
					{0.50f, FVector(-85, 8, 140), FVector(-0.6, 0, 0.8)}, {0.56f, FVector(-85, 8, 140), FVector(-0.6, 0, 0.8)},
					{0.59f, FVector(-35, 6, 145), FVector(0.3, 0, 0.95)}, {0.618f, FVector(0, 0, 4), FVector(0.6, 0, -0.8)},
					{0.66f, FVector(-5, 0, 25), FVector(0.6, 0, -0.7)}, {0.85f, FVector(-19, 10, 75), FVector(0.6, 0, 0.3)},
					{1.00f, FVector(-30, 15, 105), FVector(0.5, 0, 0.85)}}},
			// The sword raised point-down and driven into the chest, held, pulled out.
			{TEXT("Sword"), true, TEXT("spine_03"), 10.0f,
				{{0.00f, FVector(-25, 12, 20), FVector(0.4, 0, -1)}, {0.20f, FVector(-15, 8, 45), FVector(0.2, 0, -1)},
					{0.50f, FVector(-8, 4, 60), FVector(0.1, 0, -1)}, {0.56f, FVector(-6, 4, 62), FVector(0.1, 0, -1)},
					{0.605f, FVector(0, 0, -15), FVector(0.05, 0, -1)}, {0.76f, FVector(0, 0, -15), FVector(0.05, 0, -1)},
					{0.90f, FVector(-8, 4, 35), FVector(0.15, 0, -1)}, {1.00f, FVector(-20, 10, 25), FVector(0.35, 0, -1)}}},
			// The knife raised high in a fist and slammed straight down, like into a table; held, pulled out.
			{TEXT("Knife"), true, TEXT("spine_03"), 10.0f,
				{{0.00f, FVector(-30, 12, 35), FVector(0.2, 0, -1)}, {0.18f, FVector(-25, 10, 55), FVector(0.25, 0, -1)},
					{0.50f, FVector(-22, 8, 78), FVector(0.3, 0, -1)}, {0.56f, FVector(-20, 8, 80), FVector(0.3, 0, -1)},
					{0.625f, FVector(0, 0, -6), FVector(0.1, 0, -1)}, {0.74f, FVector(0, 0, -6), FVector(0.1, 0, -1)},
					{0.90f, FVector(-12, 6, 30), FVector(0.2, 0, -1)}, {1.00f, FVector(-20, 10, 40), FVector(0.2, 0, -1)}}},
		};
		return Moves;
	}

	// The point at T: a smooth curve through the keys (Hermite, tangents from the neighbours; none at a held key, so a
	// hold holds), the direction eased between keys.
	void WeaponFinisherAt(const FWeaponFinisherMove& Move, float T, FVector& OutPoint, FVector& OutDir)
	{
		const TArray<FWeaponFinisherKey>& K = Move.Keys;
		int32 I = 0;
		while (I + 2 < K.Num() && T > K[I + 1].T)
		{
			++I;
		}
		const FWeaponFinisherKey& A = K[I];
		const FWeaponFinisherKey& B = K[I + 1];
		const float Span = FMath::Max(0.001f, B.T - A.T);
		const float S = FMath::Clamp((T - A.T) / Span, 0.0f, 1.0f);
		const auto Tangent = [&](int32 J)
		{
			if (J <= 0 || J >= K.Num() - 1 || K[J].Point.Equals(K[J - 1].Point, 0.5) || K[J].Point.Equals(K[J + 1].Point, 0.5))
			{
				return FVector::ZeroVector;
			}
			return (K[J + 1].Point - K[J - 1].Point) / FMath::Max(0.001f, K[J + 1].T - K[J - 1].T);
		};
		const FVector M0 = Tangent(I) * Span;
		const FVector M1 = Tangent(I + 1) * Span;
		const float S2 = S * S;
		const float S3 = S2 * S;
		OutPoint = A.Point * (2 * S3 - 3 * S2 + 1) + M0 * (S3 - 2 * S2 + S) + B.Point * (-2 * S3 + 3 * S2) + M1 * (S3 - S2);
		const float E = MeleePose::EaseInOut(S);
		OutDir = FMath::Lerp(A.Dir.GetSafeNormal(), B.Dir.GetSafeNormal(), static_cast<double>(E)).GetSafeNormal();
	}
}

bool UWeaponDrawSubsystem::FinisherPose(const ASimPawnActor& Actor, const colony::ThingView& View, const HoldDraw::FPoints& Points, float Size,
										FTransform& OutDrawn, FTransform& OutPinned, float& OutWeight)
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (View.meleeFinisher == 0 || View.meleeFinisherTarget == colony::kNoEntity || !Bridge || !Bridge->GetSim())
	{
		if (View.meleeFinisher != 0)
		{
			PinFailReason = TEXT("finisher with no target");
		}
		return false;
	}
	const std::vector<colony::MeleeFinisher>& Finishers = Bridge->GetSim()->melee().rules().finishers;
	const size_t Index = static_cast<size_t>(View.meleeFinisher) - 1;
	if (Index >= Finishers.size())
	{
		return false;
	}
	const FString MoveName = UTF8_TO_TCHAR(Finishers[Index].move.c_str());
	const FWeaponFinisherMove* Move = WeaponFinisherMoves().FindByPredicate([&](const FWeaponFinisherMove& M) { return MoveName == M.Name; });
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	const ASimPawnActor* Target = Pawns ? Pawns->FindPawn(View.meleeFinisherTarget) : nullptr;
	const colony::ThingView* TargetView = Bridge->GetCurrentSnapshot().find(View.meleeFinisherTarget);
	if (!Move || (!Target && !TargetView))
	{
		PinFailReason = !Move ? TEXT("finisher move not in the table") : TEXT("finisher target not drawn");
		return false;
	}
	// The spot on the body: its bone as drawn now (lying, crawling or a ragdoll), lifted to the body's top; of the move's
	// bone, the spine's, the hips' and the thighs', the one nearest the attacker's shoulder (a ragdoll lies any way round
	// and slides: the chest may be the far end, and the attacker never slides over to it, round 10 2026-11-21).
	// With no pawn actor (a corpse drawn otherwise), the sim's place of the body, on the ground.
	const USkeletalMeshComponent* TargetMesh = Target ? HoldDraw::VisibleMesh(*Target) : nullptr;
	const float TargetScale = Target ? Target->GetBodyScale() : 1.0f;
	FVector Spot = Target ? Target->GetActorLocation() + FVector(0.0, 0.0, 20.0)
						  : Bridge->SimToWorld(TargetView->x, TargetView->y, TargetView->level) + FVector(0.0, 0.0, 20.0);
	if (TargetMesh)
	{
		const USkeletalMeshComponent* OwnMesh = HoldDraw::VisibleMesh(Actor);
		const FVector From = OwnMesh && OwnMesh->GetBoneIndex(TEXT("upperarm_r")) != INDEX_NONE ? OwnMesh->GetSocketLocation(TEXT("upperarm_r"))
																								: Actor.GetActorLocation();
		const FName Own(Move->Bone);
		double Best = TNumericLimits<double>::Max();
		for (const FName Bone : {Own, FName(TEXT("spine_03")), FName(TEXT("spine_02")), FName(TEXT("spine_01")), FName(TEXT("pelvis")),
				 FName(TEXT("thigh_l")), FName(TEXT("thigh_r"))})
		{
			if (TargetMesh->GetBoneIndex(Bone) == INDEX_NONE)
			{
				continue;
			}
			const FVector At = TargetMesh->GetSocketLocation(Bone);
			// The move's own bone wins unless another is 15 cm nearer.
			const double Distance = FVector::Dist(At, From) + (Bone == Own ? -15.0 : 0.0);
			if (Distance < Best)
			{
				Best = Distance;
				Spot = At;
			}
		}
	}
	// Never above the body's middle: a downed body still getting up, kneeling or crawling is struck low, where the
	// attacker's arms reach (a standing one put the spot 1.3 m up and the weapon off the hands).
	if (Target)
	{
		Spot.Z = FMath::Min(Spot.Z, Target->GetActorLocation().Z);
	}
	Spot.Z += Move->SurfaceCm * TargetScale;
	FVector F = (Spot - Actor.GetActorLocation()).GetSafeNormal2D();
	if (F.IsNearlyZero())
	{
		F = Actor.GetActorForwardVector().GetSafeNormal2D();
	}
	const FVector U = FVector::UpVector;
	const FVector R = FVector::CrossProduct(U, F);
	const float T = FMath::Clamp(View.meleeFinisherT, 0.0f, 1.0f);
	FVector Point;
	FVector Dir;
	WeaponFinisherAt(*Move, T, Point, Dir);
	const FVector PointWorld = Spot + F * Point.X + R * Point.Y + U * Point.Z;
	const FVector DirWorld = (F * Dir.X + R * Dir.Y + U * Dir.Z).GetSafeNormal();
	const FQuat Drawn = FRotationMatrix::MakeFromXZ(DirWorld, F).ToQuat();
	OutDrawn = FTransform(Drawn, PointWorld - Drawn.RotateVector(Points.Tip * Size));
	OutPinned = OutDrawn;
	if (Move->bReverse)
	{
		// The hands close on the weapon as if its blade stood up out of the thumb side; drawn, it comes out under the
		// little finger, point down: the same grip turned half round about the fingers' line.
		const FQuat Up = FRotationMatrix::MakeFromXZ(-DirWorld, F).ToQuat();
		const FVector Grip = OutDrawn.TransformPosition(Points.Main * Size);
		OutPinned = FTransform(Up, Grip - Up.RotateVector(Points.Main * Size));
	}
	// Within the arms' reach: where the body (its ragdoll slid, a long blade, a standing attacker over a low body) puts
	// the grip beyond what the arm can reach from the shoulder, the weapon comes back along that line until the hand
	// holds it, and strikes short of the body instead of hanging off the hands.
	if (const USkeletalMeshComponent* Mesh = HoldDraw::VisibleMesh(Actor); Mesh && Mesh->GetBoneIndex(TEXT("upperarm_r")) != INDEX_NONE &&
		Mesh->GetBoneIndex(TEXT("lowerarm_r")) != INDEX_NONE && Mesh->GetBoneIndex(TEXT("hand_r")) != INDEX_NONE)
	{
		const FVector Shoulder = Mesh->GetSocketLocation(TEXT("upperarm_r"));
		const double Arm = FVector::Dist(Mesh->GetSocketLocation(TEXT("upperarm_r")), Mesh->GetSocketLocation(TEXT("lowerarm_r"))) +
			FVector::Dist(Mesh->GetSocketLocation(TEXT("lowerarm_r")), Mesh->GetSocketLocation(TEXT("hand_r")));
		const double Reach = Arm * 0.97;  // nearly straight: a finisher reaches all the way down
		// First the weapon tips back about its point toward the shoulder (up to 50 degrees), so the point stays on the
		// body and the grip comes into the hands (a longsword's grip a metre above the body, round 12 2026-11-21).
		{
			const FVector TipNow = OutDrawn.TransformPosition(Points.Tip * Size);
			const FVector GripNow = OutPinned.TransformPosition(Points.Main * Size);
			const FVector FromTip = GripNow - TipNow;
			const FVector ToShoulder = (Shoulder - TipNow).GetSafeNormal();
			if (Arm > 10.0 && FVector::Dist(GripNow, Shoulder) > Reach && !FromTip.IsNearlyZero() && !ToShoulder.IsNearlyZero())
			{
				const FQuat Full = FQuat::FindBetweenNormals(FromTip.GetSafeNormal(), ToShoulder);
				const double FullDegrees = FMath::RadiansToDegrees(Full.GetAngle());
				FQuat Tilt = FQuat::Identity;
				for (double Degrees = 5.0; Degrees <= FMath::Min(50.0, FullDegrees) + 0.01; Degrees += 5.0)
				{
					Tilt = FQuat::Slerp(FQuat::Identity, Full, static_cast<float>(Degrees / FMath::Max(FullDegrees, 1.0)));
					if (FVector::Dist(TipNow + Tilt.RotateVector(FromTip), Shoulder) <= Reach)
					{
						break;
					}
				}
				const FTransform AboutTip = FTransform(-TipNow) * FTransform(Tilt) * FTransform(TipNow);
				OutDrawn = OutDrawn * AboutTip;
				OutPinned = OutPinned * AboutTip;
			}
		}
		const FVector Grip = OutPinned.TransformPosition(Points.Main * Size);
		const FVector Out = Grip - Shoulder;
		if (Arm > 10.0 && Out.Size() > Reach)
		{
			const FVector Back = Out.GetSafeNormal() * (Out.Size() - Reach);
			OutDrawn.AddToTranslation(-Back);
			OutPinned.AddToTranslation(-Back);
			FinisherShort = static_cast<float>(Back.Size());
		}
		else
		{
			FinisherShort = 0.0f;
		}
		// How far ahead of the shoulder the grip wants to be: bending further over only helps while it is ahead.
		FinisherAhead = static_cast<float>(FVector::DotProduct(Out, Actor.GetActorForwardVector().GetSafeNormal2D()));
	}
	// In over the first seventh, out over the last.
	OutWeight = MeleePose::EaseInOut(FMath::Clamp(T / 0.14f, 0.0f, 1.0f)) * (1.0f - MeleePose::EaseInOut(FMath::Clamp((T - 0.86f) / 0.14f, 0.0f, 1.0f)));
	FinisherTip = OutDrawn.TransformPosition(Points.Tip * Size);
	FinisherGoal = Spot;
	return true;
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
	float Bend = View.carryingPawn != colony::kNoEntity && !View.downed && !View.dead ? WeaponCarryBend : 0.0f;
	// Through a finisher: bent over the body on the ground, most at the blow (sneed 2026-11-20).
	const std::vector<colony::MeleeFinisher>& Finishers = Bridge->GetSim() ? Bridge->GetSim()->melee().rules().finishers : std::vector<colony::MeleeFinisher>{};
	if (View.meleeFinisher != 0 && static_cast<size_t>(View.meleeFinisher) <= Finishers.size() && !View.downed)
	{
		const colony::MeleeFinisher& F = Finishers[static_cast<size_t>(View.meleeFinisher) - 1];
		const float T = View.meleeFinisherT;
		const float Hit = F.hitSeconds / FMath::Max(0.1f, F.seconds);
		const float In = MeleePose::EaseInOut(FMath::Clamp(T / 0.2f, 0.0f, 1.0f)) * (1.0f - MeleePose::EaseInOut(FMath::Clamp((T - 0.86f) / 0.14f, 0.0f, 1.0f)));
		const float AtBlow = 1.0f - FMath::Clamp(FMath::Abs(T - Hit) / 0.12f, 0.0f, 1.0f);
		Bend = FMath::Max(Bend, F.bend * In * (0.75f + 0.35f * AtBlow) + Pawn.FinisherReachBend * In);
	}
	Anim->SetSpineBend(Bend);
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
	// The combat range's targets are on your side (so they never shoot back); there anyone in front of you can be struck.
	const UGunRangeSubsystem* Range = GetWorld()->GetSubsystem<UGunRangeSubsystem>();
	const bool bAnySide = Range && Range->IsEnabled();
	// Aim assist (sneed 2026-11-20: "if I use a melee weapon near a non-friendly, even neutral, the attack aims at them
	// even if not focused"): anyone not on its side, or a prisoner, in the swing's reach (as far as the player walks on
	// under it) within 70 degrees of the view, the one nearest the middle of the view and nearest first. A body on the
	// ground the swing passes over is left to the sim, which drives the blow down at it (a finisher).
	float BestDistance = 0.0f;
	for (const colony::ThingView& Other : Bridge->GetCurrentSnapshot().things)
	{
		if (Other.category != colony::ThingCategory::Pawn || Other.id == Me->id || Other.dead || Other.prisoner || (Other.faction == Me->faction && !bAnySide) ||
			!Melee.inPlayerReach(Me->id, Other.id))
		{
			continue;
		}
		const FVector Off = FVector(Other.x - Me->x, Other.y - Me->y, 0.0);
		const FVector To = Off.GetSafeNormal();
		const float Cosine = static_cast<float>(FVector::DotProduct(To, Look));
		const float Distance = static_cast<float>(Off.Size2D());
		if (Cosine < WeaponSwingCone)
		{
			continue;
		}
		// Within 20 degrees of each other counts as the same direction: then the nearer one.
		if (Best == colony::kNoEntity || Cosine > BestScore + 0.06f || (Cosine > BestScore - 0.06f && Distance < BestDistance))
		{
			BestScore = Cosine;
			BestDistance = Distance;
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
	if (Rim3DKeys::IsDown(PC, ERim3DAction::Attack))
	{
		Swing();
	}
	// The right mouse button holds up the shield when one is on the arm, else the weapon (a gun aims, ProtagonistPawn).
	USimSubsystem* Bridge = nullptr;
	const colony::ThingView* Me = ControlledView(Bridge);
	const bool bRight = bScriptedRight ? bScriptRightHeld : Rim3DKeys::IsDown(PC, ERim3DAction::AimBlock);
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
	if (Rim3DKeys::WasPressed(PC, ERim3DAction::SwitchWeapon))
	{
		SwitchNext();
	}
}
