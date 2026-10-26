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

#include <string>

namespace
{
	const FVector AnimalSmokeOrigin(-900000.0, 0.0, 0.0);
	// Every def A4 has art for, plus two with none (they fall back on a stand-in model).
	const TCHAR* const AnimalSmokeDefs[] = {TEXT("Cow"), TEXT("Muffalo"), TEXT("Deer"), TEXT("Husky"), TEXT("TimberWolf"), TEXT("Thrumbo"),
		TEXT("Alpaca"), TEXT("Cat"), TEXT("WildBoar"), TEXT("Cougar"), TEXT("GrizzlyBear"), TEXT("Chicken")};
	constexpr float AnimalSmokeSpacing = 2.0f;

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
			// Drawn about as big as an animal: the whole body between 15 cm and 4.5 m long.
			const double Size = Body->Bounds.BoxExtent.GetMax() * 2.0;
			if (Size < 15.0 || Size > 450.0)
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
}
