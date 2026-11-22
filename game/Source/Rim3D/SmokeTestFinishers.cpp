// Smoke test `finishers` (sneed 2026-11-20): a finisher per melee weapon at a downed enemy (spear in the back, club smash,
// sword thrust into the chest, knife slammed down), Tab lock-on while sprinting with real key presses, the weapon and arms
// following the crosshair in third person, and no floating weapon after switching, freeing the hands and lying down.
#include "SmokeTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "UnrealClient.h"
#include "Widgets/SViewport.h"

#include "FightSubsystem.h"
#include "KeyBindings.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "WeaponDrawSubsystem.h"

#include "ColonySim/BodyMotion.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Cover.h"
#include "ColonySim/FightResponse.h"
#include "ColonySim/Health.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "CreatorPeople.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/WeaponSlot.h"

#include <algorithm>
#include <string>

namespace
{
	const FVector FinSmokeOrigin(340000.0, 0.0, 0.0);

	struct FFinSmokeState
	{
		bool bLooksApplied = false;
		colony::Entity Me = colony::kNoEntity;
		colony::Entity Enemy = colony::kNoEntity;
		colony::CellPos Cell{0, 0, 0};
		TArray<colony::Entity> Runners;

		// A finisher phase.
		bool bSent = false;
		int32 FramesAtStart = 0;
		int32 KneelsAtStart = -1;  // the body's kneel count when this phase's finisher was first seen
		int32 FixesAtStart = 0;
		int32 HitsAtStart = 0;
		float MaxT = 0.0f;
		float BestTipGap = 1.0e9f;
		bool bKneelSeen = false;
		bool bShot45 = false;
		bool bShot63 = false;
		bool bShot80 = false;
		bool bShotBlow = false;
		FString BlowGeometry;
		int32 Blows = 0;
		float LastBlowAt = -1.0f;
		float DownedAt = -1.0f;
		float SentAt = -1.0f;
		int32 Sends = 0;
		float ShortAtStrike = 0.0f;
		FString PinAtStrike;
		FString PinAtStart;

		// The Tab run.
		FVector LastBody = FVector::ZeroVector;
		bool bHaveLast = false;
		TArray<float> Speeds;
		TArray<colony::Entity> FocusSeen;
		int32 TabsSent = 0;
		bool bKeysDown = false;
		float FastestBeforeTab = 0.0f;
		float SlowestAfterTab = 1.0e9f;
		bool bFocusLeftViewport = false;

		// 3P aim.
		float WorstAimDegrees = 0.0f;
		float WorstYawDegrees = 0.0f;
		int32 AimFrames = 0;

		// Switch and lie down.
		int32 Step = 0;
		float WorstGap = 0.0f;
		int32 FixesInWindow = 0;
	};

	const colony::ThingView* FinSmokeFind(const USimSubsystem& Bridge, colony::Entity Thing)
	{
		return Thing == colony::kNoEntity ? nullptr : Bridge.GetCurrentSnapshot().find(Thing);
	}

	/** A key pressed (and, unless `bHold`, let go) through Slate, the way the keyboard arrives. */
	bool FinSmokeKey(const FKey& Key, bool bDown, bool bUp)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}
		FSlateApplication& Slate = FSlateApplication::Get();
		const uint32* KeyCode = nullptr;
		const uint32* CharCode = nullptr;
		FInputKeyManager::Get().GetCodesFromKey(Key, KeyCode, CharCode);
		const FKeyEvent Event(Key, FModifierKeysState(), Slate.GetUserIndexForKeyboard(), false, CharCode ? *CharCode : 0, KeyCode ? *KeyCode : 0);
		bool bHandled = false;
		if (bDown)
		{
			bHandled = Slate.ProcessKeyDownEvent(Event);
		}
		if (bUp)
		{
			Slate.ProcessKeyUpEvent(Event);
		}
		return bHandled;
	}

	bool FinSmokeViewportHasFocus()
	{
		if (!FSlateApplication::IsInitialized() || !GEngine || !GEngine->GameViewport)
		{
			return true;
		}
		const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
		const TSharedPtr<SViewport> Viewport = GEngine->GameViewport->GetGameViewportWidget();
		return !Focused.IsValid() || Focused == StaticCastSharedPtr<SWidget>(Viewport);
	}

	void FinSmokeHeal(colony::Sim& Sim, colony::Entity Pawn)
	{
		if (Pawn != colony::kNoEntity && Sim.things().spawned(Pawn))
		{
			Sim.things().registry().replace<colony::Health>(Pawn, colony::Health{});
			Sim.health().refresh(Pawn);
		}
	}

	/** A plain pawn (not the player) standing within 60 cm of the point. */
	colony::Entity FinSmokePawnAt(const USimSubsystem& Bridge, float X, float Y, colony::Entity Not)
	{
		for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.id != Not && !View.dead && !View.downed && !Bridge.GetSim()->colonistOf(View.id) &&
				FVector2D::Distance(FVector2D(X, Y), FVector2D(View.x, View.y)) < 0.6f)
			{
				return View.id;
			}
		}
		return colony::kNoEntity;
	}

	struct FFinSmokeWeapon
	{
		const TCHAR* Phase;
		const char* Def;
		const char* Move;
		bool bKneels;
	};
}

void USmokeTestSubsystem::BuildFinishersPhases()
{
	const TSharedRef<FFinSmokeState> State = MakeShared<FFinSmokeState>();

	const auto AddPhase = [this](const TCHAR* Name, TFunction<void()> Setup, float Warm, float Measure, TFunction<void()> EveryFrame, TFunction<void()> Check)
	{
		FPhase Phase;
		Phase.Name = Name;
		Phase.Setup = MoveTemp(Setup);
		Phase.WarmupSeconds = Warm;
		Phase.MeasureSeconds = Measure;
		Phase.OnFrame = MoveTemp(EveryFrame);
		Phase.Verify = MoveTemp(Check);
		Phases.Add(MoveTemp(Phase));
	};

	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Prot = [this]() { return GetWorld()->GetSubsystem<UProtagonistSubsystem>(); };
	const auto Fight = [this]() { return GetWorld()->GetSubsystem<UFightSubsystem>(); };
	const auto Pawns = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto Weapons = [this]() { return GetWorld()->GetSubsystem<UWeaponDrawSubsystem>(); };
	const auto MutableSim = [=]() -> colony::Sim* { return Bridge() && Bridge()->GetSim() ? const_cast<colony::Sim*>(Bridge()->GetSim()) : nullptr; };
	const auto ActorOf = [=](colony::Entity Thing) -> ASimPawnActor* { return Pawns() ? Pawns()->FindPawn(Thing) : nullptr; };
	const auto Shot = [this](const FString& Name)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke") / TestName / (Name + TEXT(".png")), false, false);
	};
	// A camera to the side of the line between two pawns, looking at the point between, low.
	const auto SideView = [=, this](colony::Entity A, colony::Entity B, double Side, double Height)
	{
		FVector PA, PB;
		if (!Bridge()->GetBlendedLocation(A, PA) || !Bridge()->GetBlendedLocation(B, PB))
		{
			return;
		}
		const FVector Middle = (PA + PB) * 0.5;
		const FVector Along = (PB - PA).GetSafeNormal2D();
		const FVector Out = FVector::CrossProduct(FVector::UpVector, Along);
		const FVector Eye = Middle + Out * Side - Along * 60.0 + FVector(0.0, 0.0, Height);
		ViewFrom(Eye, (Middle + FVector(0.0, 0.0, 60.0) - Eye).Rotation());
	};

	// Back to the player's own camera with the keys working (ViewFrom turns the pawn's input off).
	const auto PlayerView = [=, this]()
	{
		APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
		AProtagonistPawn* Body = Prot() ? Prot()->GetBody() : nullptr;
		if (PC && Body)
		{
			PC->SetViewTarget(Body);
			Body->EnableInput(PC);
		}
	};

	// 1. Crashlanded, the protagonist taken over in third person on a flat map.
	AddPhase(TEXT("setup"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!B || !P || !Fight() || !Pawns() || !Weapons())
		{
			Fail(TEXT("setup: a subsystem did not start"));
			return;
		}
		// MetaHuman bodies (the hold system and the finisher pose need them; mannequins keep the old drawing) by day.
		for (const TPair<const TCHAR*, float>& Var : {TPair<const TCHAR*, float>(TEXT("Pawns.MetaHumans"), 1.0f), TPair<const TCHAR*, float>(TEXT("Sky.Hour"), 10.0f)})
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
		B->SetWorldOrigin(FinSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("setup: the game's data did not load"));
			return;
		}
		B->SetSpeed(1);
		State->Me = P->FindProtagonist();
		const colony::ThingView* Me = FinSmokeFind(*B, State->Me);
		if (!Me)
		{
			Fail(TEXT("setup: no protagonist"));
			return;
		}
		State->Cell = colony::CellPos{FMath::FloorToInt32(Me->x), FMath::FloorToInt32(Me->y), 0};
		// Only the player: the other colonists would crowd the pictures and stand in the run.
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.id != State->Me)
			{
				B->Submit(colony::DestroyThingCommand{View.id});
			}
		}
		// The player on a MakeHuman body (the creator's generic woman, as on the gun range): the weapon hold, the
		// finisher pose and the crosshair aim drive MakeHuman bodies; MetaHumans and mannequins keep the old drawing.
		{
			const FSimPawnLooks From = Me->looks >= 0 ? USimPawnSubsystem::MakeLooks(B->GetCurrentSnapshot().looks[static_cast<size_t>(Me->looks)], B->GetDefs().get()) : FSimPawnLooks();
			Pawns()->SetLooksOverride(State->Me, CreatorPeople::DefaultLooks(true, From));
		}
		if (!P->EnterDirectMode(State->Me))
		{
			Fail(TEXT("setup: could not take control of the protagonist"));
			return;
		}
		P->GetBody()->SetFirstPerson(false);
		P->GetBody()->Look(FRotator::NormalizeAxis(-P->GetBody()->GetYaw()), -5.0f - P->GetBody()->GetPitch());
		P->GetBody()->SnapCamera();
	}, 3.5f, 0.5f, [=, this]()
	{
		// The creator's sliders go on once the MakeHuman body is there.
		if (ASimPawnActor* Actor = ActorOf(State->Me); Actor && Actor->GetMakeHumanMesh() && !State->bLooksApplied)
		{
			State->bLooksApplied = CreatorPeople::ApplyDefault(true, *Actor);
		}
	}, [=, this]()
	{
		const ASimPawnActor* Actor = ActorOf(State->Me);
		if (!Actor || !Actor->GetMakeHumanMesh())
		{
			Fail(TEXT("setup: the player is not on a MakeHuman body (is the creator base imported? run -run=MakeHumanArt)"));
		}
	});

	// 2. A finisher per weapon at a downed enemy just ahead: pictures from the side at the start, the strike and after.
	static const FFinSmokeWeapon FinWeapons[] = {
		{TEXT("finisher_spear"), "Melee_Spear", "Spear", false},
		{TEXT("finisher_club"), "Melee_Club", "Club", true},
		{TEXT("finisher_gladius"), "Melee_Gladius", "Sword", true},
		{TEXT("finisher_longsword"), "Melee_Longsword", "Sword", false},
		{TEXT("finisher_knife"), "Melee_Knife", "Knife", true},
	};
	for (const FFinSmokeWeapon& Weapon : FinWeapons)
	{
		const FString PhaseName = Weapon.Phase;
		AddPhase(Weapon.Phase, [=, this]()
		{
			colony::Sim* Sim = MutableSim();
			if (!Sim || State->Me == colony::kNoEntity)
			{
				return;
			}
			*State = [&]() { FFinSmokeState Fresh; Fresh.Me = State->Me; Fresh.Cell = State->Cell; Fresh.bLooksApplied = State->bLooksApplied; return Fresh; }();
			FinSmokeHeal(*Sim, State->Me);
			const colony::Entity Item = Sim->things().spawn(std::string(Weapon.Def), State->Cell);
			std::string Why;
			if (!Sim->combat().equip(State->Me, Item, &Why))
			{
				Fail(FString::Printf(TEXT("%s: could not equip %s: %s"), *PhaseName, UTF8_TO_TCHAR(Weapon.Def), UTF8_TO_TCHAR(Why.c_str())));
				return;
			}
			// The old victim goes; a new one is made 1.2 m east of the player.
			Bridge()->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{State->Cell.x + 1, State->Cell.y, 0}, 1});
			State->FramesAtStart = Weapons()->GetFinisherFrames();
			State->FixesAtStart = Weapons()->GetFloatFixes();
			State->HitsAtStart = Weapons()->GetHitsSeen();
			Weapons()->TakeWorstGripGap();
		}, 0.6f, 8.0f, [=, this]()
		{
			USimSubsystem* B = Bridge();
			colony::Sim* Sim = MutableSim();
			if (!B || !Sim)
			{
				return;
			}
			if (State->Enemy == colony::kNoEntity)
			{
				State->Enemy = FinSmokePawnAt(*B, State->Cell.x + 1.5f, State->Cell.y + 0.5f, State->Me);
				return;
			}
			if (!Sim->things().spawned(State->Enemy))
			{
				return;
			}
			// Knocked out for real (blows to the head till it is out cold: downed and limp, a ragdoll on the ground, as
			// a body downed in a fight is drawn), then made an enemy.
			const colony::ThingView* Victim = FinSmokeFind(*B, State->Enemy);
			if (!Victim)
			{
				return;
			}
			if (!Victim->downed && !Victim->dead && State->Blows < 12 && PhaseTime - State->LastBlowAt > 0.25f)
			{
				const std::vector<std::string> Labels = Sim->health().partLabels(Victim->def);
				const auto Head = std::find(Labels.begin(), Labels.end(), std::string("head"));
				colony::DamagePawnCommand Blow;
				Blow.pawn = State->Enemy;
				Blow.damageType = "Blunt";
				Blow.amount = 5.0f;
				Blow.part = Head != Labels.end() ? static_cast<int>(Head - Labels.begin()) : -1;
				B->Submit(Blow);
				++State->Blows;
				State->LastBlowAt = PhaseTime;
			}
			if (Victim->downed && State->DownedAt < 0.0f)
			{
				State->DownedAt = PhaseTime;
				B->Submit(colony::SetFactionCommand{State->Enemy, 2});
			}
			const bool bFinisherOn = Sim->melee().finisherOf(State->Me) != nullptr;
			if (State->bSent && !bFinisherOn && State->MaxT <= 0.0f && State->Sends < 4 && PhaseTime - State->SentAt > 0.6f)
			{
				State->bSent = false;  // refused (out of reach, still recovering): walk in and try again
			}
			if (!State->bSent && State->DownedAt >= 0.0f && PhaseTime > State->DownedAt + 1.0f)
			{
				// Face it, step up to it if the blows knocked it away, then the attack: a finisher, since it is on the ground.
				AProtagonistPawn* Body = Prot()->GetBody();
				const colony::ThingView* MeNow = FinSmokeFind(*B, State->Me);
				FVector Me, It;
				if (Body && MeNow && B->GetBlendedLocation(State->Me, Me) && B->GetBlendedLocation(State->Enemy, It))
				{
					Body->Look(FRotator::NormalizeAxis(static_cast<float>((It - Me).Rotation().Yaw) - Body->GetYaw()), 0.0f);
					const float Apart = FVector2D::Distance(FVector2D(MeNow->x, MeNow->y), FVector2D(Victim->x, Victim->y));
					// Up to the finisher's own distance from the body (a clip finisher steps the rest itself).
					float WalkTo = 0.8f;
					if (const colony::Sim* StandSim = Bridge()->GetSim())
					{
						const int StandFinisher = StandSim->melee().finisherFor(StandSim->defs().idOf("Things", Weapon.Def));
						if (StandSim->melee().finisherPlaysClip(StandFinisher))
						{
							WalkTo = StandSim->melee().rules().finishers[static_cast<size_t>(StandFinisher - 1)].standOff + 0.2f;
						}
					}
					if (Apart > WalkTo && PhaseTime < State->DownedAt + 4.0f)
					{
						Body->SetScriptedInput(0.0, 1.0, true);
						return;
					}
					Body->ClearScriptedInput();
				}
				State->bSent = true;
				State->SentAt = PhaseTime;
				++State->Sends;
				B->Submit(colony::MeleeCommand{State->Me, State->Enemy});
			}
			SideView(State->Me, State->Enemy, 280.0, 70.0);
			const colony::ThingView* MeView = FinSmokeFind(*B, State->Me);
			// Only this phase's finisher, at this phase's victim (not one left over from the phase before).
			const float T = MeView && MeView->meleeFinisher != 0 && MeView->meleeFinisherTarget == State->Enemy ? MeView->meleeFinisherT : 0.0f;
			State->MaxT = FMath::Max(State->MaxT, T);
			if (const ASimPawnActor* Actor = ActorOf(State->Me))
			{
				if (State->KneelsAtStart < 0)
				{
					State->KneelsAtStart = Actor->GetFinisherKneels();  // kneels from earlier phases don't count
				}
				State->bKneelSeen |= Actor->GetFinisherKneels() > State->KneelsAtStart && T > 0.2f && T < 0.8f;
			}
			if (T > 0.5f && T < 0.75f)
			{
				State->BestTipGap = FMath::Min(State->BestTipGap, static_cast<float>(FVector::Dist(Weapons()->GetFinisherTip(), Weapons()->GetFinisherGoal())));
			}
			if (T > 0.05f && State->PinAtStart.IsEmpty())
			{
				State->PinAtStart = Weapons()->GetPinDebug();
			}
			// At the blow itself (a clip finisher's hit moment): a picture and where everything is, so the distance from the
			// body can be tuned from numbers, not guessed from a camera angle (sneed 2026-11-22, round 2).
			float BlowT = -1.0f;
			if (const colony::Sim* BlowSim = Bridge()->GetSim(); BlowSim && MeView && MeView->meleeFinisher != 0 &&
				BlowSim->melee().finisherPlaysClip(MeView->meleeFinisher))
			{
				const colony::MeleeFinisher& Rule = BlowSim->melee().rules().finishers[static_cast<size_t>(MeView->meleeFinisher - 1)];
				if (const colony::MotionClip* BlowClip = BlowSim->combat().motion().clip(Rule.clip); BlowClip && BlowClip->seconds > 0.0f)
				{
					BlowT = BlowClip->hitSeconds / BlowClip->seconds;
				}
			}
			if (!State->bShotBlow && BlowT > 0.0f && T >= BlowT)
			{
				State->bShotBlow = true;
				Shot(PhaseName + TEXT("_blow"));
				const colony::ThingView* It = FinSmokeFind(*B, State->Enemy);
				const ASimPawnActor* MeActor = ActorOf(State->Me);
				const ASimPawnActor* ItActor = ActorOf(State->Enemy);
				const USkeletalMeshComponent* MeBody = MeActor ? MeActor->GetBody() : nullptr;
				const USkeletalMeshComponent* ItBody = ItActor ? ItActor->GetBody() : nullptr;
				const FVector MePelvis = MeBody && MeBody->GetBoneIndex(TEXT("pelvis")) != INDEX_NONE ? MeBody->GetBoneLocation(TEXT("pelvis")) : FVector::ZeroVector;
				const FVector ItPelvis = ItBody && ItBody->GetBoneIndex(TEXT("pelvis")) != INDEX_NONE ? ItBody->GetBoneLocation(TEXT("pelvis")) : FVector::ZeroVector;
				const FVector ItHead = ItBody && ItBody->GetBoneIndex(TEXT("head")) != INDEX_NONE ? ItBody->GetBoneLocation(TEXT("head")) : FVector::ZeroVector;
				const UStaticMeshComponent* Held = Weapons()->GetHandsComponent(State->Me);
				const FBox WeaponBox = Held ? Held->Bounds.GetBox() : FBox(ForceInit);
				const float SimApart = It && MeView ? FVector2D::Distance(FVector2D(MeView->x, MeView->y), FVector2D(It->x, It->y)) : -1.0f;
				FVector MeSpot = FVector::ZeroVector;
				FVector ItSpot = FVector::ZeroVector;
				B->GetBlendedLocation(State->Me, MeSpot);
				B->GetBlendedLocation(State->Enemy, ItSpot);
				State->BlowGeometry = FString::Printf(
					TEXT("sim apart %.2f m; drawn: my pelvis %.0f cm from my sim spot, its pelvis %.0f cm and its head %.0f cm from its sim spot, pelvis to pelvis %.0f cm; weapon box %.0f cm from its pelvis, lowest at %.0f cm, reaching %.0f cm out from my pelvis"),
					SimApart, FVector::Dist2D(MePelvis, MeSpot), FVector::Dist2D(ItPelvis, ItSpot), FVector::Dist2D(ItHead, ItSpot),
					FVector::Dist2D(MePelvis, ItPelvis), Held ? FMath::Sqrt(WeaponBox.ComputeSquaredDistanceToPoint(ItPelvis)) : -1.0,
					Held ? WeaponBox.Min.Z - ItPelvis.Z : -1.0,
					Held ? FMath::Max(FVector::Dist2D(WeaponBox.Min, MePelvis), FVector::Dist2D(WeaponBox.Max, MePelvis)) : -1.0);
			}
			if (!State->bShot45 && T >= 0.45f)
			{
				State->bShot45 = true;
				Shot(PhaseName + TEXT("_raise"));
			}
			else if (!State->bShot63 && T >= 0.63f)
			{
				State->bShot63 = true;
				State->PinAtStrike = Weapons()->GetPinDebug();
				State->ShortAtStrike = Weapons()->GetFinisherShort();
				Shot(PhaseName + TEXT("_strike"));
			}
			else if (!State->bShot80 && T >= 0.82f)
			{
				State->bShot80 = true;
				Shot(PhaseName + TEXT("_after"));
			}
		}, [=, this]()
		{
			if (AProtagonistPawn* Body = Prot()->GetBody())
			{
				Body->ClearScriptedInput();
			}
			UWeaponDrawSubsystem* W = Weapons();
			const int32 Frames = W->GetFinisherFrames() - State->FramesAtStart;
			const int32 Fixes = W->GetFloatFixes() - State->FixesAtStart;
			const int32 Hits = W->GetHitsSeen() - State->HitsAtStart;
			const float Gap = W->TakeWorstGripGap();
			PhaseNumbers.Add(TEXT("finisher_frames"), Frames);
			PhaseNumbers.Add(TEXT("max_t"), State->MaxT);
			PhaseNumbers.Add(TEXT("tip_gap_cm"), State->BestTipGap);
			PhaseNumbers.Add(TEXT("grip_gap_cm"), Gap);
			PhaseNumbers.Add(TEXT("float_fixes"), Fixes);
			Notes.Add(FString::Printf(TEXT("%s: %d finisher frames, got to T %.2f, tip %.0f cm from its goal at the strike, kneel %s, worst grip gap %.0f cm, %d float fixes, %d hits"),
				*PhaseName, Frames, State->MaxT, State->BestTipGap, State->bKneelSeen ? TEXT("yes") : TEXT("no"), Gap, Fixes, Hits));
			if (!State->BlowGeometry.IsEmpty())
			{
				Notes.Add(FString::Printf(TEXT("%s: at the blow: %s"), *PhaseName, *State->BlowGeometry));
			}
			Notes.Add(FString::Printf(TEXT("%s: weapon hold at the start [%s], at the strike [%s], pulled back %.0f cm to stay in reach, %d attack orders"), *PhaseName, *State->PinAtStart, *State->PinAtStrike, State->ShortAtStrike, State->Sends));
			if (State->Enemy == colony::kNoEntity)
			{
				Fail(PhaseName + TEXT(": the victim did not appear"));
				return;
			}
			if (State->DownedAt < 0.0f)
			{
				Fail(FString::Printf(TEXT("%s: %d blows to the head never downed the victim"), *PhaseName, State->Blows));
			}
			Bridge()->Submit(colony::DestroyThingCommand{State->Enemy});
			// A finisher that plays a whole-body clip (sneed 2026-11-22) has no hand-made weapon path, kneel or reach:
			// it must play, land its blow and keep the weapon in the hand.
			const colony::Sim* ClipSim = Bridge()->GetSim();
			const int ClipFinisher = ClipSim ? ClipSim->melee().finisherFor(ClipSim->defs().idOf("Things", Weapon.Def)) : 0;
			const bool bClip = ClipSim && ClipSim->melee().finisherPlaysClip(ClipFinisher);
			Notes.Add(FString::Printf(TEXT("%s: %s"), *PhaseName, bClip ? TEXT("whole-body clip finisher") : TEXT("hand-drawn finisher")));
			if (bClip)
			{
				if (State->MaxT < 0.8f)
				{
					Fail(FString::Printf(TEXT("%s: the %s finisher clip did not play through (T %.2f)"), *PhaseName, UTF8_TO_TCHAR(Weapon.Move), State->MaxT));
				}
				if (Fixes > 0)
				{
					Fail(FString::Printf(TEXT("%s: the weapon floated off the hand %d times"), *PhaseName, Fixes));
				}
				if (Hits <= 0)
				{
					Fail(PhaseName + TEXT(": the finisher's blow did not land"));
				}
				return;
			}
			if (Frames <= 0 || State->MaxT < 0.8f)
			{
				Fail(FString::Printf(TEXT("%s: no %s finisher played (frames %d, T %.2f)"), *PhaseName, UTF8_TO_TCHAR(Weapon.Move), Frames, State->MaxT));
			}
			if (State->BestTipGap > 15.0f)
			{
				Fail(FString::Printf(TEXT("%s: the blade never reached the body (closest %.0f cm)"), *PhaseName, State->BestTipGap));
			}
			if (Weapon.bKneels && !State->bKneelSeen)
			{
				Fail(PhaseName + TEXT(": the pawn did not kneel"));
			}
			if (Fixes > 0)
			{
				Fail(FString::Printf(TEXT("%s: the weapon floated off the hand %d times"), *PhaseName, Fixes));
			}
			if (Hits <= 0)
			{
				Fail(PhaseName + TEXT(": the finisher's blow did not land"));
			}
		});
	}

	// 3. Tab while sprinting, with real key presses: W held, sprint on, three Tabs a second apart. Running never slows and
	// each Tab moves the lock-on to another enemy.
	AddPhase(TEXT("tab_run"), [=, this]()
	{
		colony::Sim* Sim = MutableSim();
		USimSubsystem* B = Bridge();
		if (!Sim || !B)
		{
			return;
		}
		FinSmokeHeal(*Sim, State->Me);
		State->Runners.Reset();
		State->Speeds.Reset();
		State->FocusSeen.Reset();
		State->bHaveLast = false;
		State->TabsSent = 0;
		State->bKeysDown = false;
		State->SlowestAfterTab = 1.0e9f;
		State->bFocusLeftViewport = false;
		State->FastestBeforeTab = 0.0f;
		State->Enemy = colony::kNoEntity;
		PlayerView();
		const colony::ThingView* Me = FinSmokeFind(*B, State->Me);
		if (!Me)
		{
			return;
		}
		// Face +X along a clear strip; three enemies 7 m to the north, spread along the run.
		AProtagonistPawn* Body = Prot()->GetBody();
		Body->Look(FRotator::NormalizeAxis(-Body->GetYaw()), -8.0f - Body->GetPitch());
		Body->SnapCamera();
		const int32 X = FMath::FloorToInt32(Me->x);
		const int32 Y = FMath::FloorToInt32(Me->y);
		for (int32 I = 0; I < 3; ++I)
		{
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{X + 6 + I * 6, Y + 7, 0}, 1});
		}
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetAllUserFocusToGameViewport();
		}
		if (AProtagonistPawn::IsSprintToggleOn())
		{
			AProtagonistPawn::SetSprintToggle(false);
		}
	}, 0.5f, 4.5f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!B || !P || !P->GetBody())
		{
			return;
		}
		const colony::ThingView* Me = FinSmokeFind(*B, State->Me);
		if (State->Runners.Num() < 3 && Me)
		{
			State->Runners.Reset();
			for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
			{
				if (View.category == colony::ThingCategory::Pawn && View.id != State->Me && !B->GetSim()->colonistOf(View.id) && View.y > Me->y + 5.0f && View.y < Me->y + 9.0f)
				{
					State->Runners.Add(View.id);
				}
			}
			if (State->Runners.Num() >= 3)
			{
				for (const colony::Entity Runner : State->Runners)
				{
					B->Submit(colony::SetFactionCommand{Runner, 2});
					B->Submit(colony::SetFightResponseCommand{Runner, colony::FightResponse::Flee});  // stands till hit
				}
			}
		}
		// The keys, the way the keyboard sends them.
		const float At = PhaseTime - 0.5f;
		if (At >= 0.0f && !State->bKeysDown)
		{
			State->bKeysDown = true;
			FinSmokeKey(Rim3DKeys::Get(ERim3DAction::MoveForward), true, false);
			FinSmokeKey(Rim3DKeys::Get(ERim3DAction::Sprint), true, true);
		}
		for (int32 Press = 0; Press < 3; ++Press)
		{
			if (State->TabsSent == Press && At >= 1.0f + Press * 1.0f)
			{
				FinSmokeKey(Rim3DKeys::Get(ERim3DAction::FocusNextTarget), true, true);
				++State->TabsSent;
			}
		}
		if (!FinSmokeViewportHasFocus())
		{
			State->bFocusLeftViewport = true;
		}
		const colony::Entity Focus = P->GetFocusTarget();
		if (Focus != colony::kNoEntity && (State->FocusSeen.Num() == 0 || State->FocusSeen.Last() != Focus))
		{
			State->FocusSeen.Add(Focus);
		}
		// How fast the body goes over the ground (m/s), from frame to frame.
		const FVector Now = P->GetBody()->GetActorLocation();
		const float Delta = GetWorld()->GetDeltaSeconds();
		if (State->bHaveLast && Delta > 0.0f)
		{
			const float Speed = static_cast<float>(FVector::Dist2D(Now, State->LastBody)) / 100.0f / Delta;
			State->Speeds.Add(Speed);
			if (State->TabsSent == 0)
			{
				State->FastestBeforeTab = FMath::Max(State->FastestBeforeTab, Speed);
			}
			if (State->TabsSent > 0 && At > 1.0f)
			{
				// Averaged over the last 8 frames, so one slow frame doesn't count.
				float Sum = 0.0f;
				const int32 N = FMath::Min(8, State->Speeds.Num());
				for (int32 I = State->Speeds.Num() - N; I < State->Speeds.Num(); ++I)
				{
					Sum += State->Speeds[I];
				}
				State->SlowestAfterTab = FMath::Min(State->SlowestAfterTab, Sum / FMath::Max(1, N));
			}
		}
		State->LastBody = Now;
		State->bHaveLast = At >= 0.3f;
	}, [=, this]()
	{
		FinSmokeKey(Rim3DKeys::Get(ERim3DAction::MoveForward), false, true);
		if (AProtagonistPawn::IsSprintToggleOn())
		{
			AProtagonistPawn::SetSprintToggle(false);
		}
		for (const colony::Entity Runner : State->Runners)
		{
			Bridge()->Submit(colony::DestroyThingCommand{Runner});
		}
		PhaseNumbers.Add(TEXT("fastest_before_tab"), State->FastestBeforeTab);
		PhaseNumbers.Add(TEXT("slowest_after_tab"), State->SlowestAfterTab);
		PhaseNumbers.Add(TEXT("targets_seen"), State->FocusSeen.Num());
		Notes.Add(FString::Printf(TEXT("tab_run: %.1f m/s before the first Tab, %d Tabs, %d different lock-on targets in turn, slowest %.1f m/s after a Tab, keyboard focus %s the game"),
			State->FastestBeforeTab, State->TabsSent, State->FocusSeen.Num(), State->SlowestAfterTab, State->bFocusLeftViewport ? TEXT("LEFT") : TEXT("stayed on")));
		if (State->Runners.Num() < 3)
		{
			Fail(TEXT("tab_run: the three enemies did not appear"));
			return;
		}
		if (State->FastestBeforeTab < 3.0f)
		{
			Fail(FString::Printf(TEXT("tab_run: the run never started (%.1f m/s before any Tab: the held W key did not reach the pawn)"), State->FastestBeforeTab));
		}
		else if (State->SlowestAfterTab < 3.0f)
		{
			Fail(FString::Printf(TEXT("tab_run: Tab slowed the run to %.1f m/s"), State->SlowestAfterTab));
		}
		if (State->FocusSeen.Num() < 3)
		{
			Fail(FString::Printf(TEXT("tab_run: three Tabs locked on to only %d different targets"), State->FocusSeen.Num()));
		}
		if (State->bFocusLeftViewport)
		{
			Fail(TEXT("tab_run: Tab moved the keyboard focus off the game"));
		}
	});

	// 4. Third person aim: the camera turned 40 degrees from the body, aim held. The body turns with the camera and the
	// rifle points at the crosshair.
	AddPhase(TEXT("aim3p"), [=, this]()
	{
		colony::Sim* Sim = MutableSim();
		if (!Sim)
		{
			return;
		}
		Prot()->EndFocus();
		PlayerView();
		FinSmokeHeal(*Sim, State->Me);
		const colony::Entity Rifle = Sim->things().spawn(std::string("Gun_AssaultRifle"), State->Cell);
		Sim->combat().equip(State->Me, Rifle);
		AProtagonistPawn* Body = Prot()->GetBody();
		Body->Look(40.0f, 0.0f);
		Body->SetScriptedAim(true);
		State->WorstAimDegrees = 0.0f;
		State->WorstYawDegrees = 0.0f;
		State->AimFrames = 0;
	}, 1.5f, 1.5f, [=, this]()
	{
		if (PhaseTime < 1.5f)
		{
			return;
		}
		AProtagonistPawn* Body = Prot()->GetBody();
		UStaticMeshComponent* Gun = Weapons()->GetHandsComponent(State->Me);
		const ASimPawnActor* Actor = ActorOf(State->Me);
		FVector Aim;
		if (!Body || !Gun || !Actor || !Fight()->ComputeAimPoint(Aim))
		{
			return;
		}
		++State->AimFrames;
		const FVector ToAim = (Aim - Gun->GetComponentLocation()).GetSafeNormal();
		const float AimDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(ToAim, Gun->GetForwardVector()), -1.0, 1.0)));
		const float YawDegrees = FMath::Abs(FRotator::NormalizeAxis(static_cast<float>(Actor->GetActorRotation().Yaw) - Body->GetYaw()));
		State->WorstAimDegrees = FMath::Max(State->WorstAimDegrees, AimDegrees);
		if (State->AimFrames % 60 == 1)
		{
			Notes.Add(FString::Printf(TEXT("aim3p frame %d: rifle %.1f degrees off (forward %s, to the aim %s, aim point %.0f m away), aim blend %.2f, hold [%s]"), State->AimFrames, AimDegrees,
				*Gun->GetForwardVector().ToCompactString(), *ToAim.ToCompactString(), FVector::Dist(Aim, Gun->GetComponentLocation()) / 100.0, Body->GetAimBlend(), *Weapons()->GetPinDebug()));
		}
		State->WorstYawDegrees = FMath::Max(State->WorstYawDegrees, YawDegrees);
	}, [=, this]()
	{
		Prot()->GetBody()->ClearScriptedAim();
		PhaseNumbers.Add(TEXT("aim_degrees"), State->WorstAimDegrees);
		PhaseNumbers.Add(TEXT("body_yaw_degrees"), State->WorstYawDegrees);
		Notes.Add(FString::Printf(TEXT("aim3p: %d frames, rifle at most %.1f degrees off the crosshair, body at most %.1f degrees off the camera"),
			State->AimFrames, State->WorstAimDegrees, State->WorstYawDegrees));
		if (State->AimFrames == 0)
		{
			Fail(TEXT("aim3p: no rifle in the hands or no aim point"));
			return;
		}
		if (State->WorstAimDegrees > 8.0f)
		{
			Fail(FString::Printf(TEXT("aim3p: the rifle points %.1f degrees off the crosshair"), State->WorstAimDegrees));
		}
		if (State->WorstYawDegrees > 20.0f)
		{
			Fail(FString::Printf(TEXT("aim3p: the body faces %.1f degrees away from the camera"), State->WorstYawDegrees));
		}
	});

	// 5. sneed's float: carry a rifle and a gladius, switch (Q), free hands (J) and back, put the weapon down (X) and pick
	// it up (E), then lie down (Z) and crouch (C), all by keys. The weapon stays in the hand throughout.
	const auto AddLiePhase = [=, this](const TCHAR* Name, ERim3DAction Last)
	{
		const FString PhaseName = Name;
		AddPhase(Name, [=, this]()
		{
			colony::Sim* Sim = MutableSim();
			USimSubsystem* B = Bridge();
			if (!Sim || !B)
			{
				return;
			}
			FinSmokeHeal(*Sim, State->Me);
			B->Submit(colony::SetStanceCommand{State->Me, colony::Stance::Standing});
			const colony::Entity Rifle = Sim->things().spawn(std::string("Gun_AssaultRifle"), State->Cell);
			const colony::Entity Gladius = Sim->things().spawn(std::string("Melee_Gladius"), State->Cell);
			B->Submit(colony::CarryWeaponCommand{State->Me, Rifle});
			B->Submit(colony::CarryWeaponCommand{State->Me, Gladius});
			State->Step = 0;
			State->WorstGap = 0.0f;
			State->FixesInWindow = 0;
			State->FixesAtStart = Weapons()->GetFloatFixes();
		}, 8.0f, 2.5f, [=, this]()
		{
			// Keys at set times: Q, J, J, Q, X, E, then the last key.
			struct FKeyAt { float At; ERim3DAction Action; };
			const FKeyAt Keys[] = {
				{0.6f, ERim3DAction::SwitchWeapon}, {2.0f, ERim3DAction::FreeHandToggle}, {3.0f, ERim3DAction::FreeHandToggle},
				{4.0f, ERim3DAction::SwitchWeapon}, {5.2f, ERim3DAction::PutDown}, {6.0f, ERim3DAction::EquipWeapon}, {7.0f, Last},
			};
			if (State->Step < static_cast<int32>(UE_ARRAY_COUNT(Keys)) && PhaseTime >= Keys[State->Step].At)
			{
				FinSmokeKey(Rim3DKeys::Get(Keys[State->Step].Action), true, true);
				++State->Step;
			}
			if (PhaseTime >= 7.6f && PhaseTime < 7.7f)
			{
				// Measure only once lying (or crouched) and settled.
				Weapons()->TakeWorstGripGap();
				State->FixesInWindow = Weapons()->GetFloatFixes();
			}
			if (const ASimPawnActor* Actor = ActorOf(State->Me))
			{
				const FVector P = Actor->GetActorLocation();
				const FVector Eye = P + Actor->GetActorRightVector() * 220.0 + Actor->GetActorForwardVector() * 120.0 + FVector(0.0, 0.0, 90.0);
				ViewFrom(Eye, (P + FVector(0.0, 0.0, 30.0) - Eye).Rotation());
			}
		}, [=, this]()
		{
			const float Gap = Weapons()->TakeWorstGripGap();
			const int32 Fixes = Weapons()->GetFloatFixes() - State->FixesInWindow;
			const int32 FixesAll = Weapons()->GetFloatFixes() - State->FixesAtStart;
			const colony::ThingView* Me = FinSmokeFind(*Bridge(), State->Me);
			PhaseNumbers.Add(TEXT("grip_gap_cm"), Gap);
			PhaseNumbers.Add(TEXT("float_fixes_settled"), Fixes);
			PhaseNumbers.Add(TEXT("float_fixes_all"), FixesAll);
			Notes.Add(FString::Printf(TEXT("%s: %d keys sent, weapon in hands %s, worst grip gap %.0f cm once down, float fixes %d once down (%d over the whole sequence)"),
				*PhaseName, State->Step, Me && Me->weapon.valid() ? TEXT("yes") : TEXT("no"), Gap, Fixes, FixesAll));
			if (Gap > 22.0f || Fixes > 0)
			{
				Fail(FString::Printf(TEXT("%s: the weapon floats off the hand (%.0f cm, %d fixes)"), *PhaseName, Gap, Fixes));
			}
		});
	};
	AddLiePhase(TEXT("swap_prone"), ERim3DAction::Prone);
	AddLiePhase(TEXT("swap_crouch"), ERim3DAction::Crouch);
}
