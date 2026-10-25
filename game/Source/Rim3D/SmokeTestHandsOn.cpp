// Smoke test `handson` (task U10): the controlled colonist mines, chops, picks up, delivers, builds and
// plans a building with the build ghost, driven the way the keys do, in the game, offscreen.
#include "SmokeTest.h"

#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

#include "HandsOnSubsystem.h"
#include "OrdersSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimPicking.h"
#include "SimSubsystem.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace
{
	const FVector HandsSmokeOrigin(300000.0, 0.0, 0.0);
	// The player stands at (64.5, 64.5); everything is 1.5 m from the edge of its cell, in a ring.
	const colony::CellPos HandsPlayerCell{64, 64, 0};
	const colony::CellPos HandsRockCell{66, 64, 0};   // east
	const colony::CellPos HandsTreeCell{64, 66, 0};   // north (+Y)
	const colony::CellPos HandsSteelCell{63, 64, 0};  // west, on the ground
	const colony::CellPos HandsPlanCell{64, 62, 0};   // south

	struct FHandsSmokeState
	{
		colony::Entity Pawn = colony::kNoEntity;
		colony::Entity Steel = colony::kNoEntity;
		colony::Entity Plan = colony::kNoEntity;
		colony::CellPos GhostCell;
		bool bAimed = false;
		bool bHeld = false;
		bool bDone = false;
		int32 FramesSinceAim = 0;
		int32 HeldFrames = 0;
		int32 Step = 0;
		int32 PromptFrames = 0;
		int32 ProgressFrames = 0;
		int32 CommandsAtStart = 0;
		int32 LogsBefore = 0;
	};

	int32 HandsSmokeCountItems(const colony::Sim& Sim, const char* DefName)
	{
		const colony::DefId Id = Sim.defs().idOf("Things", DefName);
		int32 Total = 0;
		Sim.things().registry().view<const colony::Thing, const colony::StackCount, const colony::Position>().each(
			[&](const colony::Thing& Thing, const colony::StackCount& Stack, const colony::Position&) { Total += Thing.def == Id ? Stack.count : 0; });
		return Total;
	}
}

void USmokeTestSubsystem::BuildHandsOnPhases()
{
	const TSharedRef<FHandsSmokeState> State = MakeShared<FHandsSmokeState>();

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
	const auto Hands = [this]() { return GetWorld()->GetSubsystem<UHandsOnSubsystem>(); };
	const auto Orders = [this]() { return GetWorld()->GetSubsystem<UOrdersSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };
	const auto Me = [=, this]() -> const colony::ThingView* { USimSubsystem* B = Bridge(); return B ? B->GetCurrentSnapshot().find(State->Pawn) : nullptr; };

	// Turns the first person view to face a direction (degrees; yaw 0 is +X, 90 is +Y) and pitch.
	const auto Aim = [=, this](float Yaw, float Pitch)
	{
		AProtagonistPawn* Body = Prot()->GetBody();
		Body->Look(FRotator::NormalizeAxis(Yaw - Body->GetYaw()), Pitch - Body->GetPitch());
		Body->SnapCamera();
		State->FramesSinceAim = 0;
	};
	// Presses F as the key does, once the crosshair has had a few frames on its target and the sim says it can be done.
	const auto HoldWhenReady = [=, this](colony::HandsOnKind Kind) -> bool
	{
		UHandsOnSubsystem* H = Hands();
		++State->FramesSinceAim;
		if (State->bHeld)
		{
			++State->HeldFrames;
			return true;
		}
		if (State->FramesSinceAim < 5)
		{
			return false;
		}
		if (H->GetPlan().kind != Kind || !H->GetPlan().ok())
		{
			if (State->FramesSinceAim == 30)
			{
				const colony::ThingView* Mine = Me();
				Fail(FString::Printf(TEXT("The crosshair plan was kind %d ok %d reach %d (%s) %s, wanted kind %d; target valid %d ground %d cell %d,%d; player at %.2f,%.2f"),
					static_cast<int32>(H->GetPlan().kind), H->GetPlan().ok() ? 1 : 0, H->GetPlan().inReach ? 1 : 0, UTF8_TO_TCHAR(H->GetPlan().label.c_str()),
					UTF8_TO_TCHAR(H->GetPlan().error.c_str()), static_cast<int32>(Kind), H->GetTarget().bValid ? 1 : 0, H->GetTarget().bGround ? 1 : 0,
					H->GetTarget().Cell.x, H->GetTarget().Cell.y, Mine ? Mine->x : -1.0f, Mine ? Mine->y : -1.0f));
			}
			return false;
		}
		H->SetInteractHeld(true);
		State->bHeld = true;
		State->HeldFrames = 0;
		return true;
	};
	const auto Release = [=, this]()
	{
		Hands()->SetInteractHeld(false);
		State->bHeld = false;
	};
	const auto Count = [=, this](UHandsOnSubsystem* H)
	{
		State->PromptFrames = H->GetStats().PromptFrames;
		State->ProgressFrames = H->GetStats().ProgressFrames;
	};

	// 1. A flat map, a colonist of ours in the middle, and the things around it.
	AddPhase(TEXT("setup"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Prot() || !Hands() || !Orders())
		{
			Fail(TEXT("A subsystem did not start"));
			return;
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 128;
		Options.bFlatMap = true;
		B->SetWorldOrigin(HandsSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		// Paused: one step makes the colonist, which would otherwise start working on its own before the player takes it over.
		B->SetSpeed(0);
		colony::Sim* Sim = SimOf();
		B->Submit(colony::SpawnThingCommand{"Human", HandsPlayerCell, 1, colony::Rot4::North});
		for (int32 Y = 56; Y < 72; ++Y)
		{
			for (int32 X = 56; X < 72; ++X)
			{
				Sim->plants().remove(colony::CellPos{X, Y, 0});
			}
		}
		if (Sim->mineables().rockTypes().empty())
		{
			Fail(TEXT("The data has no rock types"));
			return;
		}
		Sim->mineables().place(HandsRockCell, Sim->mineables().rockTypes()[0]);
		// Most of the rock is already gone, so two swings finish it.
		Sim->mineables().hit(HandsRockCell, Sim->mineables().hitPoints(HandsRockCell) - 100);
		Sim->plants().place(HandsTreeCell, Sim->defs().idOf("Things", "Plant_TreePoplar"), 1.0f);
		B->StepOnce();
		State->LogsBefore = HandsSmokeCountItems(*Sim, "WoodLog");
	}, 3.0f, 1.0f, nullptr, [=, this]()
	{
		colony::Sim* Sim = SimOf();
		if (!Sim)
		{
			return;
		}
		// The command-made colonist becomes a real colonist: a record and a work row, like the scenario gives.
		for (const colony::ThingView& View : Bridge()->GetCurrentSnapshot().things)
		{
			const FString Name = UTF8_TO_TCHAR(Sim->defs().get(View.def).defName().c_str());
			if (View.category == colony::ThingCategory::Pawn)
			{
				State->Pawn = View.id;
			}
		}
		if (State->Pawn != colony::kNoEntity)
		{
			colony::Colonist Record;
			for (const char* Skill : {"Mining", "Plants", "Construction"})
			{
				Record.skills.push_back(colony::SkillRecord{Skill, 20, colony::Passion::None, 0.0f, false});
			}
			Sim->things().registry().emplace_or_replace<colony::Colonist>(State->Pawn, Record);
			Sim->jobs().addWorker(State->Pawn, {}, false);
		}
		if (Me() == nullptr)
		{
			Fail(TEXT("The colonist did not get made"));
		}
	});

	// 2. Into direct mode, first person, face the rock and hold F.
	AddPhase(TEXT("mine_by_hand"), [=, this]()
	{
		if (!Prot()->EnterDirectMode(State->Pawn))
		{
			Fail(TEXT("Could not take control of the colonist"));
			return;
		}
		Bridge()->SetSpeed(1);
		Bridge()->Submit(colony::SpawnThingCommand{"Steel", HandsSteelCell, 10, colony::Rot4::North});
		Bridge()->Submit(colony::PlaceBlueprintCommand{"Wall", "Steel", HandsPlanCell, colony::Rot4::North});
		Prot()->GetBody()->SetFirstPerson(true);
		Hands()->SetScripted(true);
		Aim(0.0f, 0.0f);
		State->bHeld = false;
		State->CommandsAtStart = Hands()->GetStats().CommandsSent;
	}, 1.0f, 16.0f, [=, this]()
	{
		if (!Prot()->IsDirectMode() || !SimOf() || State->bDone)
		{
			return;
		}
		if (HoldWhenReady(colony::HandsOnKind::Mine))
		{
			Count(Hands());
			if (!SimOf()->mineables().has(HandsRockCell))
			{
				State->bDone = true;
				Release();
			}
		}
	}, [=, this]()
	{
		const colony::Sim* Sim = SimOf();
		if (!Sim)
		{
			return;
		}
		if (Sim->mineables().has(HandsRockCell))
		{
			Fail(TEXT("Holding F did not mine the rock out"));
		}
		if (Sim->designations().count() != 0)
		{
			Fail(TEXT("The mark the hands-on job made is still there"));
		}
		if (State->PromptFrames < 10 || State->ProgressFrames < 10)
		{
			Fail(FString::Printf(TEXT("The prompt drew %d frames and the progress bar %d"), State->PromptFrames, State->ProgressFrames));
		}
		if (Sim->stats().failedJobs != 0)
		{
			Fail(TEXT("A job failed"));
		}
		if (Hands()->GetStats().CommandsSent - State->CommandsAtStart < 2)
		{
			Fail(TEXT("The press and the release should be two commands"));
		}
		State->bDone = false;
	});

	// 3. Face the tree and chop it.
	AddPhase(TEXT("chop_by_hand"), [=, this]()
	{
		Release();
		Aim(90.0f, 0.0f);
	}, 1.0f, 30.0f, [=, this]()
	{
		if (!Prot()->IsDirectMode() || !SimOf() || State->bDone)
		{
			return;
		}
		if (HoldWhenReady(colony::HandsOnKind::Chop))
		{
			if (!SimOf()->plants().has(HandsTreeCell))
			{
				State->bDone = true;
				Release();
			}
		}
	}, [=, this]()
	{
		const colony::Sim* Sim = SimOf();
		if (!Sim || Sim->plants().has(HandsTreeCell))
		{
			Fail(TEXT("Holding F did not chop the tree down"));
			return;
		}
		if (HandsSmokeCountItems(*Sim, "WoodLog") <= State->LogsBefore)
		{
			Fail(TEXT("The tree gave no wood"));
		}
		State->bDone = false;
	});

	// 4. Look down at the steel and pick it up.
	AddPhase(TEXT("pick_up"), [=, this]()
	{
		Release();
		Aim(180.0f, -60.0f);
	}, 1.0f, 3.0f, [=, this]()
	{
		if (!Prot()->IsDirectMode() || !SimOf() || State->bDone)
		{
			return;
		}
		if (HoldWhenReady(colony::HandsOnKind::PickUp))
		{
			const colony::ThingView* Mine = Me();
			if (Mine && Mine->carriedCount > 0)
			{
				State->bDone = true;
				Release();
			}
		}
	}, [=, this]()
	{
		const colony::ThingView* Mine = Me();
		if (!Mine || Mine->carriedCount != 10 || HandsSmokeCountItems(*SimOf(), "Steel") != 0)
		{
			Fail(TEXT("The colonist did not pick up the 10 steel"));
		}
		State->bDone = false;
	});

	// 5. Turn to the plan and put the steel in.
	AddPhase(TEXT("deliver"), [=, this]()
	{
		State->Plan = SimOf()->construction().siteAt(HandsPlanCell);
		Release();
		Aim(270.0f, 0.0f);
	}, 1.0f, 3.0f, [=, this]()
	{
		if (!Prot()->IsDirectMode() || !SimOf() || State->bDone)
		{
			return;
		}
		if (HoldWhenReady(colony::HandsOnKind::Deliver))
		{
			const colony::ThingView* Mine = Me();
			if (Mine && Mine->carriedCount == 0)
			{
				State->bDone = true;
				Release();
			}
		}
	}, [=, this]()
	{
		const colony::Sim* Sim = SimOf();
		const colony::ConstructionSite* Site = Sim && Sim->things().registry().valid(State->Plan)
			? Sim->things().registry().try_get<colony::ConstructionSite>(State->Plan) : nullptr;
		if (!Site || !std::all_of(Site->materials.begin(), Site->materials.end(), [](const colony::MaterialCount& M) { return M.have >= M.needed; }))
		{
			Fail(TEXT("The plan did not get its steel"));
		}
		else if (HandsSmokeCountItems(*Sim, "Steel") != 5)
		{
			Fail(TEXT("The 5 steel the plan did not need should be on the ground"));
		}
		State->bDone = false;
	});

	// 6. Build it.
	AddPhase(TEXT("build_by_hand"), [=, this]()
	{
		Release();
		Aim(270.0f, 0.0f);
	}, 1.0f, 24.0f, [=, this]()
	{
		if (!Prot()->IsDirectMode() || !SimOf() || State->bDone)
		{
			return;
		}
		if (HoldWhenReady(colony::HandsOnKind::Build))
		{
			if (SimOf()->map().passage(HandsPlanCell) == colony::Passage::Blocked)
			{
				State->bDone = true;
				Release();
			}
		}
	}, [=, this]()
	{
		const colony::Sim* Sim = SimOf();
		if (!Sim || Sim->map().passage(HandsPlanCell) != colony::Passage::Blocked)
		{
			Fail(TEXT("Holding F did not finish the wall"));
		}
		State->bDone = false;
	});

	// 7. The build ghost: B, aim at the ground, see it, click.
	AddPhase(TEXT("build_ghost"), [=, this]()
	{
		Release();
		Hands()->SetScripted(true);
		Aim(315.0f, -35.0f);
		State->Step = 0;
		Hands()->ToggleBuilding();
		if (!Orders()->IsDirectBuilding() || !Orders()->SelectBuilding(TEXT("Wall")))
		{
			Fail(TEXT("B did not take the build ghost in hand"));
		}
	}, 1.0f, 4.0f, [=, this]()
	{
		++State->FramesSinceAim;
		if (State->Step == 0 && State->FramesSinceAim > 60)
		{
			State->Step = 1;
			colony::CellPos Cell;
			if (!Hands()->GetAimCell(Cell))
			{
				Fail(TEXT("The crosshair found no ground for the ghost"));
				return;
			}
			State->GhostCell = Cell;
			if (Orders()->GetStats().GhostValid + Orders()->GetStats().GhostInvalid < 1)
			{
				Fail(TEXT("No ghost was drawn"));
			}
			if (!Hands()->PlaceBuilding())
			{
				Fail(TEXT("The click did not plan the wall"));
			}
		}
	}, [=, this]()
	{
		const colony::Sim* Sim = SimOf();
		if (!Sim || Sim->construction().siteAt(State->GhostCell) == colony::kNoEntity)
		{
			Fail(TEXT("There is no plan where the ghost was"));
		}
		Orders()->CancelTool();
	});

	// 8. Pick up again, put it down where the crosshair is, and Tab out.
	AddPhase(TEXT("put_down_and_leave"), [=, this]()
	{
		Bridge()->Submit(colony::SpawnThingCommand{"Steel", HandsSteelCell, 4, colony::Rot4::North});
		Aim(180.0f, -60.0f);
		State->bDone = false;
		State->bHeld = false;
		State->Step = 1;
	}, 1.0f, 4.0f, [=, this]()
	{
		if (!Prot()->IsDirectMode() || !SimOf())
		{
			return;
		}
		++State->FramesSinceAim;
		if (State->Step == 1 && State->FramesSinceAim > 8)
		{
			State->Step = 2;
			Hands()->SetInteractHeld(true);
		}
		else if (State->Step == 2)
		{
			if (const colony::ThingView* Mine = Me(); Mine && Mine->carriedCount > 0)
			{
				State->Step = 3;
				Hands()->SetInteractHeld(false);
				Aim(180.0f, -60.0f);
			}
		}
		else if (State->Step == 3 && State->FramesSinceAim > 8)
		{
			State->Step = 4;
			Hands()->PutDown();
		}
	}, [=, this]()
	{
		const colony::ThingView* Mine = Me();
		if (!Mine || Mine->carriedCount != 0 || HandsSmokeCountItems(*SimOf(), "Steel") != 5 + 4)
		{
			Fail(TEXT("Putting the steel down did not leave it on the ground"));
		}
		Prot()->ToggleMode();  // Tab
		Hands()->SetScripted(false);
		if (Prot()->IsDirectMode())
		{
			Fail(TEXT("Tab did not give the colonist back"));
		}
	});
}
