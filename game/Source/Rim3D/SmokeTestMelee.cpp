// Smoke test `melee` (task U22): holstered weapons, the weapon switch key, and melee swings, blocks, dodges
// and hit-stops on screen, in the game, offscreen.
#include "SmokeTest.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"

#include "FightSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "WeaponDrawSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/WeaponSlot.h"

#include <algorithm>
#include <string>

namespace
{
	const FVector MeleeSmokeOrigin(300000.0, 0.0, 0.0);

	// Where weapons sit on a standing pawn, from WeaponDrawSubsystem.cpp (actor space, cm).
	const FVector MeleeSmokeHands(26.0, 14.0, 118.0);
	const FVector MeleeSmokeRifleHolster(-14.0, 6.0, 104.0);
	const FVector MeleeSmokeSidearmHolster(4.0, 23.0, 80.0);
	const FVector MeleeSmokeMeleeHolster(-2.0, -23.0, 86.0);

	// In the hand: hung on a hand bone (HoldDraw, docs/design/weapon-hold.md), or at the old fixed place when the hands
	// are off (Weapons.Hands 0).
	bool MeleeSmokeInHand(const USceneComponent* Component, const FVector& OldPlace, double Tolerance)
	{
		if (!Component)
		{
			return false;
		}
		const FName Socket = Component->GetAttachSocketName();
		if (Socket == FName(TEXT("hand_r")) || Socket == FName(TEXT("hand_l")))
		{
			return true;
		}
		return Component->GetRelativeLocation().Equals(OldPlace, Tolerance);
	}

	// Where a component is in its actor's frame (cm), whatever it hangs from.
	FVector MeleeSmokeActorSpace(const USceneComponent& Component)
	{
		const AActor* Owner = Component.GetOwner();
		if (!Owner)
		{
			return Component.GetRelativeLocation();
		}
		FTransform Frame = Owner->GetActorTransform();
		Frame.SetScale3D(FVector::OneVector);
		return Frame.InverseTransformPosition(Component.GetComponentLocation());
	}

	struct FMeleeSmokeState
	{
		colony::Entity Me = colony::kNoEntity;
		colony::Entity Enemy = colony::kNoEntity;
		colony::Entity Attacker = colony::kNoEntity;
		colony::Entity Defender = colony::kNoEntity;
		colony::Entity Rifle = colony::kNoEntity;
		colony::Entity Revolver = colony::kNoEntity;
		colony::Entity Gladius = colony::kNoEntity;
		colony::Entity Shield = colony::kNoEntity;
		colony::Entity Basher = colony::kNoEntity;
		FVector2D Start = FVector2D::ZeroVector;
		colony::CellPos Cell{0, 0, 0};

		// Phase bookkeeping.
		bool bSent = false;
		bool bPaused = false;
		bool bSwingStarted = false;
		bool bEnemyMade = false;
		bool bStaged = false;
		int64 LastOrderStep = -1;
		int32 SwitchTotal = 0;
		float MaxOldFromHands = 0.0f;
		float MaxNewFromHolster = 0.0f;
		float MaxLean = 0.0f;
		float MaxBlock = 0.0f;
		float LastHealSeconds = 0.0f;
		FVector PausedRifle = FVector::ZeroVector;
		float WindupZ = 0.0f;
		float WindupLean = 0.0f;
		FString SwingPhaseAtPause;

		int32 SwingsAtStart = 0;
		int32 HitsAtStart = 0;
		int32 OnTimeAtStart = 0;
		int32 LateAtStart = 0;
		int32 StopsAtStart = 0;
		int32 ImpactsAtStart = 0;
		int32 ReactionsAtStart = 0;
		int32 OrdersAtStart = 0;
		int32 SwitchesAtStart = 0;
		int32 SwitchesDoneAtStart = 0;
		int32 FireOrdersAtStart = 0;
		int32 DodgesAtStart = 0;
		int32 BlockedAtStart = 0;
		int32 BlocksAtStart = 0;
		int32 ShieldOrdersAtStart = 0;
		int32 ShieldBlocksAtStart = 0;
		int32 SparksAtStart = 0;
		float MaxShieldRaise = 0.0f;
		bool bShieldUpSeen = false;
		FVector RaisedAt = FVector::ZeroVector;
	};

	const colony::ThingView* MeleeSmokeFind(const USimSubsystem& Bridge, colony::Entity Thing)
	{
		return Thing == colony::kNoEntity ? nullptr : Bridge.GetCurrentSnapshot().find(Thing);
	}

	/** The item made from this def lying nearest the point (sim metres), within `Within` metres. */
	colony::Entity MeleeSmokeNearestItem(const USimSubsystem& Bridge, const char* DefName, float X, float Y, float Within)
	{
		colony::Entity Best = colony::kNoEntity;
		float BestDistance = Within;
		if (!Bridge.GetDefs())
		{
			return Best;
		}
		for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Item || !View.def.valid() || Bridge.GetDefs()->get(View.def).defName() != DefName)
			{
				continue;
			}
			const float Distance = FVector2D::Distance(FVector2D(X, Y), FVector2D(View.x, View.y));
			if (Distance < BestDistance)
			{
				Best = View.id;
				BestDistance = Distance;
			}
		}
		return Best;
	}

	/** A plain pawn (not a colonist) standing within 60 cm of the point. */
	colony::Entity MeleeSmokePawnAt(const USimSubsystem& Bridge, float X, float Y, colony::Entity Not1, colony::Entity Not2)
	{
		for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.id != Not1 && View.id != Not2 && !Bridge.GetSim()->colonistOf(View.id) &&
				FVector2D::Distance(FVector2D(X, Y), FVector2D(View.x, View.y)) < 0.6f)
			{
				return View.id;
			}
		}
		return colony::kNoEntity;
	}

	/** Unhurt again, so a long fight does not end the test. */
	void MeleeSmokeHeal(colony::Sim& Sim, colony::Entity Pawn)
	{
		if (Pawn != colony::kNoEntity && Sim.things().spawned(Pawn))
		{
			Sim.things().registry().replace<colony::Health>(Pawn, colony::Health{});
			Sim.health().refresh(Pawn);
		}
	}

	/** A training dummy: the enemy the player's swings are tried on reads no swing, so it neither sidesteps, rolls nor
	    parries (real-time combat, docs/design/real-time-combat.md: AI pawns defend by reaction time). */
	void MeleeSmokeDummy(colony::Sim& Sim, colony::Entity Pawn)
	{
		if (Pawn != colony::kNoEntity && Sim.things().spawned(Pawn))
		{
			Sim.things().registry().get_or_emplace<colony::Melee>(Pawn).noDefendUntil = TNumericLimits<int64>::Max() / 2;
		}
	}

	void MeleeSmokeSkill(colony::Sim& Sim, colony::Entity Pawn, int Level)
	{
		colony::Colonist& Record = Sim.things().registry().get_or_emplace<colony::Colonist>(Pawn);
		Record.skills.clear();
		Record.skills.push_back(colony::SkillRecord{"Melee", Level});
	}
}

void USmokeTestSubsystem::BuildMeleePhases()
{
	const TSharedRef<FMeleeSmokeState> State = MakeShared<FMeleeSmokeState>();

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
	const auto Snapshot = [=](colony::Entity Thing) { return MeleeSmokeFind(*Bridge(), Thing); };
	const auto ActorOf = [=](colony::Entity Thing) -> ASimPawnActor* { return Pawns() ? Pawns()->FindPawn(Thing) : nullptr; };

	// A camera to the side of the line between two pawns, looking at the point between.
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
		const FVector Eye = Middle + Out * Side + FVector(0.0, 0.0, Height);
		ViewFrom(Eye, (Middle + FVector(0.0, 0.0, 105.0) - Eye).Rotation());
	};
	// A camera to the right of a pawn, a little ahead, looking at its chest.
	const auto BodyView = [=, this](colony::Entity Pawn, double Right, double Forward, double Height)
	{
		const ASimPawnActor* Actor = ActorOf(Pawn);
		if (!Actor)
		{
			return;
		}
		const FVector P = Actor->GetActorLocation();
		const FVector Eye = P + Actor->GetActorRightVector() * Right + Actor->GetActorForwardVector() * Forward + FVector(0.0, 0.0, Height);
		ViewFrom(Eye, (P + FVector(0.0, 0.0, 100.0) - Eye).Rotation());
	};

	const auto MarkBaselines = [=]()
	{
		UWeaponDrawSubsystem* W = Weapons();
		UFightSubsystem* F = Fight();
		State->SwingsAtStart = W->GetSwingsSeen();
		State->HitsAtStart = W->GetHitsSeen();
		State->OnTimeAtStart = W->GetHitsOnTime();
		State->LateAtStart = W->GetHitsLate();
		State->StopsAtStart = W->GetHitStopsPlayed();
		State->ImpactsAtStart = F->GetImpactsOfKind(colony::ProjectileHitKind::Pawn);
		State->ReactionsAtStart = F->GetHitReactions();
		State->OrdersAtStart = W->GetSwingOrders();
		State->SwitchesAtStart = W->GetSwitchesStarted();
		State->SwitchesDoneAtStart = W->GetSwitchesDone();
		State->FireOrdersAtStart = F->GetFireOrders();
		State->DodgesAtStart = W->GetDodgeActions();
		State->BlockedAtStart = W->GetBlockedSeen();
		State->BlocksAtStart = W->GetBlockActions();
	};

	// Keeps the fighters on their feet: whoever is hurt gets healed every few seconds.
	const auto KeepAlive = [=, this]()
	{
		if (PhaseTime - State->LastHealSeconds < 2.5f || !Bridge() || !Bridge()->GetSim())
		{
			return;
		}
		State->LastHealSeconds = PhaseTime;
		colony::Sim& Sim = *const_cast<colony::Sim*>(Bridge()->GetSim());
		for (const colony::Entity Pawn : {State->Me, State->Enemy, State->Attacker, State->Defender})
		{
			MeleeSmokeHeal(Sim, Pawn);
		}
	};

	// The enemy of the swing phases stands as a dummy that doesn't defend.
	const auto HoldDummy = [=, this]()
	{
		if (Bridge() && Bridge()->GetSim())
		{
			MeleeSmokeDummy(*const_cast<colony::Sim*>(Bridge()->GetSim()), State->Enemy);
		}
	};

	// 1. A sim with the Crashlanded start, three weapons on the ground by the protagonist, who is taken over in third person.
	AddPhase(TEXT("setup"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!B || !P || !Fight() || !Pawns() || !Weapons())
		{
			Fail(TEXT("setup: a subsystem did not start (sim, protagonist, fights, pawns or weapons)"));
			return;
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 128;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(MeleeSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("setup: the game's data did not load"));
			return;
		}
		for (const std::string& Problem : B->GetSim()->setupErrors())
		{
			Fail(FString::Printf(TEXT("setup: sim setup: %s"), UTF8_TO_TCHAR(Problem.c_str())));
		}
		B->SetSpeed(1);
		State->Me = P->FindProtagonist();
		const colony::ThingView* Me = Snapshot(State->Me);
		if (!Me)
		{
			Fail(TEXT("setup: no protagonist"));
			return;
		}
		State->Start = FVector2D(Me->x, Me->y);
		State->Cell = colony::CellPos{FMath::FloorToInt32(Me->x), FMath::FloorToInt32(Me->y), 0};
		B->Submit(colony::SpawnThingCommand{"Gun_BoltActionRifle", colony::CellPos{State->Cell.x - 2, State->Cell.y + 4, 0}, 1});
		B->Submit(colony::SpawnThingCommand{"Gun_Revolver", colony::CellPos{State->Cell.x, State->Cell.y + 4, 0}, 1});
		B->Submit(colony::SpawnThingCommand{"Melee_Gladius", colony::CellPos{State->Cell.x + 2, State->Cell.y + 4, 0}, 1});
		if (!P->EnterDirectMode(State->Me))
		{
			Fail(TEXT("setup: could not take control of the protagonist"));
			return;
		}
		P->GetBody()->SetFirstPerson(false);
		P->GetBody()->Look(FRotator::NormalizeAxis(-P->GetBody()->GetYaw()), -5.0f - P->GetBody()->GetPitch());
		P->GetBody()->SnapCamera();
	}, 2.5f, 0.5f, nullptr, [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		const float Y = static_cast<float>(State->Cell.y + 4) + 0.5f;
		State->Rifle = MeleeSmokeNearestItem(*B, "Gun_BoltActionRifle", State->Cell.x - 1.5f, Y, 1.2f);
		State->Revolver = MeleeSmokeNearestItem(*B, "Gun_Revolver", State->Cell.x + 0.5f, Y, 1.2f);
		State->Gladius = MeleeSmokeNearestItem(*B, "Melee_Gladius", State->Cell.x + 2.5f, Y, 1.2f);
		if (State->Rifle == colony::kNoEntity || State->Revolver == colony::kNoEntity || State->Gladius == colony::kNoEntity)
		{
			Fail(TEXT("setup: the rifle, revolver or gladius did not appear"));
		}
		if (!ActorOf(State->Me) || !Prot()->IsDirectMode())
		{
			Fail(TEXT("setup: the protagonist has no pawn actor or is not under direct control"));
		}
		const colony::ThingView* Me = Snapshot(State->Me);
		if (Me && (Me->weapon.valid() || Me->carried[0].valid() || Me->carried[1].valid() || Me->carried[2].valid()))
		{
			Fail(TEXT("setup: the protagonist carries a weapon before it was given one"));
		}
		if (Weapons()->CountVisibleWeapons(State->Me) != 0)
		{
			Fail(TEXT("setup: weapons are drawn on a pawn that carries none"));
		}
	});

	// 2. Three weapons given: the rifle goes in the hands, the revolver and the gladius ride on the body.
	AddPhase(TEXT("holsters"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		B->Submit(colony::CarryWeaponCommand{State->Me, State->Rifle});
		B->Submit(colony::CarryWeaponCommand{State->Me, State->Revolver});
		B->Submit(colony::CarryWeaponCommand{State->Me, State->Gladius});
	}, 2.0f, 2.5f, [=, this]()
	{
		BodyView(State->Me, 240.0, 80.0, 150.0);
	}, [=, this]()
	{
		const colony::ThingView* Me = Snapshot(State->Me);
		UWeaponDrawSubsystem* W = Weapons();
		if (!Me || !Me->carried[0].valid() || !Me->carried[1].valid() || !Me->carried[2].valid())
		{
			Fail(TEXT("holsters: the sim does not have the protagonist carrying a rifle, a sidearm and a melee weapon"));
			return;
		}
		if (!Me->weapon.valid() || Me->weapon != Me->carried[0])
		{
			Fail(TEXT("holsters: the rifle is not the weapon in the hands"));
		}
		if (W->CountVisibleWeapons(State->Me) != 3)
		{
			Fail(FString::Printf(TEXT("holsters: %d weapons are drawn on the protagonist, expected 3"), W->CountVisibleWeapons(State->Me)));
			return;
		}
		const auto Near = [&](int32 Slot, const FVector& Where, const TCHAR* Name)
		{
			const UStaticMeshComponent* Component = W->GetSlotComponent(State->Me, Slot);
			if (!Component || !Component->GetRelativeLocation().Equals(Where, 3.0))
			{
				Fail(FString::Printf(TEXT("holsters: the %s is at %s, expected %s"), Name, Component ? *Component->GetRelativeLocation().ToString() : TEXT("nowhere"), *Where.ToString()));
			}
		};
		if (const UStaticMeshComponent* Rifle = W->GetSlotComponent(State->Me, 0); !MeleeSmokeInHand(Rifle, MeleeSmokeHands, 3.0))
		{
			Fail(FString::Printf(TEXT("holsters: the rifle is not in the hands (at %s on %s)"), Rifle ? *MeleeSmokeActorSpace(*Rifle).ToString() : TEXT("nowhere"),
				Rifle ? *Rifle->GetAttachSocketName().ToString() : TEXT("nothing")));
		}
		Near(1, MeleeSmokeSidearmHolster, TEXT("sidearm on the hip"));
		if (const UStaticMeshComponent* Melee = W->GetSlotComponent(State->Me, 2); !Melee || FMath::Abs(Melee->GetRelativeLocation().Y - MeleeSmokeMeleeHolster.Y) > 3.0 || Melee->GetRelativeLocation().Z > 100.0)
		{
			Fail(TEXT("holsters: the melee weapon is not on the left hip"));
		}
		if (W->GetHandsComponent(State->Me) != W->GetSlotComponent(State->Me, 0))
		{
			Fail(TEXT("holsters: the weapon in the hands is not the rifle's mesh"));
		}
		if (Fight()->GetHeldGun(State->Me) != W->GetSlotComponent(State->Me, 0))
		{
			Fail(TEXT("holsters: the fight subsystem does not see the rifle in the hands"));
		}
	});

	// 3. The switch key: Q brings up the sidearm. The rifle goes to the back and the revolver comes up to the hand.
	AddPhase(TEXT("switch_key"), [=, this]()
	{
		State->bSent = false;
		State->SwitchTotal = 0;
		State->MaxOldFromHands = 0.0f;
		State->MaxNewFromHolster = 0.0f;
		MarkBaselines();
	}, 0.5f, 3.5f, [=, this]()
	{
		BodyView(State->Me, 240.0, 80.0, 150.0);
		const colony::ThingView* Me = Snapshot(State->Me);
		UWeaponDrawSubsystem* W = Weapons();
		if (!Me)
		{
			return;
		}
		if (!State->bSent && PhaseTime > 0.6f)
		{
			State->bSent = true;
			if (!W->SwitchNext())
			{
				Fail(TEXT("switch_key: Q did not start a switch"));
			}
		}
		if (Me->switchSlot == static_cast<uint8_t>(colony::WeaponSlot::Sidearm))
		{
			State->SwitchTotal = FMath::Max(State->SwitchTotal, Me->switchStepsLeft);
			const UStaticMeshComponent* Old = W->GetSlotComponent(State->Me, 0);
			const UStaticMeshComponent* New = W->GetSlotComponent(State->Me, 1);
			if (Old && New)
			{
				State->MaxOldFromHands = FMath::Max<float>(State->MaxOldFromHands, FVector::Dist(Old->GetRelativeLocation(), MeleeSmokeHands));
				State->MaxNewFromHolster = FMath::Max<float>(State->MaxNewFromHolster, FVector::Dist(New->GetRelativeLocation(), MeleeSmokeSidearmHolster));
			}
		}
	}, [=, this]()
	{
		const colony::ThingView* Me = Snapshot(State->Me);
		UWeaponDrawSubsystem* W = Weapons();
		if (!Me)
		{
			return;
		}
		if (W->GetSwitchesStarted() - State->SwitchesAtStart != 1 || W->GetSwitchesDone() - State->SwitchesDoneAtStart != 1)
		{
			Fail(FString::Printf(TEXT("switch_key: %d switches started and %d finished, expected 1 and 1"), W->GetSwitchesStarted() - State->SwitchesAtStart, W->GetSwitchesDone() - State->SwitchesDoneAtStart));
		}
		if (State->SwitchTotal <= 0)
		{
			Fail(TEXT("switch_key: the snapshot never showed a switch in progress"));
		}
		if (State->MaxOldFromHands < 20.0f || State->MaxNewFromHolster < 20.0f)
		{
			Fail(FString::Printf(TEXT("switch_key: the weapons barely moved (%.0f cm for the one put away, %.0f cm for the one brought up)"), State->MaxOldFromHands, State->MaxNewFromHolster));
		}
		if (!Me->weapon.valid() || Me->weapon != Me->carried[1])
		{
			Fail(TEXT("switch_key: the revolver is not in the hands after the switch"));
		}
		const UStaticMeshComponent* Rifle = W->GetSlotComponent(State->Me, 0);
		const UStaticMeshComponent* Revolver = W->GetSlotComponent(State->Me, 1);
		if (!Rifle || !Rifle->GetRelativeLocation().Equals(MeleeSmokeRifleHolster, 3.0))
		{
			Fail(TEXT("switch_key: the rifle is not on the back after the switch"));
		}
		if (!Revolver || !MeleeSmokeInHand(Revolver, MeleeSmokeHands, 3.0) || W->GetHandsComponent(State->Me) != Revolver)
		{
			Fail(TEXT("switch_key: the revolver is not in the hand after the switch"));
		}
		Notes.Add(FString::Printf(TEXT("switch_key: a %d step switch; the rifle went %.0f cm from the hand, the revolver %.0f cm from its holster"), State->SwitchTotal, State->MaxOldFromHands, State->MaxNewFromHolster));
		// A gun in the hands shoots: the swing key does nothing, and the melee weapon is still on the body.
		if (W->Swing())
		{
			Fail(TEXT("switch_key: the swing key swung with a gun in the hands"));
		}
	});

	// 4. A switch stopped three quarters of the way through: the rifle half way up from the back.
	AddPhase(TEXT("switch_frozen"), [=, this]()
	{
		State->bSent = false;
		State->bPaused = false;
		State->SwitchTotal = 0;
		Bridge()->SetSpeed(1);
	}, 1.0f, 3.0f, [=, this]()
	{
		BodyView(State->Me, 240.0, 80.0, 150.0);
		const colony::ThingView* Me = Snapshot(State->Me);
		UWeaponDrawSubsystem* W = Weapons();
		if (!Me)
		{
			return;
		}
		if (!State->bSent && PhaseTime > 0.3f)
		{
			State->bSent = true;
			if (!W->SwitchTo(static_cast<int32>(colony::WeaponSlot::Primary)))
			{
				Fail(TEXT("switch_frozen: could not start a switch to the rifle"));
			}
		}
		if (Me->switchSlot == static_cast<uint8_t>(colony::WeaponSlot::Primary))
		{
			State->SwitchTotal = FMath::Max(State->SwitchTotal, Me->switchStepsLeft);
			if (!State->bPaused && State->SwitchTotal > 0 && Me->switchStepsLeft * 4 <= State->SwitchTotal)
			{
				State->bPaused = true;
				Bridge()->SetSpeed(0);
				if (const UStaticMeshComponent* Rifle = W->GetSlotComponent(State->Me, 0))
				{
					State->PausedRifle = Rifle->GetRelativeLocation();
				}
			}
		}
	}, [=, this]()
	{
		if (!State->bPaused)
		{
			Fail(TEXT("switch_frozen: the switch never reached three quarters"));
			Bridge()->SetSpeed(1);
			return;
		}
		const float FromHands = FVector::Dist(State->PausedRifle, MeleeSmokeHands);
		const float FromBack = FVector::Dist(State->PausedRifle, MeleeSmokeRifleHolster);
		if (FromHands < 8.0f || FromBack < 8.0f)
		{
			Fail(FString::Printf(TEXT("switch_frozen: the rifle is not part way (%.0f cm from the hand, %.0f cm from the back)"), FromHands, FromBack));
		}
		Notes.Add(FString::Printf(TEXT("switch_frozen: stopped with the rifle %.0f cm from the hand and %.0f cm from the back"), FromHands, FromBack));
		Bridge()->SetSpeed(1);
	});

	// 5. The melee weapon comes to the hand (Q again from the rifle goes to the sidearm; the melee slot by number).
	AddPhase(TEXT("draw_melee"), [=, this]()
	{
		State->bSent = false;
		Bridge()->SetSpeed(1);
		MarkBaselines();
	}, 1.5f, 1.5f, [=, this]()
	{
		BodyView(State->Me, 200.0, 110.0, 140.0);
		const colony::ThingView* Me = Snapshot(State->Me);
		if (Me && !State->bSent && Me->switchSlot > 2 && PhaseTime > 0.3f)
		{
			State->bSent = true;
			if (!Weapons()->SwitchTo(static_cast<int32>(colony::WeaponSlot::Melee)))
			{
				Fail(TEXT("draw_melee: could not start a switch to the melee weapon"));
			}
		}
	}, [=, this]()
	{
		const colony::ThingView* Me = Snapshot(State->Me);
		UWeaponDrawSubsystem* W = Weapons();
		if (!Me || !Me->weapon.valid() || Me->weapon != Me->carried[2])
		{
			Fail(TEXT("draw_melee: the gladius is not in the hands"));
			return;
		}
		const UStaticMeshComponent* Blade = W->GetHandsComponent(State->Me);
		if (!Blade || Blade != W->GetSlotComponent(State->Me, 2))
		{
			Fail(TEXT("draw_melee: the weapon in the hands is not the gladius's mesh"));
			return;
		}
		// At rest the blade is held at the side, tip up and forward.
		if (!MeleeSmokeInHand(Blade, FVector(24.0, 20.0, 100.0), 4.0))
		{
			Fail(FString::Printf(TEXT("draw_melee: the gladius rests at %s, not in the hand"), *MeleeSmokeActorSpace(*Blade).ToString()));
		}
		if (W->IsGun(Me->weapon))
		{
			Fail(TEXT("draw_melee: the gladius counts as a gun"));
		}
		// The fire button does nothing with a blade in the hands.
		if (Fight()->Fire() || Fight()->GetFireOrders() != State->FireOrdersAtStart)
		{
			Fail(TEXT("draw_melee: the fire button shot with a blade in the hands"));
		}
	});

	// 6. An enemy one metre ahead. The protagonist winds up a swing and the game stops near the top of the wind-up.
	AddPhase(TEXT("swing_windup"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		State->bSent = false;
		State->bPaused = false;
		State->bEnemyMade = false;
		State->WindupZ = 0.0f;
		B->SetSpeed(1);
		MarkBaselines();
		const colony::ThingView* Me = Snapshot(State->Me);
		if (!Me)
		{
			return;
		}
		State->Cell = colony::CellPos{FMath::FloorToInt32(Me->x), FMath::FloorToInt32(Me->y), 0};
		// Stand in the middle of the cell so the enemy in the next one is exactly a metre away.
		Prot()->GetBody()->SetSimPosition(FVector2D(State->Cell.x + 0.5, State->Cell.y + 0.5));
		B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{State->Cell.x + 1, State->Cell.y, 0}, 1});
	}, 1.0f, 3.5f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		const colony::ThingView* Me = Snapshot(State->Me);
		ASimPawnActor* Actor = ActorOf(State->Me);
		UWeaponDrawSubsystem* W = Weapons();
		if (!Me || !Actor)
		{
			return;
		}
		if (State->Enemy == colony::kNoEntity)
		{
			State->Enemy = MeleeSmokePawnAt(*B, State->Cell.x + 1.5f, State->Cell.y + 0.5f, State->Me, colony::kNoEntity);
			if (State->Enemy != colony::kNoEntity)
			{
				B->Submit(colony::SetFactionCommand{State->Enemy, 2});
			}
			return;
		}
		HoldDummy();
		SideView(State->Me, State->Enemy, 260.0, 130.0);
		if (State->bPaused)
		{
			return;
		}
		if (!State->bSent && PhaseTime > 0.5f)
		{
			State->bSent = true;
			if (!W->Swing())
			{
				Fail(TEXT("swing_windup: the swing key did not start a swing"));
			}
			return;
		}
		const FMeleeSwingTrack& Track = Actor->GetSwing();
		if (State->bSent && Track.Phase == FMeleeSwingTrack::EPhase::DrawBack && Track.Clock >= Track.Length * 0.85f)
		{
			State->bPaused = true;
			B->SetSpeed(0);
			FVector Location;
			FRotator Rotation;
			Actor->GetMeleeWeaponPose(Location, Rotation);
			State->WindupZ = static_cast<float>(Location.Z);
			State->WindupLean = Track.Current.Lean;
			State->SwingPhaseAtPause = Me->meleeTarget == State->Enemy ? TEXT("at the enemy") : TEXT("at no one");
		}
	}, [=, this]()
	{
		UWeaponDrawSubsystem* W = Weapons();
		const colony::ThingView* Me = Snapshot(State->Me);
		if (State->Enemy == colony::kNoEntity)
		{
			Fail(TEXT("swing_windup: the enemy never appeared"));
			return;
		}
		if (!State->bPaused)
		{
			Fail(TEXT("swing_windup: the swing never reached the top of its wind-up"));
			Bridge()->SetSpeed(1);
			return;
		}
		if (W->GetSwingOrders() - State->OrdersAtStart != 1 || W->GetSwingsSeen() - State->SwingsAtStart < 1)
		{
			Fail(FString::Printf(TEXT("swing_windup: %d swing orders and %d swings seen, expected 1 and at least 1"), W->GetSwingOrders() - State->OrdersAtStart, W->GetSwingsSeen() - State->SwingsAtStart));
		}
		if (State->SwingPhaseAtPause != TEXT("at the enemy"))
		{
			Fail(TEXT("swing_windup: the swing was not aimed at the enemy standing in front (the swing key's target)"));
		}
		if (!Me || Me->meleePhase != colony::MeleePhase::Windup)
		{
			Fail(TEXT("swing_windup: the sim does not have the protagonist winding up"));
		}
		if (State->WindupZ < 135.0f || State->WindupLean > -3.0f)
		{
			Fail(FString::Printf(TEXT("swing_windup: the blade is %.0f cm up and the body leans %.1f degrees (expected above 135 and back of -3)"), State->WindupZ, State->WindupLean));
		}
		Notes.Add(FString::Printf(TEXT("swing_windup: stopped at the top of the wind-up, blade %.0f cm up, body leaning %.1f degrees"), State->WindupZ, State->WindupLean));
	});

	// 7. The blow lands: the game stops just after the hit moment.
	AddPhase(TEXT("swing_blow"), [=, this]()
	{
		State->bPaused = false;
		State->WindupZ = 0.0f;
		Bridge()->SetSpeed(1);
		MarkBaselines();
	}, 0.5f, 3.5f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		ASimPawnActor* Actor = ActorOf(State->Me);
		UWeaponDrawSubsystem* W = Weapons();
		HoldDummy();
		if (!Actor || State->bPaused)
		{
			return;
		}
		SideView(State->Me, State->Enemy, 260.0, 130.0);
		const FMeleeSwingTrack& Track = Actor->GetSwing();
		if (W->GetHitsSeen() + W->GetDeflectedSeen() > State->HitsAtStart && Track.Phase == FMeleeSwingTrack::EPhase::Follow)
		{
			State->bPaused = true;
			B->SetSpeed(0);
			FVector Location;
			FRotator Rotation;
			Actor->GetMeleeWeaponPose(Location, Rotation);
			State->WindupZ = static_cast<float>(Location.Z);
			State->WindupLean = Track.Current.Lean;
		}
	}, [=, this]()
	{
		UWeaponDrawSubsystem* W = Weapons();
		UFightSubsystem* F = Fight();
		const ASimPawnActor* Me = ActorOf(State->Me);
		const ASimPawnActor* Enemy = ActorOf(State->Enemy);
		if (!State->bPaused)
		{
			Fail(TEXT("swing_blow: no blow landed (no hit moment with a pawn struck)"));
			Bridge()->SetSpeed(1);
			return;
		}
		if (State->WindupZ > 115.0f || State->WindupLean < 8.0f)
		{
			Fail(FString::Printf(TEXT("swing_blow: at the blow the blade is %.0f cm up and the body leans %.1f degrees forward (expected under 115 and over 8)"), State->WindupZ, State->WindupLean));
		}
		if (!Me || !Me->IsHitStopped() || (Enemy && !Enemy->IsHitStopped()))
		{
			Fail(TEXT("swing_blow: no hit-stop on the attacker or the struck pawn at the moment of the blow"));
		}
		if (W->GetHitStopsPlayed() - State->StopsAtStart < 1)
		{
			Fail(TEXT("swing_blow: the hit played no hit-stop"));
		}
		if (F->GetImpactsOfKind(colony::ProjectileHitKind::Pawn) - State->ImpactsAtStart < 1 && F->GetImpactsOfKind(colony::ProjectileHitKind::Armor) < 1)
		{
			Fail(TEXT("swing_blow: the blow made no blood or sparks"));
		}
		if (!Enemy || Enemy->GetHitReactions() < 1)
		{
			Fail(TEXT("swing_blow: the struck pawn did not flinch"));
		}
		Notes.Add(FString::Printf(TEXT("swing_blow: stopped on the blow, blade %.0f cm up, body leaning %.1f degrees forward, enemy flinches %d"), State->WindupZ, State->WindupLean, Enemy ? Enemy->GetHitReactions() : 0));
		Bridge()->SetSpeed(1);
	});

	// 8. A run of swings at normal speed: each blow lands on the sim's step.
	AddPhase(TEXT("swing_flow"), [=, this]()
	{
		State->MaxLean = 0.0f;
		State->LastHealSeconds = 0.0f;
		Bridge()->SetSpeed(1);
		MarkBaselines();
	}, 0.5f, 9.0f, [=, this]()
	{
		ASimPawnActor* Actor = ActorOf(State->Me);
		if (!Actor)
		{
			return;
		}
		SideView(State->Me, State->Enemy, 260.0, 130.0);
		KeepAlive();
		HoldDummy();
		Weapons()->Swing();
		State->MaxLean = FMath::Max(State->MaxLean, Actor->GetMeleeBodyDegrees());
	}, [=, this]()
	{
		UWeaponDrawSubsystem* W = Weapons();
		UFightSubsystem* F = Fight();
		const ASimPawnActor* Enemy = ActorOf(State->Enemy);
		const int32 Swings = W->GetSwingsSeen() - State->SwingsAtStart;
		const int32 Hits = W->GetHitsSeen() - State->HitsAtStart;
		const int32 OnTime = W->GetHitsOnTime() - State->OnTimeAtStart;
		const int32 Late = W->GetHitsLate() - State->LateAtStart;
		if (Swings < 4 || Hits < 3)
		{
			Fail(FString::Printf(TEXT("swing_flow: %d swings and %d hits in 9 s, expected at least 4 and 3"), Swings, Hits));
		}
		if (OnTime < Hits || Late > 0)
		{
			Fail(FString::Printf(TEXT("swing_flow: %d blows landed on time and %d late (the picture was up to %.1f steps behind), expected all on time"), OnTime, Late, W->GetMaxStepsBehind()));
		}
		if (W->GetHitStopsPlayed() - State->StopsAtStart < Hits)
		{
			Fail(TEXT("swing_flow: a hit played no hit-stop"));
		}
		if (F->GetImpactsOfKind(colony::ProjectileHitKind::Pawn) - State->ImpactsAtStart < 2 || !Enemy || Enemy->GetHitReactions() < 3)
		{
			Fail(TEXT("swing_flow: a hit made no blood or no flinch"));
		}
		if (State->MaxLean < 20.0f)
		{
			Fail(FString::Printf(TEXT("swing_flow: the body hardly moved in the swings (%.1f degrees)"), State->MaxLean));
		}
		Notes.Add(FString::Printf(TEXT("swing_flow: %d swings, %d hits, %d on time, %d late, %d hit-stops, enemy flinched %d times, body bent up to %.0f degrees"), Swings, Hits, OnTime, Late,
			W->GetHitStopsPlayed() - State->StopsAtStart, Enemy ? Enemy->GetHitReactions() : 0, State->MaxLean));
	});

	// 9. The right mouse button: the blade comes across the chest and stays there.
	AddPhase(TEXT("block"), [=, this]()
	{
		State->bSent = false;
		State->bPaused = false;
		State->MaxBlock = 0.0f;
		State->WindupZ = 0.0f;
		MarkBaselines();
		Bridge()->SetSpeed(1);
		// The enemy has made its point.
		if (Bridge()->GetSim())
		{
			MeleeSmokeHeal(*const_cast<colony::Sim*>(Bridge()->GetSim()), State->Me);
		}
	}, 0.5f, 3.5f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		const colony::ThingView* Me = Snapshot(State->Me);
		ASimPawnActor* Actor = ActorOf(State->Me);
		if (!Me || !Actor)
		{
			return;
		}
		BodyView(State->Me, 150.0, 170.0, 130.0);
		if (State->bPaused)
		{
			return;
		}
		if (!State->bSent && Me->meleePhase == colony::MeleePhase::Ready)
		{
			State->bSent = Weapons()->Block();
		}
		State->MaxBlock = FMath::Max(State->MaxBlock, Actor->GetBlockBlend());
		if (State->bSent && Me->blocking && Actor->GetBlockBlend() >= 0.99f)
		{
			State->bPaused = true;
			B->SetSpeed(0);
		}
	}, [=, this]()
	{
		UWeaponDrawSubsystem* W = Weapons();
		ASimPawnActor* Actor = ActorOf(State->Me);
		if (!State->bPaused || !Actor)
		{
			Fail(FString::Printf(TEXT("block: the blade never came all the way up (most %.2f)"), State->MaxBlock));
			Bridge()->SetSpeed(1);
			return;
		}
		FVector Location;
		FRotator Rotation;
		Actor->GetMeleeWeaponPose(Location, Rotation);
		if (W->GetBlockActions() - State->BlocksAtStart < 1)
		{
			Fail(TEXT("block: the sim reported no raised weapon (PawnAction Block)"));
		}
		if (FMath::Abs(Location.Y) > 6.0 || Location.Z < 108.0 || FMath::Abs(FRotator::NormalizeAxis(static_cast<float>(Rotation.Yaw)) - 90.0f) > 12.0f)
		{
			Fail(FString::Printf(TEXT("block: the blade is held at %s turned %s, expected across the chest (y near 0, z above 108, turned 90 degrees)"), *Location.ToString(), *Rotation.ToString()));
		}
		Notes.Add(FString::Printf(TEXT("block: the blade is across the chest at %s"), *Location.ToString()));
		Bridge()->SetSpeed(1);
	});

	// 10. A duel on its own: two sides with melee weapons close in, swing, block and dodge, at three times speed.
	AddPhase(TEXT("duel"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		State->bStaged = false;
		State->bSent = false;
		State->Attacker = colony::kNoEntity;
		State->Defender = colony::kNoEntity;
		State->MaxBlock = 0.0f;
		State->LastHealSeconds = 0.0f;
		// Out of the way of the protagonist and the enemy.
		const colony::CellPos A{State->Cell.x + 12, State->Cell.y - 30, 0};
		B->Submit(colony::SpawnThingCommand{"Human", A, 1});
		B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{A.x + 1, A.y, 0}, 1});
		B->Submit(colony::SpawnThingCommand{"Melee_Club", colony::CellPos{A.x, A.y + 2, 0}, 1});
		B->Submit(colony::SpawnThingCommand{"Melee_Gladius", colony::CellPos{A.x + 1, A.y + 2, 0}, 1});
		// The enemy and the protagonist stand easy.
		MeleeSmokeHeal(*const_cast<colony::Sim*>(B->GetSim()), State->Me);
		MarkBaselines();
		B->SetSpeed(1);
	}, 0.5f, 26.0f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		const colony::CellPos A{State->Cell.x + 12, State->Cell.y - 30, 0};
		if (!State->bStaged)
		{
			if (State->Attacker == colony::kNoEntity)
			{
				State->Attacker = MeleeSmokePawnAt(*B, A.x + 0.5f, A.y + 0.5f, State->Enemy, colony::kNoEntity);
			}
			if (State->Defender == colony::kNoEntity)
			{
				State->Defender = MeleeSmokePawnAt(*B, A.x + 1.5f, A.y + 0.5f, State->Enemy, State->Attacker);
			}
			const colony::Entity Club = MeleeSmokeNearestItem(*B, "Melee_Club", A.x + 0.5f, A.y + 2.5f, 1.5f);
			const colony::Entity Blade = MeleeSmokeNearestItem(*B, "Melee_Gladius", A.x + 1.5f, A.y + 2.5f, 1.5f);
			if (State->Attacker == colony::kNoEntity || State->Defender == colony::kNoEntity || Club == colony::kNoEntity || Blade == colony::kNoEntity)
			{
				return;
			}
			State->bStaged = true;
			colony::Sim& Sim = *const_cast<colony::Sim*>(B->GetSim());
			MeleeSmokeSkill(Sim, State->Attacker, 20);
			MeleeSmokeSkill(Sim, State->Defender, 20);
			B->Submit(colony::EquipCommand{State->Attacker, Club});
			B->Submit(colony::EquipCommand{State->Defender, Blade});
			B->Submit(colony::SetFactionCommand{State->Attacker, 2});
			B->Submit(colony::SetFactionCommand{State->Defender, 3});
			B->SetSpeed(3);
			return;
		}
		SideView(State->Attacker, State->Defender, 330.0, 150.0);
		KeepAlive();
		// The attacker swings whenever it is ready; the two are put back side by side if a sidestep took them out of reach.
		colony::Sim& Sim = *const_cast<colony::Sim*>(B->GetSim());
		if (B->GetGameSeconds() > State->LastOrderStep && Sim.melee().ready(State->Attacker) && Sim.things().spawned(State->Defender))
		{
			if (!Sim.melee().inReach(State->Attacker, State->Defender))
			{
				entt::registry& Registry = Sim.things().registry();
				colony::Position& AttackerAt = Registry.get<colony::Position>(State->Attacker);
				colony::Position& DefenderAt = Registry.get<colony::Position>(State->Defender);
				AttackerAt.x = A.x + 0.5f;
				AttackerAt.y = A.y + 0.5f;
				DefenderAt.x = A.x + 1.5f;
				DefenderAt.y = A.y + 0.5f;
				Registry.get<colony::Mover>(State->Defender).path.clear();
				Registry.get<colony::Mover>(State->Defender).next = 0;
				Registry.get<colony::Mover>(State->Attacker).path.clear();
				Registry.get<colony::Mover>(State->Attacker).next = 0;
			}
			B->Submit(colony::MeleeCommand{State->Attacker, State->Defender, colony::MeleeMode::Attack});
			State->LastOrderStep = B->GetGameSeconds();
		}
		if (const ASimPawnActor* Defender = ActorOf(State->Defender))
		{
			State->MaxBlock = FMath::Max(State->MaxBlock, Defender->GetBlockBlend());
		}
	}, [=, this]()
	{
		UWeaponDrawSubsystem* W = Weapons();
		Bridge()->SetSpeed(1);
		if (!State->bStaged)
		{
			Fail(TEXT("duel: the two fighters were never set up"));
			return;
		}
		const int32 Swings = W->GetSwingsSeen() - State->SwingsAtStart;
		const int32 Dodges = W->GetDodgeActions() - State->DodgesAtStart;
		const int32 Blocked = W->GetBlockedSeen() - State->BlockedAtStart;
		const int32 Blocks = W->GetBlockActions() - State->BlocksAtStart;
		int32 DodgesPlayed = 0;
		for (const colony::Entity Pawn : {State->Attacker, State->Defender})
		{
			if (const ASimPawnActor* Actor = ActorOf(Pawn))
			{
				DodgesPlayed += Actor->GetDodgesPlayed();
			}
		}
		if (Swings < 8)
		{
			Fail(FString::Printf(TEXT("duel: only %d swings in 26 s of a duel"), Swings));
		}
		if (Dodges < 1 || DodgesPlayed < 1)
		{
			Fail(FString::Printf(TEXT("duel: %d sidesteps reported by the sim and %d played on screen"), Dodges, DodgesPlayed));
		}
		if (Blocks < 1 || State->MaxBlock < 0.9f)
		{
			Fail(FString::Printf(TEXT("duel: %d raised weapons reported by the sim, the blade came up %.2f of the way"), Blocks, State->MaxBlock));
		}
		if (W->GetHitsSeen() - State->HitsAtStart + Blocked < 1)
		{
			Fail(TEXT("duel: no blow landed or was blocked"));
		}
		Notes.Add(FString::Printf(TEXT("duel: %d swings, %d hits, %d blocked, %d deflected, %d dodged, %d missed; %d sidesteps (%d played), %d raised weapons, %d hit-stops"), Swings,
			W->GetHitsSeen() - State->HitsAtStart, Blocked, W->GetDeflectedSeen(), W->GetDodgedSeen(), W->GetMissedSeen(), Dodges, DodgesPlayed, Blocks,
			W->GetHitStopsPlayed() - State->StopsAtStart));
	});
	// 11. A shield (docs/design/shields.md): a steel medium shield on the protagonist's arm beside the gladius, the right
	// mouse button raises it facing the camera, a clubber beats on it (sparks), and letting go lowers it.
	AddPhase(TEXT("shield"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		colony::Sim& Sim = *const_cast<colony::Sim*>(B->GetSim());
		entt::registry& Registry = Sim.things().registry();
		State->bStaged = false;
		State->MaxShieldRaise = 0.0f;
		State->bShieldUpSeen = false;
		State->LastHealSeconds = 0.0f;
		State->ShieldOrdersAtStart = Weapons()->GetShieldOrders();
		State->ShieldBlocksAtStart = Weapons()->GetShieldBlocksSeen();
		State->SparksAtStart = Fight()->GetImpactsOfKind(colony::ProjectileHitKind::Armor);
		// Away from the duel, the protagonist facing east with the gladius in hand.
		const colony::CellPos A{State->Cell.x - 12, State->Cell.y - 30, 0};
		colony::Position& MeAt = Registry.get<colony::Position>(State->Me);
		MeAt.x = A.x + 0.5f;
		MeAt.y = A.y + 0.5f;
		Registry.get<colony::Mover>(State->Me).path.clear();
		Registry.get<colony::Mover>(State->Me).next = 0;
		const colony::ThingView* Me = Snapshot(State->Me);
		if (Me && Me->carried[2].valid() && Me->weapon != Me->carried[2])
		{
			B->Submit(colony::SwitchWeaponCommand{State->Me, colony::WeaponSlot::Melee});
		}
		std::string Why;
		State->Shield = Sim.apparel().make(Sim.defs().idOf("Things", "Apparel_ShieldMedium"), Sim.defs().idOf("Things", "Steel"), -1, A, &Why);
		if (State->Shield == colony::kNoEntity || !Sim.apparel().putOn(State->Me, State->Shield, Why))
		{
			Fail(FString::Printf(TEXT("shield: could not put a steel medium shield on the protagonist: %s"), UTF8_TO_TCHAR(Why.c_str())));
		}
		B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{A.x + 1, A.y, 0}, 1});
		B->Submit(colony::SpawnThingCommand{"Melee_Club", colony::CellPos{A.x + 1, A.y + 2, 0}, 1});
		MeleeSmokeHeal(Sim, State->Me);
		UProtagonistSubsystem* P = Prot();
		P->GetBody()->Look(FRotator::NormalizeAxis(-P->GetBody()->GetYaw()), -5.0f - P->GetBody()->GetPitch());
		B->SetSpeed(1);
	}, 1.5f, 10.0f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		const colony::ThingView* Me = Snapshot(State->Me);
		ASimPawnActor* Actor = ActorOf(State->Me);
		if (!B || !B->GetSim() || !Me || !Actor)
		{
			return;
		}
		colony::Sim& Sim = *const_cast<colony::Sim*>(B->GetSim());
		const colony::CellPos A{State->Cell.x - 12, State->Cell.y - 30, 0};
		BodyView(State->Me, 260.0, 120.0, 150.0);
		KeepAlive();
		MeleeSmokeHeal(Sim, State->Basher);
		// Held for the first 8 seconds, then let go.
		Weapons()->ScriptRightMouse(true, PhaseTime < 8.0f);
		State->MaxShieldRaise = FMath::Max(State->MaxShieldRaise, Weapons()->GetShieldRaise(State->Me));
		State->bShieldUpSeen = State->bShieldUpSeen || Me->shieldUp;
		if (Me->shieldUp && Weapons()->GetShieldRaise(State->Me) >= 0.99f)
		{
			if (const UStaticMeshComponent* Shield = Weapons()->GetShieldComponent(State->Me))
			{
				State->RaisedAt = MeleeSmokeActorSpace(*Shield);
			}
		}
		if (!State->bStaged)
		{
			State->Basher = MeleeSmokePawnAt(*B, A.x + 1.5f, A.y + 0.5f, State->Me, State->Enemy);
			const colony::Entity Club = MeleeSmokeNearestItem(*B, "Melee_Club", A.x + 1.5f, A.y + 2.5f, 1.5f);
			if (State->Basher == colony::kNoEntity || Club == colony::kNoEntity)
			{
				return;
			}
			State->bStaged = true;
			MeleeSmokeSkill(Sim, State->Basher, 20);
			B->Submit(colony::EquipCommand{State->Basher, Club});
			B->Submit(colony::SetFactionCommand{State->Basher, 2});
			return;
		}
		if (PhaseTime < 8.0f && B->GetGameSeconds() > State->LastOrderStep && Sim.melee().ready(State->Basher) && Sim.melee().inReach(State->Basher, State->Me))
		{
			B->Submit(colony::MeleeCommand{State->Basher, State->Me, colony::MeleeMode::Attack});
			State->LastOrderStep = B->GetGameSeconds();
		}
	}, [=, this]()
	{
		UWeaponDrawSubsystem* W = Weapons();
		W->ScriptRightMouse(false, false);
		const colony::ThingView* Me = Snapshot(State->Me);
		const UStaticMeshComponent* Shield = W->GetShieldComponent(State->Me);
		if (!Me || !Me->shield.valid() || Me->shieldSlung)
		{
			Fail(TEXT("shield: the snapshot does not show a shield on the protagonist's arm"));
			return;
		}
		if (!Shield || !Shield->IsVisible())
		{
			Fail(TEXT("shield: no shield is drawn on the protagonist"));
			return;
		}
		if (W->GetShieldOrders() - State->ShieldOrdersAtStart < 2)
		{
			Fail(FString::Printf(TEXT("shield: %d shield commands sent, expected at least raise and lower"), W->GetShieldOrders() - State->ShieldOrdersAtStart));
		}
		if (!State->bShieldUpSeen || State->MaxShieldRaise < 0.99f)
		{
			Fail(FString::Printf(TEXT("shield: the sim raised it: %s; drawn up %.2f of the way"), State->bShieldUpSeen ? TEXT("yes") : TEXT("no"), State->MaxShieldRaise));
		}
		// On the forearm, the block clip lifts it before the chest and face (docs/design/weapon-hold.md): the left arm
		// crosses the body, so it may sit a little right of the middle.
		if (State->RaisedAt.X < 30.0 || FMath::Abs(State->RaisedAt.Y) > 25.0 || State->RaisedAt.Z < 100.0 || State->RaisedAt.Z > 150.0)
		{
			Fail(FString::Printf(TEXT("shield: raised it sits at %s, expected in front of the chest or face"), *State->RaisedAt.ToString()));
		}
		if (Me->shieldUp || W->GetShieldRaise(State->Me) > 0.01f)
		{
			Fail(TEXT("shield: still up 2 seconds after the button was let go"));
		}
		// The real model (docs/design/shields.md "Models"), when the ShieldArt commandlet has made it: thin along the face
		// (x), about 60 cm wide and 75 cm tall for a medium shield.
		FString ModelNote = TEXT("plain shapes (the ShieldArt commandlet has not made the models)");
		if (const UStaticMesh* Mesh = Shield->GetStaticMesh(); Mesh && Mesh->GetPathName().Contains(TEXT("/Generated/Shields/")))
		{
			const FVector Size = Mesh->GetBoundingBox().GetSize();
			ModelNote = FString::Printf(TEXT("model %s, %.0f x %.0f x %.0f cm"), *Mesh->GetName(), Size.X, Size.Y, Size.Z);
			if (Size.X > Size.Y || Size.X > Size.Z || Size.Z < 60.0 || Size.Z > 95.0 || Size.Y < 45.0 || Size.Y > 75.0)
			{
				Fail(FString::Printf(TEXT("shield: the medium shield model is %s, expected thin along x, about 60 wide and 75 tall (turned or scaled wrong on import)"), *ModelNote));
			}
		}
		Notes.Add(FString::Printf(TEXT("shield: drawn as %s"), *ModelNote));
		const int32 Blocks = W->GetShieldBlocksSeen() - State->ShieldBlocksAtStart;
		const int32 Sparks = Fight()->GetImpactsOfKind(colony::ProjectileHitKind::Armor) - State->SparksAtStart;
		if (State->bStaged && (Blocks < 1 || Sparks < 1))
		{
			Fail(FString::Printf(TEXT("shield: %d blows landed on the shield, %d sparks"), Blocks, Sparks));
		}
		Notes.Add(FString::Printf(TEXT("shield: raised at %s, %d blows on the shield, %d sparks, %d commands"), *State->RaisedAt.ToString(), Blocks, Sparks,
			W->GetShieldOrders() - State->ShieldOrdersAtStart));
	});
}
