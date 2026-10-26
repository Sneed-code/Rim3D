#include "WeaponDrawSubsystem.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"

#include "FightSubsystem.h"
#include "OrdersSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "VisualMesh.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/WeaponSlot.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeaponDraw, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GWeaponDraw(
		TEXT("Weapons.Draw"), 1, TEXT("1 draws the weapons pawns carry and their swings, 0 draws none."), ECVF_Default);

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

	FVector WeaponSimToWorld(const USimSubsystem& Bridge, float X, float Y, float Z)
	{
		return Bridge.GetWorldOrigin() + FVector(X * 100.0, Y * 100.0, Z * 100.0);
	}

	FRotator WeaponSlerp(const FRotator& A, const FRotator& B, float T)
	{
		return FQuat::Slerp(A.Quaternion(), B.Quaternion(), T).Rotator();
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

// ---------------------------------------------------------------------------
// Meshes and poses.

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
	if (!SolidMaterial)
	{
		SolidMaterial = LoadObject<UMaterialInterface>(nullptr, WeaponSolidMaterialPath);
		if (!SolidMaterial)
		{
			UE_LOG(LogWeaponDraw, Error, TEXT("Could not load %s; weapons cannot be drawn"), WeaponSolidMaterialPath);
			return nullptr;
		}
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

	for (int32 Slot = 0; Slot < GWeaponDisplaySlots; ++Slot)
	{
		FDisplayed& Shown = Pawn.Slots[Slot];
		const colony::DefId Def = Wanted[Slot];
		if (!Def.valid())
		{
			if (Shown.Component.IsValid())
			{
				Shown.Component->DestroyComponent();
			}
			Shown = FDisplayed();
			continue;
		}
		if (!Shown.Component.IsValid() || Shown.Def != Def)
		{
			if (Shown.Component.IsValid())
			{
				Shown.Component->DestroyComponent();
			}
			Shown = FDisplayed();
			UStaticMesh* Mesh = WeaponMesh(Def);
			if (!Mesh)
			{
				continue;
			}
			Shown.Component = MakeComponent(Actor, Mesh);
			Shown.Def = Def;
			Shown.Component->SetRelativeScale3D(FVector(Slot == static_cast<int32>(colony::WeaponSlot::Sidearm) ? WeaponSidearmScale : 1.0f));
		}

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

		FVector HolsterLocation;
		FRotator HolsterRotation;
		HolsterPose(Slot, Height, HolsterLocation, HolsterRotation);
		FVector HandsLocation;
		FRotator HandsRotation;
		if (IsGun(Def))
		{
			HoldPose(Slot, Height, HandsLocation, HandsRotation);
			// The protagonist's gun follows the camera's pitch.
			if (const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
				Protagonist && Protagonist->IsDirectMode() && Protagonist->GetControlled() == View.id && Protagonist->GetBody())
			{
				HandsRotation.Pitch = Protagonist->GetBody()->GetPitch();
			}
		}
		else
		{
			Actor.GetMeleeWeaponPose(HandsLocation, HandsRotation);
		}
		const FVector Location = FMath::Lerp(HolsterLocation, HandsLocation, static_cast<double>(InHand));
		const FRotator Rotation = WeaponSlerp(HolsterRotation, HandsRotation, InHand);
		Shown.Component->SetRelativeLocationAndRotation(Location, Rotation);
		Shown.Component->SetVisibility(bShow);
	}
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
		return;
	}
	// With a building in hand the mouse buttons plan it and cancel it (UHandsOnSubsystem).
	const UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>();
	if (Orders && Orders->IsDirectBuilding())
	{
		return;
	}
	if (PC->IsInputKeyDown(EKeys::LeftMouseButton))
	{
		Swing();
	}
	if (PC->IsInputKeyDown(EKeys::RightMouseButton))
	{
		Block();
	}
	if (PC->WasInputKeyJustPressed(EKeys::Q))
	{
		SwitchNext();
	}
}
