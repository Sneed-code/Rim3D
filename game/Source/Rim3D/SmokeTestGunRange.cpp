// The gun range (docs/design/gun-range.md): a first-person scene to play, not a check. The player's colonist (a random
// creator woman) stands at one end of a cleared flat field with the guns of assets/guns.json in a rack; random creator
// women stand and walk at 8 to 30 m. The sim does all the shooting, wounds, downing and dying; UGunRangeSubsystem draws
// the arms and guns. K (or downing them all) brings a new group. -GunRangeScript runs a short tour by itself and saves
// pictures to game/Saved/GunRange/ (each gun from the hip, aimed, firing and cooling down, then a downed target).

#include "SmokeTest.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#include "BloodSubsystem.h"
#include "CreatorPeople.h"
#include "FightSubsystem.h"
#include "GunRangeSubsystem.h"
#include "HoldDraw.h"
#include "MakeHumanAnimInstance.h"
#include "KeyBindings.h"
#include "KillFeedSubsystem.h"
#include "PawnActor.h"
#include "PawnAnimInstance.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "SoundSubsystem.h"
#include "WeaponDrawSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

namespace
{
	const FVector GunRangeOrigin(-1695000.0, 1300000.0, 0.0);
	constexpr int32 GunRangeTargets = 8;

	/** How a target behaves (sneed, 2026-11-17): some stand, some flee on sight, some flee when attacked, some fight back
	 *  with their fists when attacked. Driven by the scene with ordinary commands, as a player would drive them, until the
	 *  sim's own sight and flee settings come (the stealth thread). */
	enum class EGunRangeBehaviour : uint8
	{
		Stand,
		FleeOnSight,
		FleeWhenHit,
		Fists,
	};

	const TCHAR* GunRangeBehaviourName(EGunRangeBehaviour B)
	{
		switch (B)
		{
			case EGunRangeBehaviour::FleeOnSight: return TEXT("flees on sight");
			case EGunRangeBehaviour::FleeWhenHit: return TEXT("flees when hit");
			case EGunRangeBehaviour::Fists: return TEXT("fights back");
			default: return TEXT("stands");
		}
	}

	/** She sees the player this close (the field is open; the sim's sight will replace this). */
	constexpr float GunRangeSightMetres = 20.0f;

	struct FGunRangeTarget
	{
		colony::Entity Thing = colony::kNoEntity;
		EGunRangeBehaviour Behaviour = EGunRangeBehaviour::Stand;
		bool bFleeing = false;
		bool bAngry = false;
		bool bTurned = false;   // struck and handed to the sim's fight response
		bool bSprinted = false;
		bool bCharged = false;
		float TurnedDist = 0.0f;
		float NextOrder = 0.0f;
		int32 Seed = 0;
		TWeakObjectPtr<USkeletalMeshComponent> AppliedTo;
		bool bWalker = false;
		colony::CellPos A;
		colony::CellPos B;
		bool bToB = true;
		float NextMove = 0.0f;
		// A guard group (U, docs/design/stealth.md): she walks a few steps away from you, then at `GuardAt` goes over to a
		// hostile side, unaware, her back to you. From then on the sim's own sight, hearing and fighting drive her.
		float GuardAt = 0.0f;
		bool bGuard = false;
	};

	struct FGunRangeState
	{
		colony::Entity Player = colony::kNoEntity;
		int32 PlayerSeed = 0;
		bool bRandomPlayer = false;
		TWeakObjectPtr<USkeletalMeshComponent> PlayerAppliedTo;
		bool bControlled = false;
		bool bArmed = false;
		colony::CellPos Middle{64, 64, 0};
		colony::CellPos Stand{50, 64, 0};
		colony::CellPos RackCell{49, 62, 0};
		TArray<colony::Entity> Rack;
		TArray<FGunRangeTarget> Targets;
		TSet<colony::Entity> Known;   // pawns that existed before a spawn (to find the new ones)
		TSet<colony::Entity> Attacked; // pawns a shot or a blow has struck (from the sim's events)
		bool bListening = false;
		int32 Fled = 0;
		int32 FistSwings = 0;
		int32 Sprinted = 0;
		int32 Charged = 0;
		int32 PendingSpawn = 0;       // frames until the spawned pawns are looked for
		int32 Group = 0;
		bool bGuards = false;         // this group are unaware hostile guards to sneak up on (U)
		int32 GuardsSet = 0;
		bool bStopAlarm = false;      // the colony alarm went off at the guards: the range switches it off again
		float AllDownFor = 0.0f;
		float Clock = 0.0f;
		float GrenadesNextCheck = 0.0f; // the belt is topped up when empty (sneed 2026-11-19)
		bool bGrenadesSpawned = false;
		// The scripted tour.
		int32 Step = 0;
		float StepTime = 0.0f;
		int32 Gun = 0;
		int32 Shots = 0;
		bool bFiringPicture = false;
		int32 Pictures = 0;
		// A picture a moment after the first react clip of each gun (the flinch and the blood).
		int32 HitClipsAtStep = 0;
		float HitSeenAt = -1.0f;
		bool bHitPicture = false;
		bool bSwingPicture = false;
		int32 MeleeTour = 0;  // step 15: every rack melee weapon in the hand, first and third person
		int32 GunTour = 0;    // step 16: every rack gun held ready and aimed, first and third person
		// Arms that can't bend wrong (docs/design/weapon-hold.md): every frame a hand is on a weapon, both views, each arm
		// measured (the solver's own reading and the drawn bones'); any angle outside human limits is a failure.
		int32 ArmFrames = 0;
		int32 ArmFails = 0;
		int32 ArmShort = 0;
		float ArmWorstGap = 0.0f;
		float ArmWorstTwist = 0.0f;
		float ArmWorstWrist = 0.0f;
		float ArmWorstElbow = 0.0f;
		float ArmWorstShort = 0.0f;
		TMap<FString, int32> ArmFailsBy;
		bool bKillFeedLines = false;
		colony::Entity FlickFrom = colony::kNoEntity;  // step 14: the lock before the flick
		bool bFlickLeftTried = false;
		bool bFlickTried = false;
		// -GunRangeIdleRecord=<seconds>: frames of a standing target close up (gunrange_idle/rec_0000.png ...).
		int32 IdleRecorded = 0;
		TWeakObjectPtr<ACameraActor> IdleCamera;
	};

	void GunRangeStrip(colony::Sim& Sim, USimSubsystem& B, colony::Entity Pawn)
	{
		for (const colony::Entity Item : Sim.apparel().worn(Pawn))
		{
			std::string Error;
			Sim.apparel().takeOff(Pawn, Item, Error);
			B.Submit(colony::DestroyThingCommand{Item});
		}
	}
}

void USmokeTestSubsystem::BuildGunRangePhases()
{
	const TSharedRef<FGunRangeState> State = MakeShared<FGunRangeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto PawnsOf = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto Range = [this]() { return GetWorld()->GetSubsystem<UGunRangeSubsystem>(); };
	const auto Prot = [this]() { return GetWorld()->GetSubsystem<UProtagonistSubsystem>(); };
	const bool bScript = FParse::Param(FCommandLine::Get(), TEXT("GunRangeScript"));
	int32 BaseSeed = 1;
	FParse::Value(FCommandLine::Get(), TEXT("GunRangeSeed="), BaseSeed);

	// A new group of targets: the old ones go, eight new random women come (seeded by the group number).
	const auto SpawnGroup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		for (const FGunRangeTarget& T : State->Targets)
		{
			if (B->GetCurrentSnapshot().find(T.Thing))
			{
				B->Submit(colony::DestroyThingCommand{T.Thing});
			}
		}
		State->Targets.Reset();
		State->Known.Reset();
		State->Attacked.Reset();
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn)
			{
				State->Known.Add(View.id);
			}
		}
		// Two rows: near ones standing, far ones walking across.
		static const int32 Ahead[GunRangeTargets] = {10, 12, 14, 17, 20, 23, 26, 30};
		static const int32 Side[GunRangeTargets] = {-3, 3, -6, 1, 6, -2, 4, -5};
		for (int32 i = 0; i < GunRangeTargets; ++i)
		{
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{State->Stand.x + Ahead[i], State->Stand.y + Side[i], 0}});
		}
		State->PendingSpawn = 2;
		++State->Group;
		State->AllDownFor = 0.0f;
	};

	// The spawned pawns found a step later: seeds, the player's side, drafted (they stand where told), no clothes.
	const auto AdoptGroup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		colony::Sim* Sim = B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr;
		if (!Sim || !Pawns)
		{
			return;
		}
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		const colony::DefDatabase* Defs = B->GetDefs().get();
		TArray<const colony::ThingView*> New;
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category == colony::ThingCategory::Pawn && !State->Known.Contains(View.id) && View.id != State->Player && !View.dead)
			{
				New.Add(&View);
			}
		}
		New.Sort([](const colony::ThingView& L, const colony::ThingView& R) { return L.x < R.x; });
		for (int32 i = 0; i < New.Num(); ++i)
		{
			const colony::ThingView& View = *New[i];
			FGunRangeTarget T;
			T.Thing = View.id;
			T.Seed = (BaseSeed * 7919 + State->Group * 1000 + i) * 104729 + 17;
			const FSimPawnLooks From = View.looks >= 0 ? USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(View.looks)], Defs) : FSimPawnLooks();
			Pawns->SetLooksOverride(View.id, CreatorPeople::RandomLooks(T.Seed, true, From));
			GunRangeStrip(*Sim, *B, View.id);
			B->Submit(colony::SetFactionCommand{View.id, 1});
			// The far half walk across the field and back; the near half stand. In each half: one stands, one flees when
			// hit, one fights back with her fists, one flees as soon as she sees you.
			T.bWalker = i >= GunRangeTargets / 2;
			static const EGunRangeBehaviour Kinds[4] = {EGunRangeBehaviour::Stand, EGunRangeBehaviour::FleeWhenHit, EGunRangeBehaviour::Fists,
				EGunRangeBehaviour::FleeOnSight};
			T.Behaviour = Kinds[i % 4];
			const colony::CellPos Here{FMath::FloorToInt32(View.x), FMath::FloorToInt32(View.y), 0};
			T.A = colony::CellPos{Here.x, State->Stand.y - 9, 0};
			T.B = colony::CellPos{Here.x, State->Stand.y + 9, 0};
			T.NextMove = State->Clock + 1.0f + i * 0.7f;
			if (State->bGuards)
			{
				// Guards: nobody walks or follows the scene's own behaviours; each turns her back by walking 3 m away.
				T.bWalker = false;
				// Three of four come at you with their fists once alerted; the fourth runs as soon as she sees you.
				T.Behaviour = i % 4 == 3 ? EGunRangeBehaviour::FleeOnSight : EGunRangeBehaviour::Fists;
				T.GuardAt = State->Clock + 6.0f;
				T.A = colony::CellPos{Here.x + 3, Here.y, 0};
			}
			State->Targets.Add(T);
		}
		B->StepOnce();
		for (const FGunRangeTarget& T : State->Targets)
		{
			B->Submit(colony::DraftCommand{T.Thing, true});
			if (State->bGuards)
			{
				B->Submit(colony::MoveOrderCommand{T.Thing, T.A});
			}
		}
		Notes.Add(FString::Printf(TEXT("gunrange: group %d, %d targets%s"), State->Group, State->Targets.Num(), State->bGuards ? TEXT(", unaware guards") : TEXT("")));
		for (const FGunRangeTarget& T : State->Targets)
		{
			Notes.Add(FString::Printf(TEXT("gunrange:   target %u %s%s"), entt::to_integral(T.Thing), GunRangeBehaviourName(T.Behaviour), T.bWalker ? TEXT(", walking") : TEXT("")));
		}
	};

	FPhase Play;
	Play.Name = TEXT("gunrange");
	Play.WarmupSeconds = 6.0f;
	float Minutes = bScript ? 3.5f : 240.0f;  // the melee line-up (step 15) adds about 25 s, the gun hold tour (16) about 30 s
	FParse::Value(FCommandLine::Get(), TEXT("GunRangeMinutes="), Minutes);
	Play.MeasureSeconds = Minutes * 60.0f;
	Play.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		UGunRangeSubsystem* G = Range();
		if (!B || !Pawns || !G)
		{
			Fail(TEXT("USimSubsystem, USimPawnSubsystem or UGunRangeSubsystem did not start"));
			return;
		}
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			for (const TCHAR* Command : {TEXT("sg.ShadowQuality 4"), TEXT("sg.GlobalIlluminationQuality 4"), TEXT("sg.ReflectionQuality 4"),
				TEXT("sg.PostProcessQuality 4"), TEXT("sg.TextureQuality 4"), TEXT("sg.EffectsQuality 4"), TEXT("sg.FoliageQuality 4"),
				TEXT("sg.ShadingQuality 4"), TEXT("sg.ViewDistanceQuality 4"), TEXT("sg.AntiAliasingQuality 4"), TEXT("r.ScreenPercentage 100"),
				TEXT("r.AntiAliasingMethod 4"), TEXT("r.DynamicGlobalIlluminationMethod 1"), TEXT("r.ReflectionMethod 1"),
				TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor 8"), TEXT("r.Shadow.Virtual.Enable 1"), TEXT("r.SSS.Quality 1"),
				TEXT("r.SSS.HalfRes 0"), TEXT("r.ContactShadows 1"), TEXT("Pawns.FullDetail 20"), TEXT("r.SetNearClipPlane 2"),
				TEXT("Overseer.UI 0"), TEXT("DisableAllScreenMessages")})
			{
				PC->ConsoleCommand(Command);
			}
		}
		float Hour = 10.0f;
		FParse::Value(FCommandLine::Get(), TEXT("GunRangeHour="), Hour);
		for (const TPair<const TCHAR*, float>& Var : {TPair<const TCHAR*, float>(TEXT("Pawns.MetaHumans"), 1.0f), TPair<const TCHAR*, float>(TEXT("Sky.Hour"), Hour),
			TPair<const TCHAR*, float>(TEXT("Sky.DayOfYear"), 22.5f)})
		{
			if (IConsoleVariable* Found = IConsoleManager::Get().FindConsoleVariable(Var.Key))
			{
				Found->Set(Var.Value, ECVF_SetByCode);
			}
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 128;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(GunRangeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		B->StepOnce();
		colony::Sim* Sim = const_cast<colony::Sim*>(B->GetSim());
		State->Middle = Sim->stockpileCell().value_or(colony::CellPos{64, 64, 0});
		// A clear field: everything but pawns and every plant within 40 m goes.
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Pawn && FMath::Abs(View.x - State->Middle.x) < 40.0f && FMath::Abs(View.y - State->Middle.y) < 40.0f)
			{
				B->Submit(colony::DestroyThingCommand{View.id});
			}
		}
		for (int32 x = -40; x <= 40; ++x)
		{
			for (int32 y = -40; y <= 40; ++y)
			{
				Sim->plants().remove(colony::CellPos{State->Middle.x + x, State->Middle.y + y, 0});
			}
		}
		B->StepOnce();
		// The first colonist is the player (a random creator woman, so the arms are hers); the others leave.
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		TArray<const colony::ThingView*> Colonists;
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category == colony::ThingCategory::Pawn && Sim->colonistOf(View.id))
			{
				Colonists.Add(&View);
			}
		}
		Colonists.Sort([](const colony::ThingView& L, const colony::ThingView& R) { return L.id < R.id; });
		if (Colonists.IsEmpty())
		{
			Fail(TEXT("the Crashlanded start has no colonists"));
			return;
		}
		for (int32 i = 1; i < Colonists.Num(); ++i)
		{
			B->Submit(colony::DestroyThingCommand{Colonists[i]->id});
		}
		State->Player = Colonists[0]->id;
		// The targets stand out along +X from where the player is (the view starts looking that way).
		State->Stand = colony::CellPos{FMath::FloorToInt32(Colonists[0]->x), FMath::FloorToInt32(Colonists[0]->y), 0};
		State->RackCell = colony::CellPos{State->Stand.x - 1, State->Stand.y - 2, 0};
		State->PlayerSeed = BaseSeed * 31337 + 5;
		State->bRandomPlayer = FParse::Param(FCommandLine::Get(), TEXT("GunRangeRandomPlayer"));
		{
			const colony::DefDatabase* Defs = B->GetDefs().get();
			const FSimPawnLooks From = Colonists[0]->looks >= 0 ? USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(Colonists[0]->looks)], Defs) : FSimPawnLooks();
			// You are the creator's generic woman (sneed); -GunRangeRandomPlayer gives a random one instead.
			Pawns->SetLooksOverride(State->Player, State->bRandomPlayer ? CreatorPeople::RandomLooks(State->PlayerSeed, true, From) : CreatorPeople::DefaultLooks(true, From));
		}
		GunRangeStrip(*Sim, *B, State->Player);
		// The rack: one of each gun, then each melee weapon, at the player's feet.
		G->SetEnabled(true);
		TArray<FString> RackDefs;
		for (const FGunRangeGun& Gun : G->GetGuns())
		{
			RackDefs.Add(Gun.Def);
		}
		for (const FGunRangeMelee& Weapon : G->GetMelee())
		{
			RackDefs.Add(Weapon.Def);
		}
		for (const FString& Def : RackDefs)
		{
			B->Submit(colony::SpawnThingCommand{TCHAR_TO_UTF8(*Def), State->RackCell, 1});
		}
		B->StepOnce();
		State->bGuards = FParse::Param(FCommandLine::Get(), TEXT("GunRangeGuards"));
		SpawnGroup();
		B->StepOnce();
		B->StepOnce();
		AdoptGroup();
		State->PendingSpawn = 0;
		// The rack's items by def, in the order of the list (guns, then melee).
		TArray<colony::Entity> Rack;
		for (const FString& Def : RackDefs)
		{
			colony::Entity Found = colony::kNoEntity;
			for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
			{
				if (View.category == colony::ThingCategory::Item && View.def.valid() && B->GetDefs()->get(View.def).defName() == TCHAR_TO_UTF8(*Def) &&
					FMath::Abs(View.x - (State->RackCell.x + 0.5f)) < 3.0f && FMath::Abs(View.y - (State->RackCell.y + 0.5f)) < 3.0f && !Rack.Contains(View.id))
				{
					Found = View.id;
				}
			}
			if (Found == colony::kNoEntity)
			{
				Notes.Add(FString::Printf(TEXT("gunrange: no %s in the rack"), *Def));
			}
			Rack.Add(Found);
		}
		State->Rack = Rack;
		G->SetRack(Rack);
		B->SetSpeed(1);
	};

	Play.OnFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		UGunRangeSubsystem* G = Range();
		UProtagonistSubsystem* P = Prot();
		if (!B || !B->GetSim() || !Pawns || !G || !P)
		{
			return;
		}
		const float Dt = GetWorld()->GetDeltaSeconds();
		State->Clock += Dt;
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		if (!State->bListening)
		{
			// Who was struck: a bullet (on the body or its armour) or a blow that landed.
			State->bListening = true;
			TWeakPtr<FGunRangeState> Weak = State;
			B->OnSimEvent.AddLambda([Weak](const colony::Event& Event)
			{
				const TSharedPtr<FGunRangeState> Shared = Weak.Pin();
				const bool bStruck = Event.hit == colony::ProjectileHitKind::Pawn || Event.hit == colony::ProjectileHitKind::Armor;
				if (Shared && bStruck && (Event.type == colony::EventType::ProjectileHit || Event.type == colony::EventType::MeleeHit))
				{
					Shared->Attacked.Add(Event.target);
				}
				if (Shared && Event.type == colony::EventType::AlarmChanged && Event.message.rfind("on", 0) == 0)
				{
					Shared->bStopAlarm = true;
				}
			});
		}
		const colony::ThingView* PlayerView = Snapshot.find(State->Player);
		if (State->bStopAlarm)
		{
			// No klaxon in the range: stopped by hand, it stays off while the guards are about.
			State->bStopAlarm = false;
			B->Submit(colony::SoundAlarmCommand{false});
		}

		// Grenades (sneed 2026-11-19): five frag grenades and five molotovs on the belt, topped up when it is empty. They are
		// laid at your feet and put on the belt, the sim's way (EquipCommand), so the throw is the game's own.
		if (State->bControlled && PlayerView && !PlayerView->dead && State->Clock >= State->GrenadesNextCheck)
		{
			const colony::CellPos Feet{FMath::FloorToInt32(PlayerView->x), FMath::FloorToInt32(PlayerView->y), 0};
			if (PlayerView->grenades > 0)
			{
				State->bGrenadesSpawned = false;
				State->GrenadesNextCheck = State->Clock + 1.0f;
			}
			else if (!State->bGrenadesSpawned)
			{
				B->Submit(colony::SpawnThingCommand{"Grenade_Frag", Feet, 5});
				B->Submit(colony::SpawnThingCommand{"Grenade_Molotov", Feet, 5});
				State->bGrenadesSpawned = true;
				State->GrenadesNextCheck = State->Clock + 0.3f;
			}
			else
			{
				for (const colony::ThingView& View : Snapshot.things)
				{
					if (View.category != colony::ThingCategory::Item || !View.def.valid() || FMath::Abs(View.x - PlayerView->x) > 3.0f || FMath::Abs(View.y - PlayerView->y) > 3.0f)
					{
						continue;
					}
					const std::string Name = B->GetDefs()->get(View.def).defName();
					if (Name == "Grenade_Frag" || Name == "Grenade_Molotov")
					{
						B->Submit(colony::EquipCommand{State->Player, View.id});
					}
				}
				State->bGrenadesSpawned = false;
				State->GrenadesNextCheck = State->Clock + 2.0f;
			}
		}

		// Take control once the body is drawn, in first person, facing the field.
		if (!State->bControlled && PhaseTime > 1.0f)
		{
			if (P->EnterDirectMode(State->Player))
			{
				State->bControlled = true;
				P->GetBody()->SetFirstPerson(true);
				P->GetBody()->Look(FRotator::NormalizeAxis(-P->GetBody()->GetYaw()), -2.0f - P->GetBody()->GetPitch());
				P->GetBody()->SnapCamera();
			}
		}
		if (State->bControlled && !State->bArmed && PhaseTime > 1.5f && G->SelectGun(0))
		{
			State->bArmed = true;
		}

		// The random looks go on once each body's creator base is there (again if it is rebuilt).
		if (ASimPawnActor* Actor = Pawns->FindPawn(State->Player); Actor && Actor->GetMakeHumanMesh() && State->PlayerAppliedTo.Get() != Actor->GetMakeHumanMesh())
		{
			if (State->bRandomPlayer ? CreatorPeople::ApplyRandom(State->PlayerSeed, true, *Actor) : CreatorPeople::ApplyDefault(true, *Actor))
			{
				State->PlayerAppliedTo = Actor->GetMakeHumanMesh();
			}
		}
		int32 Standing = 0, Downed = 0, Dead = 0;
		for (FGunRangeTarget& T : State->Targets)
		{
			const colony::ThingView* View = Snapshot.find(T.Thing);
			if (!View)
			{
				continue;
			}
			Standing += !View->downed && !View->dead;
			Downed += View->downed && !View->dead;
			Dead += View->dead;
			if (ASimPawnActor* Actor = Pawns->FindPawn(T.Thing); Actor && Actor->GetMakeHumanMesh() && T.AppliedTo.Get() != Actor->GetMakeHumanMesh())
			{
				if (CreatorPeople::ApplyRandom(T.Seed, true, *Actor))
				{
					T.AppliedTo = Actor->GetMakeHumanMesh();
				}
			}
			// A guard: once she has walked off, a hostile side and unaware; the sim does the rest (stealth.md).
			if (T.GuardAt > 0.0f)
			{
				if (!T.bGuard && State->Clock >= T.GuardAt && !View->downed && !View->dead)
				{
					B->Submit(colony::SetFactionCommand{T.Thing, 2});
					B->Submit(colony::SetAlertCommand{T.Thing, 0});
					// Unarmed, she would only stand there once alerted: the sim's fight response sends her at you or away.
					B->Submit(colony::SetFightResponseCommand{T.Thing, T.Behaviour == EGunRangeBehaviour::FleeOnSight ? colony::FightResponse::FleeOnSight : colony::FightResponse::Fists});
					T.bGuard = true;
					++State->GuardsSet;
				}
				continue;
			}
			// Each one's own way, by the sim's own fight responses (docs/design/focus.md) so the sim runs and sprints her:
			// struck, she turns hostile (faction 2) and flees (FleeOnSight, so she keeps running while she sees you) or comes
			// at you with her fists (Fists); the on-sight runner turns and flees once you are within 20 m (or she is struck).
			const bool bUp = !View->downed && !View->dead;
			const bool bStruck = State->Attacked.Contains(T.Thing);
			const float ToPlayer = PlayerView ? FVector2D::Distance(FVector2D(View->x, View->y), FVector2D(PlayerView->x, PlayerView->y)) : 1e9f;
			const bool bSpotted = T.Behaviour == EGunRangeBehaviour::FleeOnSight && ToPlayer <= GunRangeSightMetres;
			if (bUp && !T.bTurned && (bSpotted || (bStruck && T.Behaviour != EGunRangeBehaviour::Stand)))
			{
				T.bTurned = true;
				T.TurnedDist = ToPlayer;
				B->Submit(colony::SetFactionCommand{T.Thing, 2});
				B->Submit(colony::SetFightResponseCommand{T.Thing,
					T.Behaviour == EGunRangeBehaviour::Fists ? colony::FightResponse::Fists : colony::FightResponse::FleeOnSight});
			}
			if (bUp && T.bTurned)
			{
				if (View->sprinting && !T.bSprinted)
				{
					T.bSprinted = true;
					++State->Sprinted;
				}
				// "Ran": a runner moving away from you (the sim's flee response puts her in a fall-back, not the map-leaving
				// `fleeing`); "charged": the brawler sprinting in.
				if (!T.bFleeing && T.Behaviour != EGunRangeBehaviour::Fists && PlayerView &&
					(View->fleeing || View->sprinting || ToPlayer > T.TurnedDist + 3.0f))
				{
					T.bFleeing = true;
					++State->Fled;
				}
				if (!T.bCharged && T.Behaviour == EGunRangeBehaviour::Fists && View->sprinting)
				{
					T.bCharged = true;
					++State->Charged;
				}
				if (View->meleeTarget == State->Player && View->meleePhase != colony::MeleePhase::Ready && !T.bAngry)
				{
					T.bAngry = true;
				}
				if (T.bAngry && View->meleeTarget == State->Player && View->meleePhase == colony::MeleePhase::Windup && State->Clock >= T.NextOrder)
				{
					++State->FistSwings;
					T.NextOrder = State->Clock + 0.5f;
				}
				continue;
			}
			// Walkers cross the field and back, a pause at each end.
			if (T.bWalker && !View->downed && !View->dead && State->Clock >= T.NextMove)
			{
				B->Submit(colony::MoveOrderCommand{T.Thing, T.bToB ? T.B : T.A});
				T.bToB = !T.bToB;
				T.NextMove = State->Clock + 8.0f + FMath::Frac(T.Seed * 0.0137f) * 4.0f;
			}
		}
		if (State->bGuards)
		{
			G->SetSceneLine(FString::Printf(TEXT("Guards: %d standing, %d down, %d dead   (group %d)\nUnaware, backs turned: crouch to creep up, walking is heard at 3 m.\nAlerted, most come at you with their fists, some run\nN: ordinary targets   U: new guards"), Standing, Downed, Dead, State->Group));
		}
		else
		{
			G->SetSceneLine(FString::Printf(TEXT("Targets: %d standing, %d down, %d dead   (group %d)\nEach pair: one stands, one flees on sight,\none flees when hit, one fights back with fists\nU: unaware guards to sneak up on"), Standing, Downed, Dead, State->Group));
		}

		// -GunRangeIdleRecord=<seconds>: a camera 3 m in front of a standing (not walking) target, every frame at a fixed
		// 30 a second, to check that a standing pawn stands still and breathes (docs/design/pawns.md "Standing still").
		// `ffmpeg -framerate 30 -i rec_%04d.png -pix_fmt yuv420p idle.mp4`
		float IdleRecordSeconds = 0.0f;
		if (FParse::Value(FCommandLine::Get(), TEXT("GunRangeIdleRecord="), IdleRecordSeconds) && State->Clock > 5.0f
			&& State->IdleRecorded < FMath::RoundToInt32(IdleRecordSeconds * 30.0f))
		{
			if (!State->IdleCamera.IsValid())
			{
				for (const FGunRangeTarget& T : State->Targets)
				{
					const ASimPawnActor* Actor = T.bWalker ? nullptr : Pawns->FindPawn(T.Thing);
					APlayerController* Viewer = UGameplayStatics::GetPlayerController(GetWorld(), 0);
					if (Actor && Viewer)
					{
						const FVector Feet = Actor->GetActorLocation();
						const FVector Ahead = Actor->GetActorForwardVector();
						const FVector Eye = Feet + Ahead * 300.0f + Actor->GetActorRightVector() * 60.0f + FVector(0.0, 0.0, 110.0);
						const FRotator Look = (Feet + FVector(0.0, 0.0, 85.0) - Eye).Rotation();
						State->IdleCamera = GetWorld()->SpawnActor<ACameraActor>(Eye, Look);
						Viewer->SetViewTarget(State->IdleCamera.Get());
						FApp::SetUseFixedTimeStep(true);
						FApp::SetFixedDeltaTime(1.0 / 30.0);
						Notes.Add(FString::Printf(TEXT("gunrange: idle record of %s, playing %s"), *Actor->GetName(),
							Actor->GetAnim() ? *Actor->GetAnim()->GetStats().PlayingAsset.ToString() : TEXT("?")));
						break;
					}
				}
			}
			if (State->IdleCamera.IsValid())
			{
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke/gunrange_idle") / FString::Printf(TEXT("rec_%04d.png"), State->IdleRecorded++), false, false);
			}
		}

		// K (N is sleep now): a new group now. By itself only after everyone has been down a full minute (or dead for 20 seconds), so the
		// downed can still be shot and the bodies looked at (sneed: downed is not dead).
		APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
		const bool bAllDown = !State->Targets.IsEmpty() && Standing == 0;
		State->AllDownFor = bAllDown ? State->AllDownFor + Dt : 0.0f;
		const float NewGroupAfter = Downed == 0 ? 20.0f : 60.0f;
		const bool bWantGuards = PC && PC->WasInputKeyJustPressed(EKeys::U);
		if (State->PendingSpawn == 0 && (bWantGuards || (PC && PC->WasInputKeyJustPressed(EKeys::K)) || State->AllDownFor > NewGroupAfter))
		{
			// U: unaware guards; K: ordinary targets (N is the sleep key); by itself: the same kind again.
			State->bGuards = bWantGuards || (State->bGuards && !(PC && PC->WasInputKeyJustPressed(EKeys::K)));
			SpawnGroup();
		}
		else if (State->PendingSpawn > 0)
		{
			// Wait for the spawn commands to be applied by a step.
			if (--State->PendingSpawn == 0)
			{
				AdoptGroup();
			}
		}

		// Tab, middle mouse and flicks (lock-on): the ordinary targets are the player's own side, which the sim's Next skips,
		// so the range lets them be picked (FocusCommand anySide; it used to pick them itself, Tab only).
		P->SetFocusAnySide(true);

		if (!bScript || !State->bArmed)
		{
			return;
		}
		// The tour: each gun from the hip, aimed, firing at the nearest standing woman, its cooldown bar; then down one.
		UFightSubsystem* Fight = GetWorld()->GetSubsystem<UFightSubsystem>();
		AProtagonistPawn* Body = P->GetBody();
		if (!Fight || !Body)
		{
			return;
		}
		const auto GunCountNow = [G]() { return G->GetGuns().Num(); };
		// --- Arms that can't bend wrong: both views, every frame a hand holds a weapon. ---
		{
			const auto Label = [&]() -> FString
			{
				const int32 Held = G->GetHeldRackIndex();
				if (Held >= 0 && Held < GunCountNow())
				{
					return G->GetGuns()[Held].Label;
				}
				if (Held >= GunCountNow() && Held - GunCountNow() < G->GetMelee().Num())
				{
					return G->GetMelee()[Held - GunCountNow()].Label;
				}
				return TEXT("other");
			};
			const auto CheckArms = [&](const USkeletalMeshComponent* Mesh, const FVector& Up, const FVector& Forward, const FVector& Right, const TCHAR* View)
			{
				const UMakeHumanAnimInstance* Anim = Mesh ? Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
				if (!Anim)
				{
					return;
				}
				for (const bool bRight : {true, false})
				{
					if (Anim->GetHandGoal(bRight).Alpha < 0.99f)
					{
						continue;
					}
					colony::arm::ArmAngles Solved, Drawn;
					float Short = 0.0f;
					const bool bSolved = Anim->GetSolvedArm(bRight, Solved, Short);
					const bool bDrawn = HoldDraw::MeasureDrawnArm(Mesh, bRight, Up, Forward, Right, Drawn);
					if (!bSolved || !bDrawn)
					{
						continue;
					}
					++State->ArmFrames;
					// The solver's own reading must be inside the limits; the drawn bones' too, with room for the
					// skeleton's rest offsets and the fingers' curl (the palm read from the knuckles).
					const bool bBad = !colony::arm::withinLimits(Solved, colony::arm::Limits{}, 0.5) || !colony::arm::withinLimits(Drawn, colony::arm::Limits{}, 15.0);
					State->ArmWorstTwist = FMath::Max(State->ArmWorstTwist, static_cast<float>(FMath::Max(FMath::Abs(Solved.forearmTwist), FMath::Abs(Drawn.forearmTwist))));
					State->ArmWorstWrist = FMath::Max(State->ArmWorstWrist, static_cast<float>(FMath::Max(FMath::Abs(Drawn.wristFlex), FMath::Abs(Drawn.wristDeviation))));
					State->ArmWorstElbow = FMath::Max(State->ArmWorstElbow, static_cast<float>(Drawn.elbowFlex));
					State->ArmWorstShort = FMath::Max(State->ArmWorstShort, Short);
					const FString Key = FString::Printf(TEXT("%s %s %s"), *Label(), View, bRight ? TEXT("right") : TEXT("left"));
					if (bBad)
					{
						++State->ArmFails;
						int32& N = State->ArmFailsBy.FindOrAdd(Key);
						if (N++ < 2)
						{
							Notes.Add(FString::Printf(TEXT("gunrange: ARM OUT OF RANGE %s: solved elbow %.0f twist %.0f wrist %.0f/%.0f; drawn elbow %.0f twist %.0f wrist %.0f/%.0f"),
								*Key, Solved.elbowFlex, Solved.forearmTwist, Solved.wristFlex, Solved.wristDeviation, Drawn.elbowFlex, Drawn.forearmTwist, Drawn.wristFlex,
								Drawn.wristDeviation));
						}
					}
					// The hand must be drawn on its goal: the solver's reach (after the shoulder came forward) and the
					// drawn hand bone against the goal it was given (catches a goal in the wrong frame or a later node
					// moving the hand). Either off by more than 3 cm fails the test.
					FTransform GoalWorld;
					float ShoulderReach = 0.0f;
					float Gap = 0.0f;
					FVector GoalFromShoulder = FVector::ZeroVector;
					if (Anim->GetSolvedGoal(bRight, GoalWorld, ShoulderReach))
					{
						const FVector HandAt = Mesh->GetSocketLocation(bRight ? TEXT("hand_r") : TEXT("hand_l"));
						Gap = static_cast<float>(FVector::Dist(HandAt, GoalWorld.GetLocation()));
						const FVector FromShoulder = GoalWorld.GetLocation() - Mesh->GetSocketLocation(bRight ? TEXT("upperarm_r") : TEXT("upperarm_l"));
						GoalFromShoulder = FVector(FVector::DotProduct(FromShoulder, Forward), FVector::DotProduct(FromShoulder, Right), FVector::DotProduct(FromShoulder, Up));
						// How far the drawn hand is turned from its grip's turn (the wrist limits held it back): not a
						// failure, a note for tuning the places and grips.
						const FQuat HandQ = Mesh->GetSocketQuaternion(bRight ? TEXT("hand_r") : TEXT("hand_l"));
						const float HandTurn = static_cast<float>(FMath::RadiansToDegrees(HandQ.AngularDistance(GoalWorld.GetRotation())));
						if (HandTurn > 25.0f)
						{
							int32& TurnNotes = State->ArmFailsBy.FindOrAdd(Key + TEXT(" turn"));
							if (TurnNotes++ < 1)
							{
								colony::arm::ArmAngles Asked;
								float CurlAdded = 0.0f;
								float ElbowSwing = 0.0f;
								Anim->GetWantedArm(bRight, Asked, CurlAdded, ElbowSwing);
								Notes.Add(FString::Printf(TEXT("gunrange: HAND TURNED FROM ITS GRIP %s: %.0f degrees (the wrist's limits held it back); the grip asked twist %.0f wrist %.0f/%.0f, got twist %.0f wrist %.0f/%.0f, elbow %.0f swung %.0f; fingers curled %.0f more"),
									*Key, HandTurn, Asked.forearmTwist, Asked.wristFlex, Asked.wristDeviation, Solved.forearmTwist, Solved.wristFlex, Solved.wristDeviation, Solved.elbowFlex, ElbowSwing, CurlAdded));
							}
						}
					}
					State->ArmWorstGap = FMath::Max(State->ArmWorstGap, Gap);
					if (Short > 3.0f || Gap > 3.0f)
					{
						++State->ArmShort;
						int32& N = State->ArmFailsBy.FindOrAdd(Key + TEXT(" short"));
						if (N++ < 2)
						{
							Notes.Add(FString::Printf(TEXT("gunrange: HAND OFF ITS GRIP %s: out of reach by %.1f cm, drawn %.1f cm from the goal, shoulder forward %.1f cm, goal from shoulder (ahead %.0f, right %.0f, up %.0f)"),
								*Key, Short, Gap, ShoulderReach, GoalFromShoulder.X, GoalFromShoulder.Y, GoalFromShoulder.Z));
						}
					}
				}
			};
			if (G->IsViewShown() && Body->IsFirstPerson())
			{
				if (const UCameraComponent* Cam = Body->GetCamera())
				{
					CheckArms(G->GetArmsMesh(), Cam->GetUpVector(), Cam->GetForwardVector(), Cam->GetRightVector(), TEXT("first person"));
				}
			}
			const USimPawnSubsystem* PawnsNow = GetWorld()->GetSubsystem<USimPawnSubsystem>();
			if (const ASimPawnActor* Me = PawnsNow ? PawnsNow->FindPawn(State->Player) : nullptr; Me && !Body->IsFirstPerson())
			{
				CheckArms(HoldDraw::VisibleMesh(*Me), Me->GetActorUpVector(), Me->GetActorForwardVector(), Me->GetActorRightVector(), TEXT("third person"));
			}
		}
		const auto Picture = [=, this](const TCHAR* What)
		{
			const FString Path = FPaths::ProjectSavedDir() / TEXT("GunRange") / FString::Printf(TEXT("%02d_%s.png"), State->Pictures++, What);
			FScreenshotRequest::RequestScreenshot(Path, true, false);  // with the UI: the scope view is drawn by it
			Notes.Add(FString::Printf(TEXT("gunrange: picture %s (gun %d, barrel off %.1f deg, sight off %.1f cm, shots %d, reloads %d)"),
				*FPaths::GetCleanFilename(Path), G->GetCurrentGun(), G->GetBarrelOffDegrees(), G->GetSightOffCm(), G->GetShotsSeen(), G->GetReloadsSeen()));
			Notes.Add(FString::Printf(TEXT("gunrange:   bones %s"), *G->GetBoneLine()));
			if (Body->IsFirstPerson() && !G->IsHeldViewShown())
			{
				Notes.Add(FString::Printf(TEXT("gunrange:   parts %s"), *G->DescribeGunParts()));
			}
		};
		const auto AimAtNearest = [=]() -> bool
		{
			float Best = 1e9f;
			FVector Where = FVector::ZeroVector;
			for (const FGunRangeTarget& T : State->Targets)
			{
				const colony::ThingView* View = B->GetCurrentSnapshot().find(T.Thing);
				FVector At;
				if (View && !View->downed && !View->dead && B->GetBlendedLocation(T.Thing, At))
				{
					const float D = FVector::Dist(At, Body->GetViewLocation());
					if (D < Best)
					{
						Best = D;
						Where = At;
					}
				}
			}
			if (Best > 1e8f)
			{
				return false;
			}
			const FRotator Wanted = (Where + FVector(0.0, 0.0, 120.0) - Body->GetViewLocation()).Rotation();
			Body->Look(FRotator::NormalizeAxis(Wanted.Yaw - Body->GetYaw()), Wanted.Pitch - Body->GetPitch());
			return true;
		};
		State->StepTime += Dt;
		const int32 GunCount = G->GetGuns().Num();
		const auto Next = [=](int32 Step) { State->Step = Step; State->StepTime = 0.0f; State->bFiringPicture = false; };
		switch (State->Step)
		{
			case 0:  // pick the gun, look at the field
				G->SelectGun(State->Gun);
				AimAtNearest();
				if (State->StepTime > 2.5f)
				{
					Picture(*FString::Printf(TEXT("gun%d_hip"), State->Gun + 1));
					Next(1);
				}
				break;
			case 1:  // aim down the sights
				Body->SetScriptedAim(true);
				AimAtNearest();
				// Half way up (a scope coming to the eye) and then fully aimed.
				if (!State->bFiringPicture && Body->GetAimBlend() > 0.5f)
				{
					Picture(*FString::Printf(TEXT("gun%d_raising"), State->Gun + 1));
					State->bFiringPicture = true;
				}
				if (State->StepTime > 1.2f)
				{
					State->bFiringPicture = false;
					Picture(*FString::Printf(TEXT("gun%d_aimed"), State->Gun + 1));
					State->Shots = G->GetShotsSeen();
					State->HitClipsAtStep = Fight->GetHitClips();
					State->HitSeenAt = -1.0f;
					State->bHitPicture = false;
					Next(2);
				}
				break;
			case 2:  // fire a few
				AimAtNearest();
				Fight->Fire();
				// The first shot seen: a picture right away (the kick and the flash).
				if (!State->bFiringPicture && G->GetShotsSeen() > State->Shots)
				{
					Picture(*FString::Printf(TEXT("gun%d_firing"), State->Gun + 1));
					State->bFiringPicture = true;
				}
				if (State->HitSeenAt < 0.0f && Fight->GetHitClips() > State->HitClipsAtStep)
				{
					State->HitSeenAt = State->StepTime;
				}
				if (!State->bHitPicture && State->HitSeenAt >= 0.0f && State->StepTime > State->HitSeenAt + 0.25f)
				{
					Picture(*FString::Printf(TEXT("gun%d_hit_react"), State->Gun + 1));
					State->bHitPicture = true;
				}
				if (State->StepTime > 2.0f)
				{
					Body->SetScriptedAim(false);
					Notes.Add(FString::Printf(TEXT("gunrange: gun %d fired %d shots in 2 s (no reloads: bursts and a cooldown)"), State->Gun + 1,
						G->GetShotsSeen() - State->Shots));
					Next(3);
				}
				break;
			case 3:  // the cooldown: the thin yellow refill bar under the gun's name (no reloads, sneed 2026-11-17)
				if (State->StepTime > 0.4f && State->Shots >= 0)
				{
					Picture(*FString::Printf(TEXT("gun%d_cooldown"), State->Gun + 1));
					State->Shots = -1;
				}
				if (State->StepTime > 1.5f)
				{
					++State->Gun;
					Next(State->Gun < GunCount ? 0 : 4);
				}
				break;
			case 4:  // shoot until someone is down, then a picture of her
				G->SelectGun(1 % FMath::Max(GunCount, 1));
				Body->SetScriptedAim(true);
				if (!AimAtNearest() || State->StepTime > 12.0f)
				{
					Body->SetScriptedAim(false);
					Picture(TEXT("downed"));
					Next(5);
					break;
				}
				Fight->Fire();
				if (Downed + Dead > 0 && State->StepTime > 1.0f)
				{
					Body->SetScriptedAim(false);
					Next(6);
				}
				break;
			case 6:  // look at the fallen one
				if (State->StepTime > 1.5f)
				{
					for (const FGunRangeTarget& T : State->Targets)
					{
						const colony::ThingView* View = B->GetCurrentSnapshot().find(T.Thing);
						FVector At;
						if (View && (View->downed || View->dead) && B->GetBlendedLocation(T.Thing, At))
						{
							const FRotator Wanted = (At + FVector(0.0, 0.0, 20.0) - Body->GetViewLocation()).Rotation();
							Body->Look(FRotator::NormalizeAxis(Wanted.Yaw - Body->GetYaw()), Wanted.Pitch - Body->GetPitch());
							break;
						}
					}
				}
				if (State->StepTime > 2.5f)
				{
					Picture(TEXT("downed"));
					Next(8);
				}
				break;
			case 8:  // shoot the one lying down: blood and a wound through the body trace (SpawnBodyThroughHit)
				for (const FGunRangeTarget& T : State->Targets)
				{
					const colony::ThingView* View = B->GetCurrentSnapshot().find(T.Thing);
					FVector At;
					if (View && (View->downed || View->dead) && B->GetBlendedLocation(T.Thing, At))
					{
						const FRotator Wanted = (At + FVector(0.0, 0.0, 15.0) - Body->GetViewLocation()).Rotation();
						Body->Look(FRotator::NormalizeAxis(Wanted.Yaw - Body->GetYaw()), Wanted.Pitch - Body->GetPitch());
						break;
					}
				}
				Body->SetScriptedAim(true);
				if (State->StepTime > 0.8f && State->StepTime < 1.4f)
				{
					Fight->Fire();
				}
				if (State->StepTime > 1.7f)
				{
					Picture(TEXT("downed_shot"));
					Body->SetScriptedAim(false);
					Next(12);
				}
				break;
			case 12:  // a close look at her wounds through the rifle's scope (the far pictures are too small to show them):
				      // at the place the last bullet met her drawn body
				G->SelectGun(0);
				for (const FGunRangeTarget& T : State->Targets)
				{
					const colony::ThingView* View = B->GetCurrentSnapshot().find(T.Thing);
					FVector At;
					if (View && (View->downed || View->dead) && Fight->GetLastBodyHit(T.Thing, At))
					{
						if (!State->bFiringPicture)
						{
							State->bFiringPicture = true;
							Notes.Add(FString::Printf(TEXT("gunrange: close look at %u's last wound, %.0f m away"), entt::to_integral(T.Thing),
								FVector::Distance(At, Body->GetViewLocation()) / 100.0f));
						}
						const FRotator Wanted = (At - Body->GetViewLocation()).Rotation();
						Body->Look(FRotator::NormalizeAxis(Wanted.Yaw - Body->GetYaw()), Wanted.Pitch - Body->GetPitch());
						break;
					}
				}
				// The blood seen from where the shooter stands, without the scope (sneed: it must read from afar too).
				if (State->StepTime > 1.2f && State->StepTime - Dt <= 1.2f)
				{
					Picture(TEXT("downed_far"));
				}
				Body->SetScriptedAim(State->StepTime > 1.5f);
				if (State->StepTime > 3.5f)
				{
					Picture(TEXT("downed_scope"));
					Body->SetScriptedAim(false);
					Next(5);
				}
				break;
			case 5:  // third person (the V key): the body holding the gun, then aimed over the shoulder
				if (Body->IsFirstPerson())
				{
					Body->ToggleView();
					Body->SnapCamera();
				}
				AimAtNearest();
				if (State->StepTime > 2.0f && !State->bFiringPicture)
				{
					Picture(TEXT("third_person"));
					State->bFiringPicture = true;
				}
				Body->SetScriptedAim(State->StepTime > 2.0f);
				if (State->StepTime > 3.5f)
				{
					Picture(TEXT("third_person_aimed"));
					Body->SetScriptedAim(false);
					Next(G->GetMelee().Num() > 0 ? 7 : 10);
				}
				break;
			case 7:  // the combat range: back to first person with a gun, then a melee weapon from the rack
				G->SelectGun(0);
				if (!Body->IsFirstPerson())
				{
					Body->ToggleView();
					Body->SnapCamera();
				}
				if (State->StepTime > 1.0f)
				{
					Next(9);
				}
				break;
			case 9:  // a melee weapon from the rack: first person stays on, the arms hold it and swing it
				G->SelectGun(GunCount + FMath::Min(4, G->GetMelee().Num() - 1));
				if (!Body->IsFirstPerson())
				{
					Body->ToggleView();
					Body->SnapCamera();
				}
				AimAtNearest();
				if (State->StepTime > 1.5f && !State->bFiringPicture)
				{
					Notes.Add(FString::Printf(TEXT("gunrange: melee rack %d in hand, view %s, arms %s"), G->GetHeldRackIndex(),
						Body->IsFirstPerson() ? TEXT("first person") : TEXT("third person (should stay first)"), G->IsHeldViewShown() ? TEXT("shown") : TEXT("NOT shown")));
					Picture(TEXT("melee_held"));
					State->bFiringPicture = true;
					if (UWeaponDrawSubsystem* W = GetWorld()->GetSubsystem<UWeaponDrawSubsystem>())
					{
						W->Swing();
					}
				}
				if (State->StepTime > 2.0f && !State->bSwingPicture)
				{
					State->bSwingPicture = true;
					Picture(TEXT("melee_swing"));
				}
				if (State->StepTime > 3.5f)
				{
					Notes.Add(FString::Printf(TEXT("gunrange: first-person swings seen %d"), G->GetHeldSwingsSeen()));
					State->MeleeTour = 0;
					State->bSwingPicture = false;
					Next(15);
				}
				break;
			case 15:  // each melee weapon in the hand: a first-person and a third-person picture (is it in the palm, pointing right?)
			{
				const TArray<FGunRangeMelee>& Rack = G->GetMelee();
				if (State->StepTime < Dt * 1.5f)
				{
					G->SelectGun(GunCount + State->MeleeTour);
					if (!Body->IsFirstPerson())
					{
						Body->ToggleView();
						Body->SnapCamera();
					}
				}
				AimAtNearest();
				if (State->StepTime > 2.2f && !State->bFiringPicture)  // after the view change back has settled (no blur)
				{
					State->bFiringPicture = true;
					Picture(*FString::Printf(TEXT("melee_%s_fp"), *Rack[State->MeleeTour].Label));
					Body->ToggleView();
					Body->SnapCamera();
				}
				if (State->StepTime > 3.8f && !State->bSwingPicture)
				{
					State->bSwingPicture = true;
					Picture(*FString::Printf(TEXT("melee_%s_3p"), *Rack[State->MeleeTour].Label));
				}
				if (State->StepTime > 4.0f)
				{
					if (!Body->IsFirstPerson())
					{
						Body->ToggleView();
						Body->SnapCamera();
					}
					State->bSwingPicture = false;
					if (++State->MeleeTour < Rack.Num())
					{
						Next(15);
					}
					else
					{
						State->GunTour = 0;
						Next(16);
					}
				}
				break;
			}
			case 16:  // each gun held: first person from the hip and aimed, third person ready and aimed (hands on the grips?)
			{
				if (State->StepTime < Dt * 1.5f)
				{
					G->SelectGun(State->GunTour);
					if (!Body->IsFirstPerson())
					{
						Body->ToggleView();
						Body->SnapCamera();
					}
				}
				AimAtNearest();
				const FString Name = G->GetGuns().IsValidIndex(State->GunTour) ? G->GetGuns()[State->GunTour].Label.Replace(TEXT(" "), TEXT("")) : TEXT("gun");
				const float T = State->StepTime;
				const auto At = [T, Dt](float When) { return T >= When && T - Dt < When; };
				Body->SetScriptedAim((T > 2.2f && T < 3.4f) || T > 6.0f);
				if (At(2.0f))
				{
					Picture(*FString::Printf(TEXT("hold_%s_fp_hip"), *Name));
				}
				if (At(3.2f))
				{
					Picture(*FString::Printf(TEXT("hold_%s_fp_aimed"), *Name));
				}
				if (At(3.6f))
				{
					Body->ToggleView();
					Body->SnapCamera();
				}
				if (At(5.6f))
				{
					Picture(*FString::Printf(TEXT("hold_%s_3p_ready"), *Name));
				}
				if (At(7.4f))
				{
					// Long hair while aimed (docs/design/hair-physics.md, "Aiming"): over the other shoulder, off the stock.
					const USimPawnSubsystem* HairPawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
					const ASimPawnActor* HairMe = HairPawns ? HairPawns->FindPawn(State->Player) : nullptr;
					const USkeletalMeshComponent* HairMesh = HairMe ? HoldDraw::VisibleMesh(*HairMe) : nullptr;
					const UMakeHumanAnimInstance* HairAnim = HairMesh ? Cast<UMakeHumanAnimInstance>(HairMesh->GetAnimInstance()) : nullptr;
					Notes.Add(FString::Printf(TEXT("gunrange: HAIR AIM %s 3p aimed: %s"), *Name, HairAnim ? *HairAnim->DescribeHairAim() : TEXT("no MakeHuman body")));
					Picture(*FString::Printf(TEXT("hold_%s_3p_aimed"), *Name));
				}
				if (T > 7.6f)
				{
					Body->SetScriptedAim(false);
					if (!Body->IsFirstPerson())
					{
						Body->ToggleView();
						Body->SnapCamera();
					}
					if (++State->GunTour < G->GetGuns().Num())
					{
						Next(16);
					}
					else
					{
						G->SelectGun(0);
						Next(11);
					}
				}
				break;
			}
			case 11:  // a gun again: its model in the hands
				if (State->StepTime > 2.0f)
				{
					Notes.Add(FString::Printf(TEXT("gunrange: gun back in hand, view %s, gun model %s"), Body->IsFirstPerson() ? TEXT("first person") : TEXT("third person"),
						G->IsViewShown() && !G->IsHeldViewShown() ? TEXT("shown") : TEXT("NOT shown")));
					Picture(TEXT("gun_again"));
					Next(13);
				}
				break;
			case 13:  // the kill feed at the top left: sample lines (one of them yours, with its sound), then a picture
				if (UKillFeedSubsystem* Feed = GetWorld()->GetSubsystem<UKillFeedSubsystem>(); Feed && !State->bKillFeedLines)
				{
					State->bKillFeedLines = true;
					Feed->AddLine(TEXT("You"), 0, TEXT("Raider"), 1, true);
					Feed->AddLine(TEXT("Raider"), 1, TEXT("Bo"), 0, false);
					Feed->AddLine(FString(), 0, TEXT("Deer"), 2, false);
				}
				if (State->StepTime > 1.0f)
				{
					Picture(TEXT("kill_feed"));
					Next(14);
				}
				break;
			case 14:  // Tab: lock on to the nearest woman; the HP bar top middle and the marker on her
				if (State->StepTime < Dt * 1.5f)
				{
					AimAtNearest();
					P->FocusNext();
				}
				if (State->StepTime > 1.5f && !State->bFlickTried)
				{
					Notes.Add(FString::Printf(TEXT("gunrange: Tab focus on %u (%s), HP bar frames %d"), entt::to_integral(P->GetFocusTarget()),
						P->GetFocusTarget() != colony::kNoEntity ? TEXT("locked on") : TEXT("NOTHING FOCUSED"), P->GetFocusBarFramesDrawn()));
					Picture(TEXT("focus_hpbar"));
					// A mouse flick (lock-on.md): to the right, else (nobody there) to the left.
					State->bFlickTried = true;
					State->FlickFrom = P->GetFocusTarget();
					P->FlickFocus(true);
				}
				if (State->StepTime > 2.0f && State->FlickFrom != colony::kNoEntity && !State->bFlickLeftTried && P->GetFocusTarget() == State->FlickFrom)
				{
					State->bFlickLeftTried = true;
					P->FlickFocus(false);
				}
				if (State->StepTime > 2.6f)
				{
					const bool bSwitched = State->FlickFrom != colony::kNoEntity && P->GetFocusTarget() != State->FlickFrom;
					// Who a flick could have gone to: the range's living women within Tab's 20 m, other than the one held
					// (by this point of the tour most may be dead; then there is nothing to test).
					int32 Beside = 0;
					const colony::Snapshot& Now = B->GetCurrentSnapshot();
					const colony::ThingView* MeNow = Now.find(State->Player);
					for (const FGunRangeTarget& T : State->Targets)
					{
						const colony::ThingView* View = Now.find(T.Thing);
						if (MeNow && View && !View->dead && T.Thing != State->FlickFrom &&
							FVector2D::Distance(FVector2D(View->x, View->y), FVector2D(MeNow->x, MeNow->y)) < 19.5f)
						{
							++Beside;
						}
					}
					Notes.Add(FString::Printf(TEXT("gunrange: flick from %u to %u (%s), %d flicks, %d living targets within 20 m besides the one held"),
						entt::to_integral(State->FlickFrom), entt::to_integral(P->GetFocusTarget()), bSwitched ? TEXT("switched") : TEXT("DID NOT SWITCH"),
						P->GetFlicksSent(), Beside));
					if (State->FlickFrom == colony::kNoEntity)
					{
						Fail(TEXT("gunrange: Tab locked on to nobody, so the mouse flick could not be tried"));
					}
					else if (!bSwitched && Beside > 0)
					{
						Fail(TEXT("gunrange: a mouse flick did not switch the lock-on target"));
					}
					else if (!bSwitched)
					{
						Notes.Add(TEXT("gunrange: flick not tested: nobody alive near enough beside the lock"));
					}
					Picture(TEXT("focus_flick"));
					State->FlickFrom = colony::kNoEntity;
					P->EndFocus();
					Next(10);
				}
				break;
			default:
				break;
		}
	};
	Play.Verify = [=, this]()
	{
		if (UGunRangeSubsystem* G = Range())
		{
			PhaseNumbers.Add(TEXT("shots"), G->GetShotsSeen());
			PhaseNumbers.Add(TEXT("reloads"), G->GetReloadsSeen());
			for (int32 i = 0; i < G->GetGuns().Num(); ++i)
			{
				Notes.Add(FString::Printf(TEXT("gunrange: %s: %s"), *G->GetGuns()[i].Label, G->HasModel(i) ? TEXT("model") : TEXT("no model (box)")));
			}
			for (const FGunRangeMelee& M : G->GetMelee())
			{
				Notes.Add(FString::Printf(TEXT("gunrange: %s (melee): %s"), *M.Label, G->FindMeleeMesh(M.Def) ? TEXT("model") : TEXT("no model (placeholder)")));
			}
		}
		if (UFightSubsystem* Fight = GetWorld()->GetSubsystem<UFightSubsystem>())
		{
			PhaseNumbers.Add(TEXT("hit_clips"), Fight->GetHitClips());
			PhaseNumbers.Add(TEXT("pawn_hits"), Fight->GetPawnHitsSeen());
			PhaseNumbers.Add(TEXT("hits_on_drawn_body"), Fight->GetSnappedHits());
			PhaseNumbers.Add(TEXT("old_body_decals"), Fight->GetBodyDecals());
		}
		if (UBloodSubsystem* Blood = GetWorld()->GetSubsystem<UBloodSubsystem>())
		{
			Notes.Add(FString::Printf(TEXT("gunrange: blood %s: %d hits, %d drops, %d mist, %d wound marks, %d body stains, %d ground splats, %d painted into skin (%d bodies now)"),
				Blood->IsReady() ? TEXT("assets in") : TEXT("ASSETS MISSING (old simple blood)"), Blood->GetHits(), Blood->GetDropsSpawned(),
				Blood->GetMistSpawned(), Blood->GetWoundMarks(), Blood->GetBodyStainsMade(), Blood->GetSplatsMade(), Blood->GetSkinPaints(), Blood->GetSkinMasks()));
		}
		PhaseNumbers.Add(TEXT("arm_frames_checked"), State->ArmFrames);
		PhaseNumbers.Add(TEXT("arm_limit_fails"), State->ArmFails);
		PhaseNumbers.Add(TEXT("arm_hand_short"), State->ArmShort);
		Notes.Add(FString::Printf(TEXT("gunrange: arms checked on %d hand-frames: %d outside human limits, %d off the grip; worst twist %.0f, wrist %.0f, elbow %.0f deg, out of reach %.1f cm, drawn off the goal %.1f cm"),
			State->ArmFrames, State->ArmFails, State->ArmShort, State->ArmWorstTwist, State->ArmWorstWrist, State->ArmWorstElbow, State->ArmWorstShort, State->ArmWorstGap));
		// A few frames off at a switch or a clip's blend are allowed (under 1%); more is a hold that doesn't fit the body.
		if (State->ArmFrames > 0 && State->ArmShort * 100 > State->ArmFrames)
		{
			Fail(FString::Printf(TEXT("gunrange: %d of %d hand-frames with a hand off its grip (see the HAND OFF ITS GRIP notes)"), State->ArmShort, State->ArmFrames));
		}
		if (State->ArmFrames > 0 && State->ArmFails > 0)
		{
			Fail(FString::Printf(TEXT("gunrange: %d hand-frames with an arm outside human joint limits (see the ARM OUT OF RANGE notes)"), State->ArmFails));
		}
		Notes.Add(FString::Printf(TEXT("gunrange: %d targets ran, %d sprinted, %d charged you, %d punches thrown at you, %d lock-on flicks"), State->Fled, State->Sprinted, State->Charged, State->FistSwings, Prot() ? Prot()->GetFlicksSent() : 0));
		if (const UFightSubsystem* Marks = GetWorld()->GetSubsystem<UFightSubsystem>())
		{
			Notes.Add(FString::Printf(TEXT("gunrange: %d guards set unaware, %d alert events, %d alert marks drawn"), State->GuardsSet, Marks->GetAlertEventsSeen(), Marks->GetAlertMarksDrawn()));
		}
		if (UKillFeedSubsystem* Feed = GetWorld()->GetSubsystem<UKillFeedSubsystem>())
		{
			PhaseNumbers.Add(TEXT("killfeed_lines"), Feed->GetLinesShown());
			PhaseNumbers.Add(TEXT("killfeed_mine"), Feed->GetPersonalKills());
			PhaseNumbers.Add(TEXT("killfeed_frames"), Feed->GetFramesDrawn());
		}
		if (USoundSubsystem* S = GetWorld()->GetSubsystem<USoundSubsystem>())
		{
			PhaseNumbers.Add(TEXT("voice_femhit"), S->GetPlayCount(TEXT("Voice_FemHit")));
			PhaseNumbers.Add(TEXT("voice_femscream"), S->GetPlayCount(TEXT("Voice_FemScream")));
			PhaseNumbers.Add(TEXT("voices_cut"), S->GetVoicesCut());
			PhaseNumbers.Add(TEXT("voices_skipped"), S->GetVoicesSkipped());
		}
	};
	Phases.Add(MoveTemp(Play));
}
