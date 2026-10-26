// Smoke test `fire` (task U14): the sim's lamps, torches, fires, burning pawns, burnt ground and ash, drawn as lights, flames,
// embers, smoke and ground skins, in the game, offscreen, at night.
#include "SmokeTest.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "FireSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fire.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <string>
#include <vector>

namespace
{
	const FVector FireSmokeOrigin(-1500000.0, 0.0, 0.0);

	struct FFireSmokeState
	{
		colony::CellPos Middle = colony::CellPos{48, 48, 0};
		int64 StepsWanted = 0;
		int64 StepsRun = 0;
		int32 MaxFlames = 0;
		int32 MaxSmoke = 0;
		int32 MaxEmbers = 0;
		int32 MaxLights = 0;
		int32 MaxParticles = 0;
		int32 MaxFireCells = 0;
		colony::Entity Torch = colony::kNoEntity;
		colony::Entity Pawn = colony::kNoEntity;

		void Reset()
		{
			MaxFlames = MaxSmoke = MaxEmbers = MaxLights = MaxParticles = MaxFireCells = 0;
		}
	};

	void FireSmokeSet(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByCode);
		}
	}
}

void USmokeTestSubsystem::BuildFirePhases()
{
	const TSharedRef<FFireSmokeState> State = MakeShared<FFireSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Fire = [this]() { return GetWorld()->GetSubsystem<UFireSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };
	const auto At = [=](int32 DX, int32 DY) { return colony::CellPos{State->Middle.x + DX, State->Middle.y + DY, 0}; };
	const auto Spawn = [=, this](const char* Def, int32 DX, int32 DY, int32 Count = 1)
	{
		Bridge()->Submit(colony::SpawnThingCommand{Def, At(DX, DY), Count, colony::Rot4::North});
	};
	const auto Look = [=, this](int32 DX, int32 DY, float Distance, float Height, float Pitch, float Yaw)
	{
		USimSubsystem* B = Bridge();
		const FVector Target = B->SimToWorld(State->Middle.x + DX + 0.5f, State->Middle.y + DY + 0.5f, 0) + FVector(0.0, 0.0, Height);
		const FVector From = Target + FRotator(0.0f, Yaw, 0.0f).RotateVector(FVector(-Distance, 0.0, 60.0));
		ViewFrom(From, (Target - From).Rotation() + FRotator(Pitch, 0.0f, 0.0f));
	};
	const auto Advance = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim() || State->StepsRun >= State->StepsWanted)
		{
			return;
		}
		const double Start = FPlatformTime::Seconds();
		int32 Count = 0;
		while (State->StepsRun < State->StepsWanted && (FPlatformTime::Seconds() - Start) < 0.03 && Count < 300)
		{
			B->StepOnce();
			++State->StepsRun;
			++Count;
		}
	};
	const auto Watch = [=, this]()
	{
		UFireSubsystem* F = Fire();
		if (!F || !F->IsBuilt())
		{
			return;
		}
		State->MaxFlames = FMath::Max(State->MaxFlames, F->GetParticles(EFireParticle::FlameCore) + F->GetParticles(EFireParticle::FlameMid) + F->GetParticles(EFireParticle::FlameTip));
		State->MaxSmoke = FMath::Max(State->MaxSmoke, F->GetParticles(EFireParticle::Smoke));
		State->MaxEmbers = FMath::Max(State->MaxEmbers, F->GetParticles(EFireParticle::Ember));
		State->MaxLights = FMath::Max(State->MaxLights, F->GetLights().Num());
		State->MaxParticles = FMath::Max(State->MaxParticles, F->GetParticleTotal());
		State->MaxFireCells = FMath::Max(State->MaxFireCells, F->GetFireCells());
	};

	// 1. A campfire and a torch (with fuel), a standing lamp on a generator's wire and one with no power, at night.
	FPhase Lamps;
	Lamps.Name = TEXT("fire_lamps");
	Lamps.WarmupSeconds = 7.0f;
	Lamps.MeasureSeconds = 3.0f;
	Lamps.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Fire())
		{
			Fail(TEXT("USimSubsystem or UFireSubsystem did not start"));
			return;
		}
		FireSmokeSet(TEXT("Sky.Hour"), 0.5f);
		FireSmokeSet(TEXT("Sky.DayOfYear"), 22.5f);
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 96;
		Options.bFlatMap = true;
		B->SetWorldOrigin(FireSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		State->Middle = colony::CellPos{48, 48, 0};
		State->Reset();
		B->SetSpeed(0);
		Spawn("Campfire", -6, 0);
		Spawn("TorchLamp", -3, 0);
		Spawn("WoodFiredGenerator", 3, -4);
		for (int32 X = 5; X <= 8; ++X)
		{
			Spawn("PowerConduit", X, -4);
		}
		Spawn("StandingLamp", 9, -4);
		Spawn("StandingLamp", 5, 4);  // no wire: dark
		B->StepOnce();
		colony::Sim* Sim = SimOf();
		for (const colony::CellPos Cell : {At(-6, 0), At(-3, 0), At(3, -4)})
		{
			const colony::Entity Building = Sim->map().buildingAt(Cell);
			if (Building == colony::kNoEntity)
			{
				Fail(FString::Printf(TEXT("fire_lamps: nothing was built at (%d, %d)"), Cell.x, Cell.y));
				continue;
			}
			if (Sim->things().registry().all_of<colony::Fuel>(Building))
			{
				Sim->jobs().fuel().add(Building, static_cast<int>(Sim->jobs().fuel().specOf(Building).capacity));
			}
			if (Cell.x == At(-3, 0).x)
			{
				State->Torch = Building;
			}
		}
		B->StepOnce();
		B->StepOnce();
		B->SetSpeed(1);
		Look(-1, -2, 760.0f, 60.0f, -3.0f, 0.0f);
	};
	Lamps.OnFrame = Watch;
	Lamps.Verify = [=, this]()
	{
		UFireSubsystem* F = Fire();
		const colony::Sim* Sim = SimOf();
		if (!F || !Sim)
		{
			return;
		}
		Notes.Add(FString::Printf(TEXT("fire_lamps: the sim lit %d lamps; %d lights, %d flames, %d embers, %d lamps with flames"), F->GetLampSources(), F->GetLights().Num(),
			State->MaxFlames, State->MaxEmbers, F->GetFlameLamps()));
		if (F->GetLampSources() != 3)
		{
			Fail(FString::Printf(TEXT("fire_lamps: %d lit lamps, wanted the campfire, the torch and the powered standing lamp (3)"), F->GetLampSources()));
		}
		if (F->GetLights().Num() < 3)
		{
			Fail(FString::Printf(TEXT("fire_lamps: %d real lights for 3 lit lamps"), F->GetLights().Num()));
		}
		int32 Warm = 0;
		for (const FFireLightSlot& Slot : F->GetLights())
		{
			if (Slot.Intensity <= 1.0f || Slot.RadiusCm < 500.0f)
			{
				Fail(FString::Printf(TEXT("fire_lamps: a light has intensity %.1f and radius %.0f cm"), Slot.Intensity, Slot.RadiusCm));
			}
			Warm += Slot.Color.R > Slot.Color.B ? 1 : 0;
		}
		if (Warm < 3)
		{
			Fail(TEXT("fire_lamps: the lamps' lights are not warm coloured"));
		}
		if (F->GetFlameLamps() != 2 || State->MaxFlames < 8 || State->MaxEmbers < 1)
		{
			Fail(FString::Printf(TEXT("fire_lamps: %d lamps with flames, %d flames and %d embers at most (the campfire and the torch should burn)"), F->GetFlameLamps(), State->MaxFlames, State->MaxEmbers));
		}
		// The dark lamp (no wire) has no light near it.
		const FVector Dark = Bridge()->SimToWorld(State->Middle.x + 5.5f, State->Middle.y + 4.5f, 0);
		for (const FFireLightSlot& Slot : F->GetLights())
		{
			if (FVector::Dist2D(Slot.Position, Dark) < 60.0)
			{
				Fail(TEXT("fire_lamps: the standing lamp with no power has a light"));
			}
		}
	};
	Phases.Add(MoveTemp(Lamps));

	// 1b. The campfire close up: flames, embers and the light it throws.
	FPhase Close;
	Close.Name = TEXT("fire_campfire_close");
	Close.WarmupSeconds = 3.0f;
	Close.MeasureSeconds = 2.0f;
	Close.Setup = [=, this]() { State->Reset(); Look(-6, 0, 200.0f, 28.0f, 4.0f, 0.0f); };
	Close.OnFrame = Watch;
	Close.Verify = [=, this]()
	{
		if (State->MaxFlames < 6)
		{
			Fail(FString::Printf(TEXT("fire_campfire_close: %d flames at most at the campfire"), State->MaxFlames));
		}
	};
	Phases.Add(MoveTemp(Close));

	// 2. Wood piles set alight: flames, embers, smoke and orange light in proportion to the fire.
	FPhase Burning;
	Burning.Name = TEXT("fire_burning");
	Burning.WarmupSeconds = 14.0f;
	Burning.MeasureSeconds = 3.0f;
	Burning.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		State->Reset();
		for (int32 I = 0; I < 7; ++I)
		{
			Spawn("WoodLog", I - 3, 8, 75);
		}
		B->StepOnce();
		for (int32 I = 0; I < 7; I += 2)
		{
			B->Submit(colony::IgniteCommand{At(I - 3, 8), 0.9f, colony::kNoEntity, false});
		}
		B->StepOnce();
		B->SetSpeed(3);
		Look(0, 8, 620.0f, 50.0f, 4.0f, 0.0f);
	};
	Burning.OnFrame = Watch;
	Burning.Verify = [=, this]()
	{
		UFireSubsystem* F = Fire();
		const colony::Sim* Sim = SimOf();
		if (!F || !Sim)
		{
			return;
		}
		float SimSum = 0.0f;
		for (const auto& Pair : Sim->fire().all())
		{
			SimSum += Pair.second.size;
		}
		Notes.Add(FString::Printf(TEXT("fire_burning: the sim has %d fires (sum of sizes %.2f), drawn %d (%.2f); %d flames, %d embers, %d smoke puffs, %d lights, %d particles at most"),
			static_cast<int32>(Sim->fire().count()), SimSum, F->GetFireCells(), F->GetFireSizeSum(), State->MaxFlames, State->MaxEmbers, State->MaxSmoke, State->MaxLights, State->MaxParticles));
		if (Sim->fire().count() < 3)
		{
			Fail(FString::Printf(TEXT("fire_burning: the sim has only %d fires"), static_cast<int32>(Sim->fire().count())));
			return;
		}
		if (F->GetFireCells() < Sim->fire().count() / 2 || FMath::Abs(F->GetFireSizeSum() - SimSum) > SimSum * 0.35f + 0.3f)
		{
			Fail(FString::Printf(TEXT("fire_burning: %d cells drawn with sizes %.2f, the sim has %d with %.2f"), F->GetFireCells(), F->GetFireSizeSum(), static_cast<int32>(Sim->fire().count()), SimSum));
		}
		if (State->MaxFlames < 40 || State->MaxEmbers < 2 || State->MaxSmoke < 5)
		{
			Fail(FString::Printf(TEXT("fire_burning: %d flames, %d embers and %d smoke puffs at most"), State->MaxFlames, State->MaxEmbers, State->MaxSmoke));
		}
		bool bFireLight = false;
		for (const FFireLightSlot& Slot : F->GetLights())
		{
			bFireLight = bFireLight || (Slot.Kind == EFireLightKind::Fire && Slot.Color.R > Slot.Color.G && Slot.Color.G > Slot.Color.B && Slot.Intensity > 8.0f);
		}
		if (!bFireLight)
		{
			Fail(TEXT("fire_burning: no orange light over the fire"));
		}
		if (State->MaxParticles > FMath::Max(1, F->GetParticleTotal()) * 40)
		{
			Fail(TEXT("fire_burning: the particle count jumped about"));
		}
	};
	Phases.Add(MoveTemp(Burning));

	// 3. A pawn on fire.
	FPhase PawnFire;
	PawnFire.Name = TEXT("fire_pawn");
	PawnFire.WarmupSeconds = 4.0f;
	PawnFire.MeasureSeconds = 3.0f;
	PawnFire.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		State->Reset();
		B->SetSpeed(0);
		Spawn("Human", -8, -6);
		B->StepOnce();
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.lifeStage < 0 && FMath::Abs(View.x - (State->Middle.x - 8 + 0.5f)) < 1.0f && FMath::Abs(View.y - (State->Middle.y - 6 + 0.5f)) < 1.0f)
			{
				State->Pawn = View.id;
			}
		}
		if (State->Pawn == colony::kNoEntity)
		{
			Fail(TEXT("fire_pawn: the pawn was not made"));
			return;
		}
		B->Submit(colony::IgniteCommand{colony::CellPos{State->Middle.x - 8, State->Middle.y - 6, 0}, 0.0f, State->Pawn, true});
		B->StepOnce();
		B->SetSpeed(1);
		Look(-8, -6, 330.0f, 100.0f, 0.0f, 0.0f);
	};
	PawnFire.OnFrame = Watch;
	PawnFire.Verify = [=, this]()
	{
		UFireSubsystem* F = Fire();
		if (!F)
		{
			return;
		}
		bool bPawnLight = false;
		for (const FFireLightSlot& Slot : F->GetLights())
		{
			bPawnLight = bPawnLight || Slot.Kind == EFireLightKind::Pawn;
		}
		Notes.Add(FString::Printf(TEXT("fire_pawn: %d burning pawn(s) drawn, a pawn light: %d, %d flames at most"), F->GetBurningPawns(), bPawnLight, State->MaxFlames));
		if (F->GetBurningPawns() < 1 || !bPawnLight)
		{
			Fail(FString::Printf(TEXT("fire_pawn: %d burning pawns drawn, pawn light %d"), F->GetBurningPawns(), bPawnLight));
		}
	};
	Phases.Add(MoveTemp(PawnFire));

	// 4. The fires burn the wood to ash: burnt ground and ash skins, the same cells as the sim has.
	FPhase Burnt;
	Burnt.Name = TEXT("fire_burnt");
	Burnt.WarmupSeconds = 30.0f;
	Burnt.MeasureSeconds = 3.0f;
	Burnt.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		State->Reset();
		State->StepsWanted = 6 * colony::kSecondsPerHour;
		State->StepsRun = 0;
		B->SetSpeed(1);
		Look(0, 8, 560.0f, 20.0f, -10.0f, 0.0f);
	};
	Burnt.OnFrame = [=]() { Advance(); };
	Burnt.Verify = [=, this]()
	{
		UFireSubsystem* F = Fire();
		const colony::Sim* Sim = SimOf();
		if (!F || !Sim)
		{
			return;
		}
		F->SyncGround();
		const colony::Snapshot& Snapshot = Bridge()->GetCurrentSnapshot();
		const colony::DefId Ash = Sim->defs().idOf("Filth", "Ash");
		int32 AshCells = 0;
		for (const colony::FilthView& Filth : Snapshot.filth)
		{
			AshCells += Filth.def == Ash ? 1 : 0;
		}
		Notes.Add(FString::Printf(TEXT("fire_burnt: %lld of %lld steps run; the sim has %d burnt cells and %d ash piles, %d and %d drawn in %d chunk meshes (slowest %.1f ms); %d fires left"),
			State->StepsRun, State->StepsWanted, static_cast<int32>(Sim->fire().burntCells().size()), AshCells, F->GetBurntDrawn(), F->GetAshDrawn(), F->GetGroundChunks(),
			F->GetSlowestChunkMs(), static_cast<int32>(Sim->fire().count())));
		if (State->StepsRun < State->StepsWanted)
		{
			Fail(TEXT("fire_burnt: the sim did not run the six game hours in time"));
		}
		if (Sim->fire().burntCells().size() < 3 || AshCells < 2)
		{
			Fail(FString::Printf(TEXT("fire_burnt: the sim has %d burnt cells and %d ash piles after six hours"), static_cast<int32>(Sim->fire().burntCells().size()), AshCells));
		}
		if (F->GetBurntDrawn() != static_cast<int32>(Sim->fire().burntCells().size()) || F->GetAshDrawn() != AshCells)
		{
			Fail(FString::Printf(TEXT("fire_burnt: %d burnt and %d ash drawn, the sim has %d and %d"), F->GetBurntDrawn(), F->GetAshDrawn(),
				static_cast<int32>(Sim->fire().burntCells().size()), AshCells));
		}
		if (F->GetSlowestChunkMs() > 50.0)
		{
			Fail(FString::Printf(TEXT("fire_burnt: a chunk of ground took %.1f ms"), F->GetSlowestChunkMs()));
		}
	};
	Phases.Add(MoveTemp(Burnt));

	// 4b. The same ground in daylight from above: black where it burnt, grey patches of ash.
	FPhase Day;
	Day.Name = TEXT("fire_burnt_day");
	Day.WarmupSeconds = 3.0f;
	Day.MeasureSeconds = 2.0f;
	Day.Setup = [=, this]()
	{
		FireSmokeSet(TEXT("Sky.Hour"), 11.0f);
		USimSubsystem* B = Bridge();
		B->SetSpeed(0);
		const FVector Target = B->SimToWorld(State->Middle.x + 0.5f, State->Middle.y + 8.5f, 0);
		const FVector From = Target + FVector(-1200.0, 0.0, 1800.0);
		ViewFrom(From, (Target - From).Rotation());
	};
	Phases.Add(MoveTemp(Day));

	// 5. The cost: a big fire (two hundred piles of wood alight at once), the nearest lights only, the particle cap held.
	FPhase Big;
	Big.Name = TEXT("fire_big");
	Big.WarmupSeconds = 12.0f;
	Big.MeasureSeconds = 6.0f;
	Big.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		State->Reset();
		// A fresh map: the last phases burnt the old one bare.
		FSimStartOptions Options;
		Options.Seed = 43;
		Options.MapSize = 96;
		Options.bFlatMap = true;
		B->SetWorldOrigin(FireSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		for (int32 I = 0; I < 200; ++I)
		{
			Spawn("WoodLog", -20 + I % 20 * 2, -30 + I / 20 * 2, 75);
		}
		B->StepOnce();
		for (int32 I = 0; I < 200; ++I)
		{
			B->Submit(colony::IgniteCommand{At(-20 + I % 20 * 2, -30 + I / 20 * 2), 1.0f, colony::kNoEntity, false});
		}
		B->StepOnce();
		B->SetSpeed(1);
		Look(-1, -21, 1400.0f, 100.0f, -8.0f, 0.0f);
	};
	Big.OnFrame = Watch;
	Big.Verify = [=, this]()
	{
		UFireSubsystem* F = Fire();
		if (!F)
		{
			return;
		}
		Notes.Add(FString::Printf(TEXT("fire_big: %d fire cells drawn, %d lights, %d particles at most (cap %d)"), State->MaxFireCells, State->MaxLights, State->MaxParticles, 3500));
		if (State->MaxFireCells < 100)
		{
			Fail(FString::Printf(TEXT("fire_big: only %d fire cells drawn"), State->MaxFireCells));
		}
		if (State->MaxParticles > 3500 + 200 || State->MaxLights > 24)
		{
			Fail(FString::Printf(TEXT("fire_big: %d particles and %d lights at once, over the caps"), State->MaxParticles, State->MaxLights));
		}
		FireSmokeSet(TEXT("Sky.Hour"), -1.0f);
		FireSmokeSet(TEXT("Sky.DayOfYear"), -1.0f);
	};
	Phases.Add(MoveTemp(Big));
}
