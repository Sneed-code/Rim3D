// The gun range (docs/design/gun-range.md): a first-person scene to play, not a check. The player's colonist (a random
// creator woman) stands at one end of a cleared flat field with the guns of assets/guns.json in a rack; random creator
// women stand and walk at 8 to 30 m. The sim does all the shooting, wounds, downing and dying; UGunRangeSubsystem draws
// the arms and guns. N (or downing them all) brings a new group. -GunRangeScript runs a short tour by itself and saves
// pictures to game/Saved/GunRange/ (each gun from the hip, aimed, firing and reloading, then a downed target).
// A ninth woman, the partner, stands at arm's length ahead and to the side to try the free hand on (H,
// docs/design/free-hand.md); -FreeHandScript runs a short free hand tour instead of the gun tour and checks it.

#include "SmokeTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#include "CreatorPeople.h"
#include "FightSubsystem.h"
#include "FreeHandSubsystem.h"
#include "GunRangeSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "SoftContactSubsystem.h"
#include "SoundSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

namespace
{
	const FVector GunRangeOrigin(-1695000.0, 1300000.0, 0.0);
	constexpr int32 GunRangeTargets = 8;

	struct FGunRangeTarget
	{
		colony::Entity Thing = colony::kNoEntity;
		int32 Seed = 0;
		TWeakObjectPtr<USkeletalMeshComponent> AppliedTo;
		bool bWalker = false;
		colony::CellPos A;
		colony::CellPos B;
		bool bToB = true;
		float NextMove = 0.0f;
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
		int32 PendingSpawn = 0;       // frames until the spawned pawns are looked for
		int32 Group = 0;
		float AllDownFor = 0.0f;
		float Clock = 0.0f;
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
		// The partner for the free hand, and the free hand tour.
		colony::Entity Partner = colony::kNoEntity;
		int32 PartnerSeed = 0;
		TWeakObjectPtr<USkeletalMeshComponent> PartnerAppliedTo;
		int32 HandStep = 0;
		float HandTime = 0.0f;
		int32 MostSquish = 0;
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
	const bool bHandScript = FParse::Param(FCommandLine::Get(), TEXT("FreeHandScript"));
	const bool bScript = FParse::Param(FCommandLine::Get(), TEXT("GunRangeScript")) && !bHandScript;
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
			// The far half walk across the field and back; the near half stand.
			T.bWalker = i >= GunRangeTargets / 2;
			const colony::CellPos Here{FMath::FloorToInt32(View.x), FMath::FloorToInt32(View.y), 0};
			T.A = colony::CellPos{Here.x, State->Stand.y - 9, 0};
			T.B = colony::CellPos{Here.x, State->Stand.y + 9, 0};
			T.NextMove = State->Clock + 1.0f + i * 0.7f;
			State->Targets.Add(T);
		}
		B->StepOnce();
		for (const FGunRangeTarget& T : State->Targets)
		{
			B->Submit(colony::DraftCommand{T.Thing, true});
		}
		Notes.Add(FString::Printf(TEXT("gunrange: group %d, %d targets"), State->Group, State->Targets.Num()));
	};

	FPhase Play;
	Play.Name = TEXT("gunrange");
	Play.WarmupSeconds = 6.0f;
	float Minutes = bScript ? 2.0f : (bHandScript ? 0.75f : 240.0f);
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
		// The partner: a random creator woman at arm's length, ahead and to one side (out of the line of fire), standing.
		{
			TSet<colony::Entity> Before;
			for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
			{
				Before.Add(View.id);
			}
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{State->Stand.x + 1, State->Stand.y + 1, 0}});
			B->StepOnce();
			for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
			{
				if (View.category == colony::ThingCategory::Pawn && !Before.Contains(View.id))
				{
					State->Partner = View.id;
					State->PartnerSeed = BaseSeed * 4441 + 99;
					const colony::DefDatabase* Defs = B->GetDefs().get();
					const FSimPawnLooks From = View.looks >= 0 ? USimPawnSubsystem::MakeLooks(B->GetCurrentSnapshot().looks[static_cast<size_t>(View.looks)], Defs) : FSimPawnLooks();
					Pawns->SetLooksOverride(View.id, CreatorPeople::RandomLooks(State->PartnerSeed, true, From));
					GunRangeStrip(*Sim, *B, View.id);
					B->Submit(colony::SetFactionCommand{View.id, 1});
					break;
				}
			}
			if (State->Partner != colony::kNoEntity)
			{
				B->StepOnce();
				B->Submit(colony::DraftCommand{State->Partner, true});
			}
			Notes.Add(FString::Printf(TEXT("gunrange: partner %s"), State->Partner != colony::kNoEntity ? TEXT("standing at arm's length") : TEXT("did not spawn")));
		}
		B->SetSpeed(1);
	};

	// -FreeHandScript: walk up to the partner in third person, free the hand, rest it on her cheek, hold, then her belly,
	// then the same in first person, then hands back; pictures to game/Saved/GunRange/hand_*.png.
	const auto RunFreeHandTour = [=, this]()
	{
		USimPawnSubsystem* Pawns = PawnsOf();
		UProtagonistSubsystem* P = Prot();
		UFreeHandSubsystem* Hand = GetWorld()->GetSubsystem<UFreeHandSubsystem>();
		USoftContactSubsystem* Soft = GetWorld()->GetSubsystem<USoftContactSubsystem>();
		AProtagonistPawn* Body = P ? P->GetBody() : nullptr;
		ASimPawnActor* Partner = Pawns && State->Partner != colony::kNoEntity ? Pawns->FindPawn(State->Partner) : nullptr;
		ASimPawnActor* Me = Pawns ? Pawns->FindPawn(State->Player) : nullptr;
		APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
		USkeletalMeshComponent* Mesh = Partner ? (Partner->GetMakeHumanMesh() ? Partner->GetMakeHumanMesh() : Partner->GetBody()) : nullptr;
		if (!Hand || !Body || !Me || !PC || !Mesh)
		{
			if (State->HandStep == 0 && PhaseTime > 20.0f)
			{
				Fail(TEXT("freehand: no partner, free hand or player body to try it with"));
				State->HandStep = 99;
			}
			return;
		}
		State->HandTime += GetWorld()->GetDeltaSeconds();
		if (Soft)
		{
			State->MostSquish = FMath::Max(State->MostSquish, Soft->GetContactsLastFrame());
		}
		const auto Picture = [=, this](const TCHAR* What)
		{
			const FString Path = FPaths::ProjectSavedDir() / TEXT("GunRange") / FString::Printf(TEXT("hand_%02d_%s.png"), State->Pictures++, What);
			FScreenshotRequest::RequestScreenshot(Path, true, false);
			const FFreeHandStats& S = Hand->GetStats();
			Notes.Add(FString::Printf(TEXT("freehand: picture %s: state %d, wrist off %.1f cm (worst %.1f), touch frames %d, hold frames %d, kicks %d, squish contacts %d"),
				*FPaths::GetCleanFilename(Path), static_cast<int32>(Hand->GetState()), S.WristError, S.WorstWristError, S.TouchFrames, S.HoldBodyFrames, S.Kicks,
				State->MostSquish));
		};
		const auto MarkerAt = [PC](const FVector& World, FVector2D& Out)
		{
			FVector2D Pixel;
			int32 X = 0, Y = 0;
			PC->GetViewportSize(X, Y);
			if (X <= 0 || Y <= 0 || !PC->ProjectWorldLocationToScreen(World, Pixel, false))
			{
				return false;
			}
			Out = FVector2D(Pixel.X / X * 2.0 - 1.0, 1.0 - Pixel.Y / Y * 2.0);
			return true;
		};
		const auto LookAt = [Body](const FVector& World)
		{
			const FRotator Wanted = (World - Body->GetViewLocation()).Rotation();
			Body->Look(FRotator::NormalizeAxis(Wanted.Yaw - Body->GetYaw()), Wanted.Pitch - Body->GetPitch());
		};
		const auto BoneAt = [Mesh](const TCHAR* Name) { return Mesh->GetBoneIndex(Name) != INDEX_NONE ? Mesh->GetSocketLocation(Name) : Mesh->GetComponentLocation(); };
		const FVector ToMe = (Me->GetActorLocation() - Partner->GetActorLocation()).GetSafeNormal2D();
		const FVector HeadUp = (BoneAt(TEXT("head")) - BoneAt(TEXT("neck_01"))).GetSafeNormal();
		const FVector Cheek = BoneAt(TEXT("head")) + HeadUp * 6.0f + ToMe * 9.0f;
		const FVector Belly = BoneAt(TEXT("spine_02")) + ToMe * 16.0f;
		const auto Next = [=](int32 Step)
		{
			State->HandStep = Step;
			State->HandTime = 0.0f;
		};
		const auto Aim = [&](const FVector& At, bool bGrip)
		{
			FVector2D M;
			if (MarkerAt(At, M))
			{
				Hand->SetScripted(M, bGrip);
			}
		};
		const float Apart = static_cast<float>(FVector::Dist2D(Me->GetActorLocation(), Partner->GetActorLocation()));
		switch (State->HandStep)
		{
		case 0:  // third person, walk up to her
			Body->SetFirstPerson(false);
			LookAt(BoneAt(TEXT("spine_03")));
			Body->SetScriptedInput(0.0, Apart > 72.0f ? 1.0 : 0.0, true);
			if (Apart <= 72.0f || State->HandTime > 6.0f)
			{
				Body->SetScriptedInput(0.0, 0.0, true);
				Notes.Add(FString::Printf(TEXT("freehand: %.0f cm from the partner"), Apart));
				Next(1);
			}
			break;
		case 1:  // the hand comes free (the gun goes to its holster)
			LookAt(BoneAt(TEXT("spine_03")));
			if (State->HandTime > 0.5f)
			{
				Hand->SetScripted(FVector2D(0.15, -0.2), false);
				Hand->SetActive(true);
				if (!Hand->IsOn())
				{
					Fail(TEXT("freehand: the free hand did not come on in direct control"));
				}
				Next(2);
			}
			break;
		case 2:
			if (State->HandTime > 1.5f)
			{
				Picture(TEXT("air"));
				Next(3);
			}
			break;
		case 3:  // the palm on her cheek
			Aim(Cheek, false);
			if (State->HandTime > 1.5f)
			{
				Picture(TEXT("touch_cheek"));
				Next(4);
			}
			break;
		case 4:  // hold it
			Aim(Cheek, true);
			if (State->HandTime > 1.5f)
			{
				Picture(TEXT("hold_cheek"));
				Next(5);
			}
			break;
		case 5:  // let go, the belly
			Aim(Belly, false);
			if (State->HandTime > 1.2f)
			{
				Picture(TEXT("touch_belly"));
				Next(6);
			}
			break;
		case 6:
			Aim(Belly, true);
			if (State->HandTime > 1.2f)
			{
				Picture(TEXT("hold_belly"));
				Next(7);
			}
			break;
		case 7:  // first person: the arm seen from the eyes, the head hidden
			Body->SetFirstPerson(true);
			if (State->HandTime < 0.3f)
			{
				LookAt(Belly);
			}
			Aim(Belly, false);
			if (State->HandTime > 1.8f)
			{
				Picture(TEXT("first_person"));
				Next(8);
			}
			break;
		case 8:  // hands back
			if (State->HandTime < 0.1f)
			{
				Hand->SetActive(false);
				Hand->ClearScripted();
			}
			if (State->HandTime > 1.5f)
			{
				Picture(TEXT("off"));
				const FFreeHandStats& S = Hand->GetStats();
				if (S.TouchFrames == 0)
				{
					Fail(TEXT("freehand: the hand never rested on the partner"));
				}
				if (S.HoldBodyFrames == 0)
				{
					Fail(TEXT("freehand: gripping never held on to the partner"));
				}
				if (S.Kicks == 0)
				{
					Notes.Add(TEXT("freehand: no soft-part kicks (not a MakeHuman partner?)"));
				}
				if (State->MostSquish == 0)
				{
					Notes.Add(TEXT("freehand: the palm never squished her skin (no T_MH_SoftContacts?)"));
				}
				Next(9);
			}
			break;
		default:
			break;
		}
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
		if (ASimPawnActor* Actor = State->Partner != colony::kNoEntity ? Pawns->FindPawn(State->Partner) : nullptr;
			Actor && Actor->GetMakeHumanMesh() && State->PartnerAppliedTo.Get() != Actor->GetMakeHumanMesh())
		{
			if (CreatorPeople::ApplyRandom(State->PartnerSeed, true, *Actor))
			{
				State->PartnerAppliedTo = Actor->GetMakeHumanMesh();
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
			// Walkers cross the field and back, a pause at each end.
			if (T.bWalker && !View->downed && !View->dead && State->Clock >= T.NextMove)
			{
				B->Submit(colony::MoveOrderCommand{T.Thing, T.bToB ? T.B : T.A});
				T.bToB = !T.bToB;
				T.NextMove = State->Clock + 8.0f + FMath::Frac(T.Seed * 0.0137f) * 4.0f;
			}
		}
		G->SetSceneLine(FString::Printf(TEXT("Targets: %d standing, %d down, %d dead   (group %d)"), Standing, Downed, Dead, State->Group));

		// N: a new group now. By itself only after everyone has been down a full minute (or dead for 20 seconds), so the
		// downed can still be shot and the bodies looked at (sneed: downed is not dead).
		APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
		const bool bAllDown = !State->Targets.IsEmpty() && Standing == 0;
		State->AllDownFor = bAllDown ? State->AllDownFor + Dt : 0.0f;
		const float NewGroupAfter = Downed == 0 ? 20.0f : 60.0f;
		if (State->PendingSpawn == 0 && ((PC && PC->WasInputKeyJustPressed(EKeys::N)) || State->AllDownFor > NewGroupAfter))
		{
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

		if (bHandScript && State->bArmed)
		{
			RunFreeHandTour();
			return;
		}
		if (!bScript || !State->bArmed)
		{
			return;
		}
		// The tour: each gun from the hip, aimed, firing at the nearest standing woman, reloading; then down one.
		UFightSubsystem* Fight = GetWorld()->GetSubsystem<UFightSubsystem>();
		AProtagonistPawn* Body = P->GetBody();
		if (!Fight || !Body)
		{
			return;
		}
		const auto Picture = [=, this](const TCHAR* What)
		{
			const FString Path = FPaths::ProjectSavedDir() / TEXT("GunRange") / FString::Printf(TEXT("%02d_%s.png"), State->Pictures++, What);
			FScreenshotRequest::RequestScreenshot(Path, true, false);  // with the UI: the scope view is drawn by it
			Notes.Add(FString::Printf(TEXT("gunrange: picture %s (gun %d, barrel off %.1f deg, sight off %.1f cm, shots %d, reloads %d)"),
				*FPaths::GetCleanFilename(Path), G->GetCurrentGun(), G->GetBarrelOffDegrees(), G->GetSightOffCm(), G->GetShotsSeen(), G->GetReloadsSeen()));
			Notes.Add(FString::Printf(TEXT("gunrange:   bones %s"), *G->GetBoneLine()));
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
					Fight->Reload();
					Next(3);
				}
				break;
			case 3:  // reload: pictures at a third and two thirds of it
				if (G->GetReloadFraction() > 0.33f && State->Shots >= 0)
				{
					Picture(*FString::Printf(TEXT("gun%d_reload_a"), State->Gun + 1));
					State->Shots = -1;
				}
				if (G->GetReloadFraction() > 0.66f && State->Shots == -1)
				{
					Picture(*FString::Printf(TEXT("gun%d_reload_b"), State->Gun + 1));
					State->Shots = -2;
				}
				if (State->StepTime > 6.0f || (State->StepTime > 1.0f && G->GetReloadFraction() < 0.0f))
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
			case 9:  // taking the melee weapon should turn third person on by itself (the main game draws the swing)
				G->SelectGun(GunCount + FMath::Min(4, G->GetMelee().Num() - 1));
				AimAtNearest();
				if (State->StepTime > 2.0f && !State->bFiringPicture)
				{
					Notes.Add(FString::Printf(TEXT("gunrange: melee rack %d in hand, view %s"), G->GetHeldRackIndex(),
						Body->IsFirstPerson() ? TEXT("first person (auto switch failed)") : TEXT("third person")));
					Picture(TEXT("melee_held"));
					State->bFiringPicture = true;
				}
				if (State->StepTime > 3.0f)
				{
					G->SelectGun(0);
					Next(11);
				}
				break;
			case 11:  // a gun again: first person back
				if (State->StepTime > 2.0f)
				{
					Notes.Add(FString::Printf(TEXT("gunrange: gun back in hand, view %s"), Body->IsFirstPerson() ? TEXT("first person") : TEXT("third person (switch back failed)")));
					Picture(TEXT("gun_again"));
					Next(10);
				}
				break;
			default:
				break;
		}
	};
	Play.Verify = [=, this]()
	{
		if (const UFreeHandSubsystem* Hand = GetWorld()->GetSubsystem<UFreeHandSubsystem>(); Hand && Hand->GetStats().FramesOn > 0)
		{
			PhaseNumbers.Add(TEXT("hand_touch_frames"), Hand->GetStats().TouchFrames);
			PhaseNumbers.Add(TEXT("hand_hold_frames"), Hand->GetStats().HoldBodyFrames);
			PhaseNumbers.Add(TEXT("hand_kicks"), Hand->GetStats().Kicks);
			PhaseNumbers.Add(TEXT("hand_worst_wrist_cm"), Hand->GetStats().WorstWristError);
			PhaseNumbers.Add(TEXT("hand_squish_contacts"), State->MostSquish);
		}
		if (UGunRangeSubsystem* G = Range())
		{
			PhaseNumbers.Add(TEXT("shots"), G->GetShotsSeen());
			PhaseNumbers.Add(TEXT("reloads"), G->GetReloadsSeen());
			for (int32 i = 0; i < G->GetGuns().Num(); ++i)
			{
				Notes.Add(FString::Printf(TEXT("gunrange: %s: %s"), *G->GetGuns()[i].Label, G->HasModel(i) ? TEXT("model") : TEXT("no model (box)")));
			}
		}
		if (UFightSubsystem* Fight = GetWorld()->GetSubsystem<UFightSubsystem>())
		{
			PhaseNumbers.Add(TEXT("hit_clips"), Fight->GetHitClips());
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
