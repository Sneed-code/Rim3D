// Smoke test `animals` (task U15): the sim's animals drawn by USimAnimalSubsystem on their A4 models, in the game, offscreen.
#include "SmokeTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "HAL/IConsoleManager.h"

#include "AnimalActor.h"
#include "AnimalSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Plants.h"

#include <string>

namespace
{
	const FVector AnimalSmokeOrigin(-900000.0, 0.0, 0.0);
	// Every def A4 has art for, the realistic Sketchfab ones (monkey, cat, cougar), plus the grizzly and chicken.
	const TCHAR* const AnimalSmokeDefs[] = {TEXT("Cow"), TEXT("Muffalo"), TEXT("Deer"), TEXT("Husky"), TEXT("TimberWolf"),
		TEXT("Alpaca"), TEXT("Cat"), TEXT("WildBoar"), TEXT("Cougar"), TEXT("GrizzlyBear"), TEXT("Chicken"), TEXT("Monkey"), TEXT("Elephant"), TEXT("Rhinoceros"), TEXT("Lion"), TEXT("Raptor"), TEXT("TRex")};
	constexpr float AnimalSmokeSpacing = 2.0f;
	// The realistic Sketchfab models, side by side in daylight for a close look (phase animals_close).
	const TCHAR* const AnimalSmokeCloseDefs[] = {TEXT("Monkey"), TEXT("Cat"), TEXT("Cougar"), TEXT("Elephant"), TEXT("Rhinoceros"), TEXT("Muffalo"), TEXT("Raptor"), TEXT("TRex")};

	// The 2026-11-22 batch that replaced the husky stand-in (phase animals_new): big ones in the back row, small ones in front.
	const TCHAR* const AnimalSmokeNewBigDefs[] = {TEXT("GrizzlyBear"), TEXT("PolarBear"), TEXT("Caribou"), TEXT("Muskox"), TEXT("Yak"), TEXT("Dromedary"),
		TEXT("Gazelle"), TEXT("Goat"), TEXT("Sheep"), TEXT("Megaspider"), TEXT("Spelopede"), TEXT("Boomalope"), TEXT("Panther"), TEXT("Cassowary")};
	const TCHAR* const AnimalSmokeNewSmallDefs[] = {TEXT("Capybara"), TEXT("Iguana"), TEXT("Chicken"), TEXT("Duck"), TEXT("Goose"), TEXT("Turkey"),
		TEXT("ArcticFox"), TEXT("ArcticWolf"), TEXT("Ostrich"), TEXT("Megascarab"), TEXT("Cobra"), TEXT("Boomrat")};

	// The remnant war machines on their own models (phase remnants, docs/design/codex-models.md): drawn by the animal
	// subsystem, not as people.
	const TCHAR* const AnimalSmokeRemnantDefs[] = {TEXT("Mech_Scyther"), TEXT("Mech_Lancer"), TEXT("Mech_Pikeman"), TEXT("Mech_Centipede")};

	struct FAnimalSmokeState
	{
		TSet<uint8> PosesSeen;
		TSet<FName> DefsDrawn;
		int32 Errors = 0;
		FString FirstError;
		int32 Frames = 0;

		void Reset()
		{
			PosesSeen.Reset();
			DefsDrawn.Reset();
			Errors = 0;
			FirstError.Reset();
			Frames = 0;
		}

		void Error(const FString& Message)
		{
			if (Errors++ == 0)
			{
				FirstError = Message;
			}
		}
	};

	void AnimalSmokeSetDistance(const TCHAR* Name, float Metres)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Metres, ECVF_SetByCode);
		}
	}
}

void USmokeTestSubsystem::BuildAnimalsPhases()
{
	const TSharedRef<FAnimalSmokeState> State = MakeShared<FAnimalSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto AnimalsOf = [this]() { return GetWorld()->GetSubsystem<USimAnimalSubsystem>(); };
	const auto Middle = [=]() { return Bridge()->GetSim()->stockpileCell().value_or(colony::CellPos{48, 48, 0}); };

	// Every frame: each animal in the snapshot has an actor with its mesh and a clip, no animal has a pawn actor.
	const auto CheckFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimAnimalSubsystem* Animals = AnimalsOf();
		USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
		if (!B || !B->GetSim() || !Animals || PhaseTime < 2.0f)
		{
			return;
		}
		++State->Frames;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Pawn || View.lifeStage < 0)
			{
				continue;
			}
			if (Pawns && Pawns->FindPawn(View.id))
			{
				State->Error(TEXT("an animal is also drawn as a pawn"));
			}
			const ASimAnimalActor* Animal = Animals->FindAnimal(View.id);
			if (!Animal)
			{
				State->Error(TEXT("an animal has no actor"));
				continue;
			}
			const USkeletalMeshComponent* Body = Animal->GetBody();
			if (!Body || !Body->GetSkeletalMeshAsset() || !Animal->GetClip())
			{
				State->Error(TEXT("an animal has no mesh or no clip playing"));
				continue;
			}
			State->PosesSeen.Add(static_cast<uint8>(Animal->GetPose()));
			State->DefsDrawn.Add(FName(UTF8_TO_TCHAR(B->GetDefs()->get(View.def).defName().c_str())));
			// Drawn about as big as an animal: the whole body between 15 cm and 13 m long (the 12 m T-rex).
			const double Size = Animal->GetDrawLengthCm();
			if (Size < 15.0 || Size > 1300.0)
			{
				State->Error(FString::Printf(TEXT("an animal is %.0f cm across (draw scale %.2f)"), Size, Animal->GetDrawScale()));
			}
		}
	};
	const auto VerifyFrames = [=, this](const TCHAR* Phase)
	{
		if (State->Frames == 0)
		{
			Fail(FString::Printf(TEXT("%s: no frames were checked"), Phase));
		}
		if (State->Errors > 0)
		{
			Fail(FString::Printf(TEXT("%s: %d problems; the first: %s"), Phase, State->Errors, *State->FirstError));
		}
	};

	// 1. A row of animals, one of each def, seen side on. Speed 3 so they walk about.
	FPhase Close;
	Close.Name = TEXT("animals_row");
	Close.WarmupSeconds = 5.0f;
	Close.MeasureSeconds = 8.0f;
	Close.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !AnimalsOf())
		{
			Fail(TEXT("USimSubsystem or USimAnimalSubsystem did not start"));
			return;
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 96;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(AnimalSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		// Noon, and no trees or bushes in the way of the camera.
		AnimalSmokeSetDistance(TEXT("Sky.Hour"), 12.0f);
		AnimalSmokeSetDistance(TEXT("Sky.DayOfYear"), 22.5f);
		if (colony::Sim* Sim = const_cast<colony::Sim*>(B->GetSim()))
		{
			for (int32 Y = 0; Y < Options.MapSize; ++Y)
			{
				for (int32 X = 0; X < Options.MapSize; ++X)
				{
					const colony::CellPos C{X, Y, 0};
					if (Sim->plants().has(C) && Sim->defs().get(Sim->plants().defAt(C)).defName() != "Plant_Grass")
					{
						Sim->plants().remove(C);
					}
				}
			}
		}
		const colony::CellPos M = Middle();
		int32 I = 0;
		for (const TCHAR* Def : AnimalSmokeDefs)
		{
			B->Submit(colony::SpawnThingCommand{std::string(TCHAR_TO_UTF8(Def)), colony::CellPos{M.x + 6, M.y - 11 + I++ * 2, 0}});
		}
		B->StepOnce();
		B->SetSpeed(3);
		const FVector Target = B->SimToWorld(M.x + 6.0f, M.y, 0) + FVector(0.0, 0.0, 100.0);
		const FVector From = Target + FVector(-1700.0, 0.0, 500.0);
		ViewFrom(From, (Target - From).Rotation());
		AnimalSmokeSetDistance(TEXT("Animals.NearDistance"), 40.0f);
		AnimalSmokeSetDistance(TEXT("Animals.MidDistance"), 120.0f);
		AnimalSmokeSetDistance(TEXT("Animals.HideDistance"), 400.0f);
		State->Reset();
	};
	Close.OnFrame = CheckFrame;
	Close.Verify = [=, this]()
	{
		USimAnimalSubsystem* Animals = AnimalsOf();
		USimSubsystem* B = Bridge();
		if (!Animals || !B || !B->GetSim())
		{
			return;
		}
		VerifyFrames(TEXT("animals_row"));
		int32 Count = 0;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			Count += (View.category == colony::ThingCategory::Pawn && View.lifeStage >= 0) ? 1 : 0;
		}
		Notes.Add(FString::Printf(TEXT("animals_row: %d animals, %d actors, %d defs drawn, %d models loaded, poses seen %d"), Count,
			Animals->GetAnimals().Num(), State->DefsDrawn.Num(), Animals->GetModelsLoaded(), State->PosesSeen.Num()));
		if (Count < UE_ARRAY_COUNT(AnimalSmokeDefs) || Animals->GetAnimals().Num() != Count)
		{
			Fail(FString::Printf(TEXT("animals_row: %d animals in the snapshot but %d actors"), Count, Animals->GetAnimals().Num()));
		}
		if (State->DefsDrawn.Num() < UE_ARRAY_COUNT(AnimalSmokeDefs))
		{
			Fail(FString::Printf(TEXT("animals_row: only %d of %d defs were drawn"), State->DefsDrawn.Num(), static_cast<int32>(UE_ARRAY_COUNT(AnimalSmokeDefs))));
		}
		if (State->PosesSeen.Num() < 2)
		{
			Fail(TEXT("animals_row: every animal stayed in one pose (no idle and walk)"));
		}
	};
	Phases.Add(MoveTemp(Close));

	// 2. Animation detail by distance: the camera at one end of the row with the distances pulled in.
	FPhase Detail;
	Detail.Name = TEXT("animals_detail");
	Detail.WarmupSeconds = 3.0f;
	Detail.MeasureSeconds = 4.0f;
	Detail.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		B->SetSpeed(1);
		AnimalSmokeSetDistance(TEXT("Animals.NearDistance"), 6.0f);
		AnimalSmokeSetDistance(TEXT("Animals.MidDistance"), 14.0f);
		AnimalSmokeSetDistance(TEXT("Animals.HideDistance"), 60.0f);
		const colony::CellPos M = Middle();
		const FVector From = B->SimToWorld(M.x + 4.0f, M.y - 14.0f, 0) + FVector(0.0, 0.0, 250.0);
		const FVector Target = B->SimToWorld(M.x + 6.0f, M.y + 8.0f, 0) + FVector(0.0, 0.0, 60.0);
		ViewFrom(From, (Target - From).Rotation());
		State->Reset();
	};
	Detail.OnFrame = CheckFrame;
	Detail.Verify = [=, this]()
	{
		USimAnimalSubsystem* Animals = AnimalsOf();
		if (!Animals)
		{
			return;
		}
		VerifyFrames(TEXT("animals_detail"));
		const FSimAnimalDetailCounts& C = Animals->GetDetailCounts();
		Notes.Add(FString::Printf(TEXT("animals_detail: near %d, mid %d, far %d, hidden %d"), C.Near, C.Mid, C.Far, C.Hidden));
		if (C.Near == 0 || C.Mid == 0 || C.Far == 0)
		{
			Fail(TEXT("animals_detail: the animals are not spread over near, mid and far detail"));
		}
		for (const TPair<uint32, TObjectPtr<ASimAnimalActor>>& Pair : Animals->GetAnimals())
		{
			const ASimAnimalActor* Animal = Pair.Value;
			const bool bFrozen = Animal->GetBody()->bNoSkeletonUpdate;
			if ((Animal->GetDetail() == ESimAnimalDetail::Far || Animal->GetDetail() == ESimAnimalDetail::Hidden) != bFrozen)
			{
				Fail(TEXT("animals_detail: far animals should hold a frozen pose and near ones animate"));
				break;
			}
		}
		AnimalSmokeSetDistance(TEXT("Animals.NearDistance"), 40.0f);
		AnimalSmokeSetDistance(TEXT("Animals.MidDistance"), 120.0f);
		AnimalSmokeSetDistance(TEXT("Animals.HideDistance"), 400.0f);
	};
	Phases.Add(MoveTemp(Detail));

	// 3. The realistic models close up in daylight, paused so they hold still for the picture.
	FPhase Near;
	Near.Name = TEXT("animals_close");
	Near.WarmupSeconds = 3.0f;
	Near.MeasureSeconds = 2.0f;
	Near.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		const colony::CellPos M = Middle();
		int32 I = 0;
		for (const TCHAR* Def : AnimalSmokeCloseDefs)
		{
			// The 12 m T-rex stands behind the middle of the row, where it fits in the picture without hiding the rest.
			const bool bBig = FCString::Strcmp(Def, TEXT("TRex")) == 0;
			const colony::CellPos At = bBig ? colony::CellPos{M.x - 28, M.y + 10, 0} : colony::CellPos{M.x - 14, M.y - 4 + I++ * 4, 0};
			B->Submit(colony::SpawnThingCommand{std::string(TCHAR_TO_UTF8(Def)), At});
		}
		B->StepOnce();
		B->SetSpeed(0);
		// From the side the animals face (+x), wide enough for the whole row of eight and the T-rex behind it.
		const FVector Target = B->SimToWorld(M.x - 16.0f, M.y + 10.0f, 0) + FVector(0.0, 0.0, 250.0);
		const FVector From = Target + FVector(2000.0, -1100.0, 500.0);
		ViewFrom(From, (Target - From).Rotation());
		State->Reset();
	};
	Near.OnFrame = CheckFrame;
	Near.Verify = [=, this]()
	{
		VerifyFrames(TEXT("animals_close"));
		AnimalSmokeSetDistance(TEXT("Sky.Hour"), -1.0f);
		AnimalSmokeSetDistance(TEXT("Sky.DayOfYear"), -1.0f);
	};
	Phases.Add(MoveTemp(Near));

	// 4. The batch that replaced the husky stand-in, close up in daylight and paused: every one must have its own model.
	FPhase Batch;
	Batch.Name = TEXT("animals_new");
	Batch.WarmupSeconds = 3.0f;
	Batch.MeasureSeconds = 2.0f;
	Batch.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		AnimalSmokeSetDistance(TEXT("Sky.Hour"), 12.0f);
		AnimalSmokeSetDistance(TEXT("Sky.DayOfYear"), 22.5f);
		const colony::CellPos M = Middle();
		int32 I = 0;
		for (const TCHAR* Def : AnimalSmokeNewBigDefs)
		{
			B->Submit(colony::SpawnThingCommand{std::string(TCHAR_TO_UTF8(Def)), colony::CellPos{M.x + 26, M.y - 20 + I++ * 4, 0}});
		}
		I = 0;
		for (const TCHAR* Def : AnimalSmokeNewSmallDefs)
		{
			B->Submit(colony::SpawnThingCommand{std::string(TCHAR_TO_UTF8(Def)), colony::CellPos{M.x + 31, M.y - 18 + I++ * 4, 0}});
		}
		B->StepOnce();
		B->SetSpeed(0);
		const FVector Target = B->SimToWorld(M.x + 28.0f, M.y, 0) + FVector(0.0, 0.0, 80.0);
		const FVector From = Target + FVector(2200.0, -500.0, 600.0);
		ViewFrom(From, (Target - From).Rotation());
		State->Reset();
	};
	Batch.OnFrame = CheckFrame;
	Batch.Verify = [=, this]()
	{
		VerifyFrames(TEXT("animals_new"));
		USimAnimalSubsystem* Animals = AnimalsOf();
		USimSubsystem* B = Bridge();
		if (Animals && B && B->GetSim())
		{
			// A def with no entry in assets/animal_art.json is drawn as the husky and marked a stand-in; these all have one.
			const auto CheckDef = [&](const TCHAR* Def)
			{
				FSimAnimalArt Art;
				Animals->GetModelFor(FName(Def), Art);
				if (!State->DefsDrawn.Contains(FName(Def)))
				{
					Fail(FString::Printf(TEXT("animals_new: %s was not drawn"), Def));
				}
				else if (Art.Model == FName(TEXT("Husky")))
				{
					Fail(FString::Printf(TEXT("animals_new: %s is still drawn as the husky"), Def));
				}
			};
			for (const TCHAR* Def : AnimalSmokeNewBigDefs) { CheckDef(Def); }
			for (const TCHAR* Def : AnimalSmokeNewSmallDefs) { CheckDef(Def); }
			FString Lengths;
			for (const TPair<uint32, TObjectPtr<ASimAnimalActor>>& Pair : Animals->GetAnimals())
			{
				const colony::ThingView* View = B->GetCurrentSnapshot().find(Pair.Value->GetThing());
				if (View && View->def.valid())
				{
					Lengths += FString::Printf(TEXT(" %s %.0f cm,"), UTF8_TO_TCHAR(B->GetDefs()->get(View->def).defName().c_str()), Pair.Value->GetDrawLengthCm());
				}
			}
			Notes.Add(FString::Printf(TEXT("animals_new: %d models loaded; lengths:%s"), Animals->GetModelsLoaded(), *Lengths));
		}
	};
	Phases.Add(MoveTemp(Batch));

	// 4b. The remnants, paused in a row: each must be drawn as its own machine by the animal subsystem.
	FPhase Remnants;
	Remnants.Name = TEXT("remnants");
	Remnants.WarmupSeconds = 3.0f;
	Remnants.MeasureSeconds = 1.0f;
	Remnants.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		const colony::CellPos M = Middle();
		int32 I = 0;
		for (const TCHAR* Def : AnimalSmokeRemnantDefs)
		{
			B->Submit(colony::SpawnThingCommand{std::string(TCHAR_TO_UTF8(Def)), colony::CellPos{M.x + 38, M.y - 9 + I++ * 6, 0}});
		}
		B->StepOnce();
		B->SetSpeed(0);
		const FVector Target = B->SimToWorld(M.x + 38.0f, M.y, 0) + FVector(0.0, 0.0, 100.0);
		const FVector From = Target + FVector(1900.0, -500.0, 500.0);
		ViewFrom(From, (Target - From).Rotation());
		State->Reset();
	};
	Remnants.OnFrame = CheckFrame;
	Remnants.Verify = [=, this]()
	{
		VerifyFrames(TEXT("remnants"));
		USimAnimalSubsystem* Animals = AnimalsOf();
		USimSubsystem* B = Bridge();
		if (!Animals || !B || !B->GetSim())
		{
			return;
		}
		FString Seen;
		for (const TCHAR* Def : AnimalSmokeRemnantDefs)
		{
			if (!Animals->DrawsMachine(FName(Def)))
			{
				Fail(FString::Printf(TEXT("remnants: %s has no machine model (assets/animal_art.json, AnimalArt)"), Def));
			}
		}
		for (const TPair<uint32, TObjectPtr<ASimAnimalActor>>& Pair : Animals->GetAnimals())
		{
			const colony::ThingView* View = B->GetCurrentSnapshot().find(Pair.Value->GetThing());
			if (View && View->def.valid() && View->lifeStage < 0)
			{
				Seen += FString::Printf(TEXT(" %s %.0f cm,"), UTF8_TO_TCHAR(B->GetDefs()->get(View->def).defName().c_str()), Pair.Value->GetDrawLengthCm());
			}
		}
		Notes.Add(FString::Printf(TEXT("remnants: drawn as machines:%s"), *Seen));
	};
	Phases.Add(MoveTemp(Remnants));

	// 5. One picture of each animal of the batch, close up from its side (phases animal_<def>), so each model's look,
	// size and facing can be checked on its own.
	const auto AddCloseUp = [=, this](const TCHAR* Def, float CellX, float CellY, float DistanceCm)
	{
		FPhase One;
		One.Name = FString::Printf(TEXT("animal_%s"), Def);
		One.WarmupSeconds = 0.5f;
		One.MeasureSeconds = 0.5f;
		One.Setup = [=, this]()
		{
			USimSubsystem* B = Bridge();
			if (!B || !B->GetSim())
			{
				return;
			}
			const colony::CellPos M = Middle();
			const FVector Target = B->SimToWorld(M.x + CellX, M.y + CellY, 0) + FVector(0.0, 0.0, DistanceCm * 0.12);
			const FVector From = Target + FVector(DistanceCm * 0.85, -DistanceCm * 0.45, DistanceCm * 0.3);
			ViewFrom(From, (Target - From).Rotation());
		};
		Phases.Add(MoveTemp(One));
	};
	for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(AnimalSmokeNewBigDefs)); ++Index)
	{
		AddCloseUp(AnimalSmokeNewBigDefs[Index], 26.0f, -20.0f + Index * 4.0f, 900.0f);
	}
	for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(AnimalSmokeNewSmallDefs)); ++Index)
	{
		AddCloseUp(AnimalSmokeNewSmallDefs[Index], 31.0f, -18.0f + Index * 4.0f, 450.0f);
	}
	for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(AnimalSmokeRemnantDefs)); ++Index)
	{
		AddCloseUp(AnimalSmokeRemnantDefs[Index], 38.0f, -9.0f + Index * 6.0f, Index == 3 ? 1100.0f : 700.0f);
	}
}
