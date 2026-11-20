// Smoke test `look` (task L1, the look bench): one fixed seed and the same shots every run, so each look task can show
// sneed a before and after sheet (tools/look_sheet.ps1 copies the shots into Saved/Look/<date>/ and lays them beside the
// last run's). The shots: the overseer high and low over the colony, a colonist close up, inside a lit room, dusk, a
// rainy night, the HUD as the player first sees it, a sample wall of every building stuff (L4), and a snowy day.
#include "SmokeTest.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"

#include "FireSubsystem.h"
#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"
#include "SkySubsystem.h"
#include "WindowLightSubsystem.h"
#include "SnowSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Windows.h"

#include <optional>

namespace
{
	const FVector LookSmokeOrigin(-1300000.0, 0.0, 0.0);
	// The bench's map. Since the tropics focus (2026-11-13) the look tasks judge the rainforest; Crashlanded is the old
	// temperate bench (pass -ini:Engine:[SystemSettings]:Look.Scenario=Crashlanded to compare with old sheets).
	TAutoConsoleVariable<FString> GLookSmokeScenario(
		TEXT("Look.Scenario"), TEXT("CrashlandedRainforest"), TEXT("The scenario the look bench (smoke test look) starts on."), ECVF_Default);
	constexpr float LookSmokeMidsummer = 22.5f;
	constexpr float LookSmokeMidwinter = 52.5f;
	// The room: 7 x 6 cells with its walls, a door in the south wall.
	constexpr int32 LookRoomW = 7;
	constexpr int32 LookRoomH = 6;
	// The sample wall (L4): two cells of each building stuff in a north-south line four cells west of the room, turning west at its
	// south end (its west side faces the noon sun).
	constexpr int32 LookSampleX = -4;
	constexpr int32 LookSampleY = -4;
	const char* const LookSampleStuffs[] = {"BlocksGranite", "BlocksLimestone", "BlocksSandstone", "BlocksSlate", "BlocksMarble", "Steel", "Plasteel"};

	struct FLookSmokeState
	{
		int64 StepsWanted = 0;
		int64 StepsRun = 0;
		colony::CellPos Home = colony::CellPos{128, 128, 0};
		colony::CellPos Room = colony::CellPos{128, 128, 0};
		bool bRoomBuilt = false;
		uint32 CloseUpPawn = 0;
		bool bCloseUpFramed = false;
		/** The exposure (EV100) each shot was taken at, for the notes. */
		float NoonExposure = 0.0f;
	};

	void LookSmokeSet(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByCode);
		}
	}

	bool LookSmokeFree(const colony::Map& Map, int32 X, int32 Y)
	{
		const colony::CellPos Cell{X, Y, 0};
		return Map.inBounds(Cell) && (Map.terrain(Cell) == colony::Terrain::Soil || Map.terrain(Cell) == colony::Terrain::Sand) && Map.roof(Cell) == colony::Roof::None &&
			Map.buildingAt(Cell) == colony::kNoEntity && Map.groundWalkable(Cell);
	}

	/** The south-west corner of a level rectangle of open soil, W x H with a free cell round it, nearest `Near` (but 6 cells or more away). */
	bool LookSmokeFindRect(const colony::Map& Map, colony::CellPos Near, int32 W, int32 H, colony::CellPos& Out)
	{
		for (int32 R = 6; R <= 90; ++R)
		{
			for (int32 DY = -R; DY <= R; ++DY)
			{
				for (int32 DX = -R; DX <= R; ++DX)
				{
					if (FMath::Max(FMath::Abs(DX), FMath::Abs(DY)) != R)
					{
						continue;
					}
					const int32 X0 = Near.x + DX;
					const int32 Y0 = Near.y + DY;
					bool bOk = true;
					float Low = 1.0e9f;
					float High = -1.0e9f;
					for (int32 Y = Y0 - 1; Y <= Y0 + H && bOk; ++Y)
					{
						for (int32 X = X0 - 1; X <= X0 + W && bOk; ++X)
						{
							bOk = LookSmokeFree(Map, X, Y);
							if (bOk)
							{
								Low = FMath::Min(Low, Map.groundHeight(X, Y));
								High = FMath::Max(High, Map.groundHeight(X, Y));
							}
						}
					}
					if (bOk && High - Low < 0.6f)
					{
						Out = colony::CellPos{X0, Y0, 0};
						return true;
					}
				}
			}
		}
		return false;
	}
}

void USmokeTestSubsystem::BuildLookPhases()
{
	const TSharedRef<FLookSmokeState> State = MakeShared<FLookSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Sky = [this]() { return GetWorld()->GetSubsystem<USkySubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };
	const auto Pawns = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };
	const auto SetClock = [](float Hour, float DayOfYear)
	{
		LookSmokeSet(TEXT("Sky.Hour"), Hour);
		LookSmokeSet(TEXT("Sky.DayOfYear"), DayOfYear);
	};
	const auto SetWeather = [=, this](const char* Name, float Hours)
	{
		Bridge()->Submit(colony::SetWeatherCommand{Name, Hours});
		Bridge()->StepOnce();
	};
	// The game stays paused for every shot (so the pawns stand where they stood last run); the sim is stepped by hand, a
	// little each frame, when a shot needs time to pass (rain soaking in, snow lying).
	const auto Advance = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim() || State->StepsRun >= State->StepsWanted)
		{
			return;
		}
		const double Start = FPlatformTime::Seconds();
		int32 Count = 0;
		while (State->StepsRun < State->StepsWanted && (FPlatformTime::Seconds() - Start) < 0.03 && Count < 400)
		{
			B->StepOnce();
			++State->StepsRun;
			++Count;
		}
	};
	const auto RunFor = [=](int64 Seconds)
	{
		State->StepsWanted = Seconds;
		State->StepsRun = 0;
	};
	const auto CellWorld = [=, this](float X, float Y, double Up = 0.0)
	{
		return Bridge()->SimToWorld(X, Y, 0) + FVector(0.0, 0.0, Up);
	};
	// The overseer camera: zoom notches from its default 45 m, tilt and turn from its default 55 degrees looking north.
	const auto Aim = [=, this](colony::CellPos At, float Notches, float Tilt, float Turn)
	{
		AOverseerPawn* Pawn = Overseer() ? Overseer()->GetOverseerPawn() : nullptr;
		if (!Pawn)
		{
			Fail(TEXT("look: no overseer camera"));
			return;
		}
		Pawn->ResetView();
		Pawn->Zoom(Notches);
		Pawn->Rotate(Turn, Tilt);
		Pawn->FocusOn(Bridge()->SimToWorld(At.x + 0.5f, At.y + 0.5f, 0));
		Pawn->SnapCamera();
		if (APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0))
		{
			PC->SetViewTarget(Pawn);
		}
	};
	const auto RoomCentre = [=]() { return colony::CellPos{State->Room.x + LookRoomW / 2, State->Room.y + LookRoomH / 2, 0}; };
	const auto Exposure = [=, this]() { return Sky() ? Sky()->GetExposure() : 0.0f; };
	const auto NoteShot = [=, this](const TCHAR* Shot)
	{
		USkySubsystem* S = Sky();
		if (!S)
		{
			return;
		}
		const FSkyState& K = S->GetState();
		Notes.Add(FString::Printf(TEXT("%s: %02d:%02d day %.1f, sun %.1f deg %.0f lux, moon %.2f lux, sky light %.2f, fog %.4f, exposure EV %.2f"), Shot,
			FMath::FloorToInt32(K.Hour), FMath::FloorToInt32(FMath::Frac(K.Hour) * 60.0f), K.DayOfYear, K.SunElevation, K.SunLux, K.MoonLux, K.SkyLightIntensity,
			K.FogDensity, Exposure()));
	};

	// The sim, the colony's home, a room with lamps, then the clock run on to 10:00 so the colonists are up.
	FPhase Prepare;
	Prepare.Name = TEXT("_prepare");
	Prepare.WarmupSeconds = 10.0f;
	Prepare.MeasureSeconds = 0.5f;
	Prepare.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Sky() || !Overseer() || !Pawns())
		{
			Fail(TEXT("USimSubsystem, USkySubsystem, UOverseerSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		LookSmokeSet(TEXT("Terrain.Roofs"), -1.0f);
		// Full HD even offscreen (the window starts small), so the sheets show detail.
		GEngine->Exec(GetWorld(), TEXT("r.SetRes 1920x1080w"));
		// The pawns as the game means them to look: MetaHumans with their faces.
		LookSmokeSet(TEXT("Pawns.MetaHumans"), 1.0f);
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 256;
		Options.Scenario = GLookSmokeScenario.GetValueOnGameThread();
		// A biome scenario's biome comes from its world tile, so those start on the world map (as the playtest does).
		Options.bWorld = Options.Scenario != TEXT("Crashlanded");
		B->SetWorldOrigin(LookSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		if (!Overseer()->EnterOverseerMode())
		{
			Fail(TEXT("The overseer camera did not start"));
			return;
		}
		if (const std::optional<colony::CellPos> Stockpile = B->GetSim()->stockpileCell())
		{
			State->Home = *Stockpile;
		}
		SetWeather("Clear", 96.0f);
		const colony::Map& Map = B->GetSim()->map();
		if (!LookSmokeFindRect(Map, State->Home, LookRoomW, LookRoomH, State->Room))
		{
			Fail(TEXT("look: no open ground for the room near home"));
			return;
		}
		const auto At = [=](int32 DX, int32 DY) { return colony::CellPos{State->Room.x + DX, State->Room.y + DY, 0}; };
		// A colony's clearing: no trees, bamboo or bushes within 22 cells of the room or 8 of home (grass and the ground cover stay),
		// so the close-up, walls and night shots see the colony and not a wall of jungle.
		{
			colony::Sim* Clear = SimOf();
			const colony::CellPos Mid{State->Room.x + LookRoomW / 2, State->Room.y + LookRoomH / 2, 0};
			const auto Near = [](int32 X, int32 Y, colony::CellPos P, int32 R) { return (X - P.x) * (X - P.x) + (Y - P.y) * (Y - P.y) <= R * R; };
			for (int32 Y = Mid.y - 24; Clear && Y <= Mid.y + 24; ++Y)
			{
				for (int32 X = Mid.x - 24; X <= Mid.x + 24; ++X)
				{
					const colony::CellPos C{X, Y, 0};
					if (!Map.inBounds(C) || !Clear->plants().has(C) || !(Near(X, Y, Mid, 22) || Near(X, Y, State->Home, 8)))
					{
						continue;
					}
					const std::string Name = Clear->defs().get(Clear->plants().defAt(C)).defName();
					if (Name.find("Tree") != std::string::npos || Name == "Plant_Bamboo" || Name == "Plant_Bush")
					{
						Clear->plants().remove(C);
					}
				}
			}
		}
		// No trees or bushes in the room or on the sample wall's line.
		for (int32 Y = LookSampleY - 1; Y <= LookSampleY + 2 * static_cast<int32>(UE_ARRAY_COUNT(LookSampleStuffs)); ++Y)
		{
			for (int32 X = LookSampleX - 2; X <= LookRoomW; ++X)
			{
				if (SimOf()->plants().has(At(X, Y)))
				{
					SimOf()->plants().remove(At(X, Y));
				}
			}
		}
		for (int32 Y = 0; Y < LookRoomH; ++Y)
		{
			for (int32 X = 0; X < LookRoomW; ++X)
			{
				const bool bEdge = X == 0 || Y == 0 || X == LookRoomW - 1 || Y == LookRoomH - 1;
				if (bEdge && !(Y == 0 && X == LookRoomW / 2))
				{
					B->Submit(colony::SpawnThingCommand{"Wall", At(X, Y), 1});
				}
			}
		}
		B->Submit(colony::SpawnThingCommand{"Door", At(LookRoomW / 2, 0), 1});
		for (int32 Y = 0; Y < 2 * static_cast<int32>(UE_ARRAY_COUNT(LookSampleStuffs)); ++Y)
		{
			B->Submit(colony::SpawnThingCommand{"Wall", At(LookSampleX, LookSampleY + Y), 1});
		}
		B->Submit(colony::SpawnThingCommand{"Wall", At(LookSampleX - 1, LookSampleY), 1});
		B->Submit(colony::SpawnThingCommand{"Bed", At(1, 3), 1, colony::Rot4::East});
		B->Submit(colony::SpawnThingCommand{"EndTable", At(1, 1), 1});
		B->Submit(colony::SpawnThingCommand{"Table1x2", At(4, 2), 1});
		B->Submit(colony::SpawnThingCommand{"Armchair", At(5, 3), 1, colony::Rot4::West});
		B->Submit(colony::SpawnThingCommand{"TorchLamp", At(5, 1), 1});
		B->Submit(colony::SpawnThingCommand{"TorchLamp", At(1, 4), 1});
		// The sim's rooms put a thin roof over the closed room by themselves.
		B->StepOnce();
		colony::Sim* Sim = SimOf();
		// Spawned buildings are made of nothing; give the room wood (the first house most colonies build) and the sample its stuffs.
		const auto MakeOf = [Sim](colony::CellPos Cell, const char* Stuff)
		{
			const colony::Entity Wall = Sim->map().buildingAt(Cell);
			if (Wall != colony::kNoEntity)
			{
				Sim->things().registry().emplace_or_replace<colony::BuiltOf>(Wall, colony::BuiltOf{Sim->defs().idOf("Things", Stuff)});
			}
			return Wall != colony::kNoEntity;
		};
		for (int32 Y = 0; Y < LookRoomH; ++Y)
		{
			for (int32 X = 0; X < LookRoomW; ++X)
			{
				if (X == 0 || Y == 0 || X == LookRoomW - 1 || Y == LookRoomH - 1)
				{
					MakeOf(At(X, Y), "WoodLog");
				}
			}
		}
		int32 SampleWalls = 0;
		for (int32 Y = 0; Y < 2 * static_cast<int32>(UE_ARRAY_COUNT(LookSampleStuffs)); ++Y)
		{
			SampleWalls += MakeOf(At(LookSampleX, LookSampleY + Y), LookSampleStuffs[Y / 2]) ? 1 : 0;
		}
		SampleWalls += MakeOf(At(LookSampleX - 1, LookSampleY), LookSampleStuffs[0]) ? 1 : 0;
		// Windows (docs/design/windows.md): two in the room's north wall, and one in the second cell of each stuff in the
		// sample wall, the granite one with its glass broken.
		int32 WindowsCut = 0;
		for (const int32 X : {2, 4})
		{
			WindowsCut += Sim->windows().add(At(X, LookRoomH - 1)) ? 1 : 0;
		}
		for (int32 Y = 1; Y < 2 * static_cast<int32>(UE_ARRAY_COUNT(LookSampleStuffs)); Y += 2)
		{
			WindowsCut += Sim->windows().add(At(LookSampleX, LookSampleY + Y)) ? 1 : 0;
		}
		colony::Windows::breakGlass(Sim->things(), Sim->map().buildingAt(At(LookSampleX, LookSampleY + 1)));
		int32 Torches = 0;
		for (const colony::CellPos Cell : {At(5, 1), At(1, 4)})
		{
			const colony::Entity Torch = Sim->map().buildingAt(Cell);
			if (Torch != colony::kNoEntity && Sim->things().registry().all_of<colony::Fuel>(Torch))
			{
				Sim->jobs().fuel().add(Torch, static_cast<int>(Sim->jobs().fuel().specOf(Torch).capacity));
				++Torches;
			}
		}
		State->bRoomBuilt = Sim->map().buildingAt(At(0, 0)) != colony::kNoEntity && Torches == 2;
		Notes.Add(FString::Printf(TEXT("_prepare: home (%d, %d), room at (%d, %d), %d torches fuelled, %d sample walls, %d windows"), State->Home.x, State->Home.y, State->Room.x,
			State->Room.y, Torches, SampleWalls, WindowsCut));
		{
			// Which plants the bench's map grows (the look tasks dress the common ones first).
			TMap<FString, int32> Kinds;
			const colony::Map& M = Sim->map();
			for (int32 Y = 0; Y < M.height(); ++Y)
			{
				for (int32 X = 0; X < M.width(); ++X)
				{
					const colony::CellPos C{X, Y, 0};
					if (Sim->plants().has(C))
					{
						++Kinds.FindOrAdd(UTF8_TO_TCHAR(Sim->defs().get(Sim->plants().defAt(C)).defName().c_str()));
					}
				}
			}
			Kinds.ValueSort([](int32 A, int32 B) { return A > B; });
			FString List;
			for (const TPair<FString, int32>& Kind : Kinds)
			{
				List += FString::Printf(TEXT("%s %d, "), *Kind.Key, Kind.Value);
			}
			Notes.Add(TEXT("_prepare: plants: ") + List);
		}
		RunFor(10 * colony::kSecondsPerHour);
		SetClock(12.0f, LookSmokeMidsummer);
		Aim(State->Home, -8.0f, 0.0f, 0.0f);
	};
	Prepare.OnFrame = Advance;
	Prepare.Verify = [=, this]()
	{
		if (!State->bRoomBuilt)
		{
			Fail(TEXT("look: the room's walls or its two lit torches are missing"));
		}
		if (State->StepsRun < State->StepsWanted)
		{
			Fail(FString::Printf(TEXT("look: the clock ran %lld of %lld seconds in the preparation"), State->StepsRun, State->StepsWanted));
		}
	};
	Phases.Add(MoveTemp(Prepare));

	// 1. The overseer high over the colony at a clear midsummer noon.
	FPhase High;
	High.Name = TEXT("overseer_high");
	High.WarmupSeconds = 5.0f;
	High.MeasureSeconds = 2.0f;
	High.Setup = [=]()
	{
		SetClock(12.0f, LookSmokeMidsummer);
		Aim(State->Home, -6.0f, 0.0f, 0.0f);
	};
	High.Verify = [=, this]()
	{
		NoteShot(TEXT("overseer_high"));
		State->NoonExposure = Exposure();
		USkySubsystem* S = Sky();
		if (S && (!S->IsLookFileLoaded() || S->GetQualityLevel() != 2 || !S->GetPostProcess()))
		{
			Fail(FString::Printf(TEXT("overseer_high: data/look.json read %d, Look.Quality %d (wanted High), post process %d"), S->IsLookFileLoaded() ? 1 : 0, S->GetQualityLevel(),
				S->GetPostProcess() ? 1 : 0));
		}
		// A clear noon in real-world light: a sun near the look file's lux and an exposure near its noon EV.
		if (S && (S->GetState().SunLux < 0.5f * S->GetLook().SunClearNoonLux || FMath::Abs(State->NoonExposure - S->GetLook().NoonEV) > 1.0f))
		{
			Fail(FString::Printf(TEXT("overseer_high: noon sun %.0f lux, exposure EV %.2f"), S->GetState().SunLux, State->NoonExposure));
		}
	};
	Phases.Add(MoveTemp(High));

	// 2. The overseer low, looking across the field toward the room in the late morning.
	FPhase Low;
	Low.Name = TEXT("overseer_low");
	Low.WarmupSeconds = 4.0f;
	Low.MeasureSeconds = 2.0f;
	Low.Setup = [=]()
	{
		SetClock(10.5f, LookSmokeMidsummer);
		Aim(RoomCentre(), 2.0f, -30.0f, 35.0f);
	};
	Low.Verify = [=]() { NoteShot(TEXT("overseer_low")); };
	Phases.Add(MoveTemp(Low));

	// 3. A colonist close up: head and shoulders from in front, at ten in the morning.
	FPhase Close;
	Close.Name = TEXT("colonist_closeup");
	Close.WarmupSeconds = 4.0f;
	Close.MeasureSeconds = 2.0f;
	Close.Setup = [=, this]()
	{
		SetClock(10.0f, LookSmokeMidsummer);
		State->bCloseUpFramed = false;
		State->CloseUpPawn = 0;
		if (USimPawnSubsystem* P = Pawns())
		{
			for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Pair : P->GetPawns())
			{
				ASimPawnActor* Actor = Pair.Value;
				if (Actor && Actor->GetBody() && (State->CloseUpPawn == 0 || Pair.Key < State->CloseUpPawn))
				{
					State->CloseUpPawn = Pair.Key;
				}
			}
		}
	};
	Close.OnFrame = [=, this]()
	{
		USimPawnSubsystem* P = Pawns();
		const TObjectPtr<ASimPawnActor>* Found = P ? P->GetPawns().Find(State->CloseUpPawn) : nullptr;
		ASimPawnActor* Actor = Found ? Found->Get() : nullptr;
		// Far pawns have no face mesh; the camera comes close first and the face follows.
		USkeletalMeshComponent* Mesh = Actor && Actor->GetFaceMesh() && Actor->GetFaceMesh()->DoesSocketExist(TEXT("head")) ? Actor->GetFaceMesh() : (Actor ? Actor->GetBody() : nullptr);
		if (!Mesh || !Mesh->DoesSocketExist(TEXT("head")))
		{
			return;
		}
		const FVector Head = Mesh->GetSocketLocation(TEXT("head")) + FVector(0.0, 0.0, -8.0);
		const FVector From = Head + (Actor->GetActorForwardVector() * 1.0 + Actor->GetActorRightVector() * 0.35).GetSafeNormal() * 135.0 + FVector(0.0, 0.0, 6.0);
		ViewFrom(From, (Head - From).Rotation());
		State->bCloseUpFramed = true;
	};
	Close.Verify = [=, this]()
	{
		NoteShot(TEXT("colonist_closeup"));
		if (!State->bCloseUpFramed)
		{
			Fail(TEXT("colonist_closeup: no colonist with a face to look at"));
		}
	};
	Phases.Add(MoveTemp(Close));

	// 4. Inside the room at nine in the evening, roof on, lit by its two torches.
	FPhase Room;
	Room.Name = TEXT("room_lit");
	Room.WarmupSeconds = 5.0f;
	Room.MeasureSeconds = 2.0f;
	Room.Setup = [=, this]()
	{
		SetClock(21.0f, LookSmokeMidsummer);
		LookSmokeSet(TEXT("Terrain.Roofs"), 1.0f);
		const FVector From = CellWorld(State->Room.x + LookRoomW - 1.25f, State->Room.y + 1.3f, 175.0);
		const FVector To = CellWorld(State->Room.x + 1.5f, State->Room.y + LookRoomH - 1.6f, 55.0);
		ViewFrom(From, (To - From).Rotation());
	};
	Room.Verify = [=, this]()
	{
		NoteShot(TEXT("room_lit"));
		LookSmokeSet(TEXT("Terrain.Roofs"), -1.0f);
		if (UFireSubsystem* F = GetWorld()->GetSubsystem<UFireSubsystem>(); F && F->GetLampSources() < 2)
		{
			Fail(FString::Printf(TEXT("room_lit: %d lamps lit, wanted the room's two torches"), F->GetLampSources()));
		}
	};
	Phases.Add(MoveTemp(Room));

	// 5. Dusk: the low sun in the west, seen from a little above the field east of the room.
	FPhase Dusk;
	Dusk.Name = TEXT("dusk");
	Dusk.WarmupSeconds = 5.0f;
	Dusk.MeasureSeconds = 2.0f;
	Dusk.Setup = [=, this]()
	{
		SetClock(17.8f, LookSmokeMidsummer);
		const colony::CellPos C = RoomCentre();
		const FVector To = CellWorld(C.x + 0.5f, C.y - 6.0f, 150.0);
		const FVector From = CellWorld(C.x - 3.0f, C.y + 22.0f, 900.0);
		ViewFrom(From, (To - From).Rotation());
	};
	Dusk.Verify = [=, this]()
	{
		NoteShot(TEXT("dusk"));
		// Dusk is held darker than noon, but not as dark as night.
		if (Exposure() > State->NoonExposure - 1.0f || Exposure() < State->NoonExposure - 8.0f)
		{
			Fail(FString::Printf(TEXT("dusk: exposure EV %.2f against noon's %.2f"), Exposure(), State->NoonExposure));
		}
	};
	Phases.Add(MoveTemp(Dusk));

	// 6. A rainy night: an hour of rain, the overseer over the room with its lamps lit.
	FPhase Rain;
	Rain.Name = TEXT("rainy_night");
	Rain.WarmupSeconds = 8.0f;
	Rain.MeasureSeconds = 2.0f;
	Rain.Setup = [=]()
	{
		SetWeather("Rain", 24.0f);
		RunFor(colony::kSecondsPerHour);
		SetClock(23.0f, LookSmokeMidsummer);
		Aim(RoomCentre(), 0.0f, -10.0f, 0.0f);
	};
	Rain.OnFrame = Advance;
	Rain.Verify = [=, this]()
	{
		NoteShot(TEXT("rainy_night"));
		if (Exposure() > State->NoonExposure - 8.0f)
		{
			Fail(FString::Printf(TEXT("rainy_night: exposure EV %.2f against noon's %.2f; the night is not held dark"), Exposure(), State->NoonExposure));
		}
	};
	Phases.Add(MoveTemp(Rain));

	// 7. The HUD as a new player sees it (the screenshot keeps the UI): the overseer view at noon.
	FPhase Hud;
	Hud.Name = TEXT("hud");
	Hud.WarmupSeconds = 4.0f;
	Hud.MeasureSeconds = 2.0f;
	Hud.Setup = [=]()
	{
		SetWeather("Clear", 24.0f);
		RunFor(2 * colony::kSecondsPerHour);
		SetClock(12.0f, LookSmokeMidsummer);
		Aim(State->Home, 0.0f, 0.0f, 0.0f);
	};
	Hud.OnFrame = Advance;
	Hud.Verify = [=, this]()
	{
		NoteShot(TEXT("hud"));
		const IConsoleVariable* Timings = IConsoleManager::Get().FindConsoleVariable(TEXT("Sim.ShowTimings"));
		if ((Timings && Timings->GetInt() != 0) || USimSubsystem::IsKeyHelpShown())
		{
			Fail(TEXT("hud: the sim timings or the key list are shown by default"));
		}
	};
	Phases.Add(MoveTemp(Hud));

	// 7b. Dawn mist (L2/L3 weather): ground mist at a quarter past six, looking east across the field into the low sun.
	FPhase Mist;
	Mist.Name = TEXT("dawn_mist");
	Mist.WarmupSeconds = 5.0f;
	Mist.MeasureSeconds = 2.0f;
	Mist.Setup = [=, this]()
	{
		SetClock(6.25f, LookSmokeMidsummer);
		const colony::CellPos C = RoomCentre();
		const FVector From = CellWorld(C.x - 26.0f, C.y + 4.0f, 650.0);
		const FVector To = CellWorld(C.x + 10.0f, C.y - 2.0f, 120.0);
		ViewFrom(From, (To - From).Rotation());
	};
	Mist.Verify = [=, this]()
	{
		NoteShot(TEXT("dawn_mist"));
		if (USkySubsystem* S = Sky(); S && S->GetFog())
		{
			const FVector Eye = GetWorld()->GetFirstPlayerController() && GetWorld()->GetFirstPlayerController()->PlayerCameraManager ? GetWorld()->GetFirstPlayerController()->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
			Notes.Add(FString::Printf(TEXT("dawn_mist: mist %.4f, its base %.0f cm (fog at %.0f), camera at %.0f cm"), S->GetState().MistDensity, S->GetFog()->SecondFogData.FogHeightOffset + S->GetFog()->GetComponentLocation().Z, S->GetFog()->GetComponentLocation().Z, Eye.Z));
		}
		if (USkySubsystem* S = Sky(); S && S->GetLook().MistDensity > 0.0f && S->GetState().MistDensity <= 0.0f)
		{
			Fail(TEXT("dawn_mist: no ground mist at dawn"));
		}
	};
	Phases.Add(MoveTemp(Mist));

	// 7c. A clear night: the overseer low over the room at eleven, tilted up so the star dome and the moonlit ground both show.
	FPhase Night;
	Night.Name = TEXT("clear_night");
	Night.WarmupSeconds = 6.0f;
	Night.MeasureSeconds = 2.0f;
	Night.Setup = [=, this]()
	{
		SetClock(23.0f, LookSmokeMidsummer);
		const colony::CellPos C = RoomCentre();
		const FVector From = CellWorld(C.x - 3.0f, C.y + 18.0f, 260.0);
		const FVector To = CellWorld(C.x + 2.0f, C.y - 30.0f, 1500.0);
		ViewFrom(From, (To - From).Rotation());
	};
	Night.Verify = [=, this]()
	{
		NoteShot(TEXT("clear_night"));
		if (USkySubsystem* S = Sky())
		{
			Notes.Add(FString::Printf(TEXT("clear_night: stars %.3f, mist %.4f"), S->GetState().StarBrightness, S->GetState().MistDensity));
		}
	};
	Phases.Add(MoveTemp(Night));

	// 8. The walls (L4): the sample wall of every building stuff, from the west at noon.
	FPhase Walls;
	Walls.Name = TEXT("walls");
	Walls.WarmupSeconds = 4.0f;
	Walls.MeasureSeconds = 2.0f;
	Walls.Setup = [=, this]()
	{
		SetClock(12.0f, LookSmokeMidsummer);
		const float MiddleY = static_cast<float>(State->Room.y + LookSampleY) + static_cast<float>(UE_ARRAY_COUNT(LookSampleStuffs));
		const FVector To = CellWorld(static_cast<float>(State->Room.x + LookSampleX) + 0.5f, MiddleY, 120.0);
		const FVector From = CellWorld(static_cast<float>(State->Room.x + LookSampleX) - 9.5f, MiddleY - 2.0f, 420.0);
		ViewFrom(From, (To - From).Rotation());
	};
	Walls.Verify = [=]() { NoteShot(TEXT("walls")); };
	Phases.Add(MoveTemp(Walls));

	// 8b. Windows (docs/design/windows.md): inside the room at noon, looking at the two windows of its north wall, with the
	// sun coming in through them.
	FPhase WindowsShot;
	WindowsShot.Name = TEXT("windows");
	WindowsShot.WarmupSeconds = 4.0f;
	WindowsShot.MeasureSeconds = 2.0f;
	WindowsShot.Setup = [=, this]()
	{
		SetClock(12.0f, LookSmokeMidsummer);
		const float NorthY = static_cast<float>(State->Room.y + LookRoomH - 1);
		const FVector To = CellWorld(static_cast<float>(State->Room.x) + 3.5f, NorthY + 0.5f, 150.0);
		const FVector From = CellWorld(static_cast<float>(State->Room.x) + 3.5f, NorthY - 3.6f, 165.0);
		ViewFrom(From, (To - From).Rotation());
	};
	WindowsShot.Verify = [=, this]()
	{
		NoteShot(TEXT("windows"));
		const UWindowLightSubsystem* WinLights = GetWorld()->GetSubsystem<UWindowLightSubsystem>();
		const USkySubsystem* SkyNow = Sky();
		Notes.Add(FString::Printf(TEXT("windows: %d window lights shining, indoor exposure share %.2f"), WinLights ? WinLights->GetShining() : -1, SkyNow ? SkyNow->GetIndoorShare() : -1.0f));
		if (!WinLights || WinLights->GetShining() < 2)
		{
			Fail(TEXT("windows: the room's two windows should each have a daylight light"));
		}
	};
	Phases.Add(MoveTemp(WindowsShot));

	// 8c. Sunbeams: at half past eight on a midsummer morning the sun is low in the north-east of the sky's turn (azimuth about
	// 128 degrees, 41 degrees up), so it shines in through the room's north windows and lays two bright patches on the floor and
	// furniture, which bounce light round the room (docs/design/windows.md "Drawing"). From the south-west corner, looking down
	// at where they fall.
	FPhase Sunbeams;
	Sunbeams.Name = TEXT("sunbeams");
	Sunbeams.WarmupSeconds = 4.0f;
	Sunbeams.MeasureSeconds = 2.0f;
	Sunbeams.Setup = [=, this]()
	{
		SetClock(8.5f, LookSmokeMidsummer);
		const FVector To = CellWorld(static_cast<float>(State->Room.x) + 3.8f, static_cast<float>(State->Room.y) + 3.6f, 20.0);
		const FVector From = CellWorld(static_cast<float>(State->Room.x) + 1.4f, static_cast<float>(State->Room.y) + 1.3f, 215.0);
		ViewFrom(From, (To - From).Rotation());
	};
	Sunbeams.Verify = [=, this]()
	{
		NoteShot(TEXT("sunbeams"));
		if (const USkySubsystem* SkyDust = Sky(); SkyDust && SkyDust->GetFog())
		{
			const UExponentialHeightFogComponent* DustFog = SkyDust->GetFog();
			Notes.Add(FString::Printf(TEXT("sunbeams: indoor share %.2f, volumetric fog %d, fog starts %.0f cm, extinction x%.2f, reach %.0f cm"), SkyDust->GetIndoorShare(),
				DustFog->bEnableVolumetricFog ? 1 : 0, DustFog->StartDistance, DustFog->VolumetricFogExtinctionScale, DustFog->VolumetricFogDistance));
		}
		if (const UWindowLightSubsystem* DustLights = GetWorld()->GetSubsystem<UWindowLightSubsystem>())
		{
			Notes.Add(FString::Printf(TEXT("sunbeams: %d rooms with dust in the air: %s"), DustLights->GetDustRooms(), *DustLights->DescribeDust()));
			const IConsoleVariable* LfvIntoFog = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LocalFogVolume.RenderIntoVolumetricFog"));
			const IConsoleVariable* LfvSupport = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SupportLocalFogVolumes"));
			const IConsoleVariable* VolFog = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricFog"));
			Notes.Add(FString::Printf(TEXT("sunbeams: r.VolumetricFog %s, r.SupportLocalFogVolumes %s, r.LocalFogVolume.RenderIntoVolumetricFog %s, camera at %s"),
				VolFog ? *VolFog->GetString() : TEXT("?"), LfvSupport ? *LfvSupport->GetString() : TEXT("?"), LfvIntoFog ? *LfvIntoFog->GetString() : TEXT("?"),
				*(GetWorld()->GetFirstPlayerController() && GetWorld()->GetFirstPlayerController()->PlayerCameraManager ? GetWorld()->GetFirstPlayerController()->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector).ToString()));
		}
	};
	Phases.Add(MoveTemp(Sunbeams));

	// 8d. The same view with thick fog held in the room (a test, not a look): if the volumetric fog shows the sunbeams here but not
	// with the rooms' dust, the dust volumes are wrong; if not even here, the fog is not lit by the sun indoors.
	FPhase FogTest;
	FogTest.Name = TEXT("sunbeams_fogtest");
	FogTest.WarmupSeconds = 3.0f;
	FogTest.MeasureSeconds = 1.0f;
	FogTest.Setup = [=, this]()
	{
		if (IConsoleVariable* IndoorFog = IConsoleManager::Get().FindConsoleVariable(TEXT("Look.IndoorFogDensity")))
		{
			IndoorFog->Set(0.3f, ECVF_SetByCode);
		}
		SetClock(8.5f, LookSmokeMidsummer);
		const FVector To = CellWorld(static_cast<float>(State->Room.x) + 3.8f, static_cast<float>(State->Room.y) + 3.6f, 20.0);
		const FVector From = CellWorld(static_cast<float>(State->Room.x) + 1.4f, static_cast<float>(State->Room.y) + 1.3f, 215.0);
		ViewFrom(From, (To - From).Rotation());
	};
	FogTest.Verify = [=, this]()
	{
		NoteShot(TEXT("sunbeams_fogtest"));
		if (const USkySubsystem* SkyFog = Sky(); SkyFog && SkyFog->GetFog())
		{
			Notes.Add(FString::Printf(TEXT("sunbeams_fogtest: fog density %.3f, volumetric %d"), SkyFog->GetFog()->FogDensity, SkyFog->GetFog()->bEnableVolumetricFog ? 1 : 0));
		}
		if (IConsoleVariable* IndoorFog = IConsoleManager::Get().FindConsoleVariable(TEXT("Look.IndoorFogDensity")))
		{
			IndoorFog->Set(-1.0f, ECVF_SetByCode);
		}
	};
	Phases.Add(MoveTemp(FogTest));

	// 8e. The rooms' dust again with the other reading of a local fog volume's size (a test): whichever of sunbeams and this shows
	// a shaft has the right one.
	FPhase DustSize;
	DustSize.Name = TEXT("sunbeams_base500");
	DustSize.WarmupSeconds = 3.0f;
	DustSize.MeasureSeconds = 1.0f;
	DustSize.Setup = [=, this]()
	{
		if (IConsoleVariable* Base = IConsoleManager::Get().FindConsoleVariable(TEXT("Windows.DustBaseCm")))
		{
			Base->Set(500.0f, ECVF_SetByCode);
		}
		SetClock(8.5f, LookSmokeMidsummer);
		const FVector To = CellWorld(static_cast<float>(State->Room.x) + 3.8f, static_cast<float>(State->Room.y) + 3.6f, 20.0);
		const FVector From = CellWorld(static_cast<float>(State->Room.x) + 1.4f, static_cast<float>(State->Room.y) + 1.3f, 215.0);
		ViewFrom(From, (To - From).Rotation());
	};
	DustSize.Verify = [=, this]()
	{
		NoteShot(TEXT("sunbeams_base500"));
		if (const UWindowLightSubsystem* SizeLights = GetWorld()->GetSubsystem<UWindowLightSubsystem>())
		{
			Notes.Add(FString::Printf(TEXT("sunbeams_base500: %s"), *SizeLights->DescribeDust()));
		}
		if (IConsoleVariable* Base = IConsoleManager::Get().FindConsoleVariable(TEXT("Windows.DustBaseCm")))
		{
			Base->Set(50.0f, ECVF_SetByCode);
		}
	};
	Phases.Add(MoveTemp(DustSize));

	// 9. A snowy day: a new game on the same seed snows hard through the cold night, then the overseer looks at it at eleven
	// on a midwinter morning. (The sim's first day is spring; the night is cold enough for the snow to lie.)
	FPhase Snowy;
	Snowy.Name = TEXT("snowy_day");
	Snowy.WarmupSeconds = 14.0f;
	Snowy.MeasureSeconds = 2.0f;
	Snowy.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 256;
		Options.Scenario = TEXT("Crashlanded");
		if (!B->StartSim(Options))
		{
			Fail(TEXT("snowy_day: the new game did not start"));
			return;
		}
		B->SetSpeed(0);
		Overseer()->EnterOverseerMode();
		SetWeather("SnowHard", 40.0f);
		RunFor(7 * colony::kSecondsPerHour);
		SetClock(11.0f, LookSmokeMidwinter);
		Aim(State->Home, -2.0f, -15.0f, 0.0f);
	};
	Snowy.OnFrame = Advance;
	Snowy.Verify = [=, this]()
	{
		NoteShot(TEXT("snowy_day"));
		SetClock(-1.0f, -1.0f);
		if (USnowSubsystem* S = GetWorld()->GetSubsystem<USnowSubsystem>(); S && S->GetChunksWithSnow() == 0)
		{
			Fail(TEXT("snowy_day: no snow is lying"));
		}
	};
	Phases.Add(MoveTemp(Snowy));

}
