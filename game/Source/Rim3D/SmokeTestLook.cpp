// Smoke test `look` (task L1, the look bench): one fixed seed and the same shots every run, so each look task can show
// sneed a before and after sheet (tools/look_sheet.ps1 copies the shots into Saved/Look/<date>/ and lays them beside the
// last run's). The shots: the overseer high and low over the colony, a colonist close up, inside a lit room, dusk, a
// rainy night, the HUD as the player first sees it, and a snowy day.
#include "SmokeTest.h"

#include "Components/DirectionalLightComponent.h"
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
#include "SnowSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <optional>

namespace
{
	const FVector LookSmokeOrigin(-1300000.0, 0.0, 0.0);
	constexpr float LookSmokeMidsummer = 22.5f;
	constexpr float LookSmokeMidwinter = 52.5f;
	// The room: 7 x 6 cells with its walls, a door in the south wall.
	constexpr int32 LookRoomW = 7;
	constexpr int32 LookRoomH = 6;

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
		Options.Scenario = TEXT("Crashlanded");
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
		// No trees or bushes in the room.
		for (int32 Y = -1; Y <= LookRoomH; ++Y)
		{
			for (int32 X = -1; X <= LookRoomW; ++X)
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
		B->Submit(colony::SpawnThingCommand{"Bed", At(1, 3), 1, colony::Rot4::East});
		B->Submit(colony::SpawnThingCommand{"EndTable", At(1, 1), 1});
		B->Submit(colony::SpawnThingCommand{"Table1x2", At(4, 2), 1});
		B->Submit(colony::SpawnThingCommand{"Armchair", At(5, 3), 1, colony::Rot4::West});
		B->Submit(colony::SpawnThingCommand{"TorchLamp", At(5, 1), 1});
		B->Submit(colony::SpawnThingCommand{"TorchLamp", At(1, 4), 1});
		// The sim's rooms put a thin roof over the closed room by themselves.
		B->StepOnce();
		colony::Sim* Sim = SimOf();
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
		Notes.Add(FString::Printf(TEXT("_prepare: home (%d, %d), room at (%d, %d), %d torches fuelled"), State->Home.x, State->Home.y, State->Room.x, State->Room.y, Torches));
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

	// 8. A snowy day: a new game on the same seed snows hard through the cold night, then the overseer looks at it at eleven
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
