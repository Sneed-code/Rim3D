// Smoke test `power` (docs/design/building-draw.md "Downloaded models with a moving part"): a wind turbine and a solar
// generator standing in the open, drawn with their downloaded models (assets/power_art.json), and the turbine's blades
// turning while it makes power. Pictures: the turbine close, the solar generator close, both from further off.
#include "SmokeTest.h"

#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"

#include "BuildingSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

namespace
{
	const FVector PowerSmokeOrigin(-860000.0, 0.0, 0.0);

	struct FPowerSmokeState
	{
		colony::CellPos Turbine{0, 0, 0};
		colony::CellPos Solar{0, 0, 0};
		colony::Entity TurbineId = colony::kNoEntity;
		float LastAngle = -1.0f;
		float Turned = 0.0f;
		float MaxRpm = 0.0f;
		float MaxWatts = 0.0f;
	};
}

void USmokeTestSubsystem::BuildPowerPhases()
{
	const TSharedRef<FPowerSmokeState> State = MakeShared<FPowerSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Drawn = [this]() { return GetWorld()->GetSubsystem<UBuildingSubsystem>(); };
	const auto LookAt = [=, this](colony::CellPos Cell, double Metres, double HeadingDegrees, double PitchDegrees, double RaiseMetres)
	{
		const FVector Target = Bridge()->SimToWorld(Cell.x + 0.5f, Cell.y + 0.5f, 0) + FVector(0.0, 0.0, RaiseMetres * 100.0);
		const FRotator Look(-PitchDegrees, HeadingDegrees, 0.0);
		ViewFrom(Target - Look.Vector() * Metres * 100.0, Look);
	};
	// Every frame: how far the turbine's blades turned, and the most power it made.
	const auto Watch = [=, this]()
	{
		UBuildingSubsystem* B = Drawn();
		const colony::Snapshot& Snapshot = Bridge()->GetCurrentSnapshot();
		if (State->TurbineId == colony::kNoEntity)
		{
			const colony::DefId Def = Bridge()->GetSim()->defs().idOf("Things", "WindTurbine");
			for (const colony::ThingView& View : Snapshot.things)
			{
				if (View.def == Def && View.stage == colony::ConstructionStage::Built)
				{
					State->TurbineId = View.id;
				}
			}
		}
		if (const colony::ThingView* View = State->TurbineId != colony::kNoEntity ? Snapshot.find(State->TurbineId) : nullptr)
		{
			State->MaxWatts = FMath::Max(State->MaxWatts, View->powerWatts);
		}
		float Angle = 0.0f;
		float Rpm = 0.0f;
		if (B && State->TurbineId != colony::kNoEntity && B->GetRotor(State->TurbineId, Angle, Rpm))
		{
			if (State->LastAngle >= 0.0f)
			{
				State->Turned += FMath::Abs(FMath::FindDeltaAngleDegrees(State->LastAngle, Angle));
			}
			State->LastAngle = Angle;
			State->MaxRpm = FMath::Max(State->MaxRpm, Rpm);
		}
	};

	// 1. A flat map, the turbine and the solar generator spawned standing, trees cleared round them.
	FPhase Setup;
	Setup.Name = TEXT("power_setup");
	Setup.WarmupSeconds = 2.0f;
	Setup.MeasureSeconds = 0.5f;
	Setup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Drawn())
		{
			Fail(TEXT("USimSubsystem or UBuildingSubsystem did not start"));
			return;
		}
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Terrain.Roofs")))
		{
			Var->Set(0, ECVF_SetByCode);
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 256;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(PowerSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		if (IConsoleVariable* Hour = IConsoleManager::Get().FindConsoleVariable(TEXT("Sky.Hour")))
		{
			Hour->Set(11.0f, ECVF_SetByCode);
		}
		colony::Sim* Sim = const_cast<colony::Sim*>(B->GetSim());
		const colony::CellPos Home = Sim->stockpileCell() ? *Sim->stockpileCell() : colony::CellPos{128, 128, 0};
		State->Turbine = colony::CellPos{Home.x + 16, Home.y + 2, 0};
		State->Solar = colony::CellPos{Home.x + 9, Home.y + 2, 0};
		// Trees and bushes cleared (a turbine with trees round it makes less), grass kept.
		for (int32 Y = Home.y - 8; Y <= Home.y + 12; ++Y)
		{
			for (int32 X = Home.x + 3; X <= Home.x + 26; ++X)
			{
				const colony::CellPos C{X, Y, 0};
				if (Sim->map().inBounds(C) && Sim->plants().has(C) && Sim->defs().get(Sim->plants().defAt(C)).defName() != "Plant_Grass")
				{
					Sim->plants().remove(C);
				}
			}
		}
		for (const TPair<const char*, colony::CellPos> Spawn : {TPair<const char*, colony::CellPos>("WindTurbine", State->Turbine), TPair<const char*, colony::CellPos>("SolarGenerator", State->Solar)})
		{
			colony::SpawnThingCommand Command;
			Command.def = Spawn.Key;
			Command.cell = Spawn.Value;
			Command.rot = colony::Rot4::North;
			Command.count = 1;
			B->Submit(Command);
		}
		for (int32 I = 0; I < 5; ++I)
		{
			B->StepOnce();
		}
		Drawn()->Refresh();
		B->SetSpeed(1);
		LookAt(State->Turbine, 14.0, 70.0, 12.0, 4.0);
	};
	Setup.OnFrame = Watch;
	Phases.Add(MoveTemp(Setup));

	// 2. The turbine close: its model, and its blades turning while it makes power.
	FPhase Turbine;
	Turbine.Name = TEXT("power_turbine");
	Turbine.WarmupSeconds = 4.0f;
	Turbine.MeasureSeconds = 4.0f;
	Turbine.ScreenshotAt = 3.0f;
	Turbine.Setup = [=]() { LookAt(State->Turbine, 14.0, 70.0, 12.0, 4.0); };
	Turbine.OnFrame = Watch;
	Turbine.Verify = [=, this]()
	{
		UBuildingSubsystem* B = Drawn();
		const colony::Sim* Sim = Bridge()->GetSim();
		if (!B || !Sim)
		{
			return;
		}
		if (State->TurbineId == colony::kNoEntity)
		{
			Fail(TEXT("power_turbine: no finished wind turbine in the sim"));
			return;
		}
		// The models are made by the power-models and power-art steps (tools/art_steps.json); without them the shapes show.
		for (const char* DefName : {"WindTurbine", "SolarGenerator"})
		{
			const colony::Def& Def = Sim->defs().get(Sim->defs().idOf("Things", DefName));
			for (const char* Path : {"visual.mesh", "visual.rotor.mesh"})
			{
				const std::string Mesh = Def.text(Path);
				if (!Mesh.empty() && !StaticLoadObject(UStaticMesh::StaticClass(), nullptr, UTF8_TO_TCHAR(Mesh.c_str())))
				{
					Fail(FString::Printf(TEXT("power_turbine: %s %s %s is not made (the power-models and power-art steps)"), UTF8_TO_TCHAR(DefName), UTF8_TO_TCHAR(Path), UTF8_TO_TCHAR(Mesh.c_str())));
				}
			}
		}
		if (B->GetRotorsDrawn() < 1)
		{
			Fail(TEXT("power_turbine: the wind turbine's blades are not drawn as a turning part"));
			return;
		}
		if (State->MaxWatts > 1.0f && State->Turned < 20.0f)
		{
			Fail(FString::Printf(TEXT("power_turbine: making %.0f W but the blades turned only %.1f degrees"), State->MaxWatts, State->Turned));
		}
		Notes.Add(FString::Printf(TEXT("power_turbine: up to %.0f W, blades turned %.0f degrees, up to %.1f turns a minute"), State->MaxWatts, State->Turned, State->MaxRpm));
		PhaseNumbers.Add(TEXT("turbine_watts"), State->MaxWatts);
		PhaseNumbers.Add(TEXT("blades_turned_deg"), State->Turned);
	};
	Phases.Add(MoveTemp(Turbine));

	// 3. The solar generator close.
	FPhase Solar;
	Solar.Name = TEXT("power_solar");
	Solar.WarmupSeconds = 2.0f;
	Solar.MeasureSeconds = 1.5f;
	Solar.Setup = [=]() { LookAt(State->Solar, 8.0, 60.0, 30.0, 0.5); };
	Solar.OnFrame = Watch;
	Phases.Add(MoveTemp(Solar));

	// 4. Both from further off and higher, the way the overseer sees them.
	FPhase Both;
	Both.Name = TEXT("power_both");
	Both.WarmupSeconds = 2.0f;
	Both.MeasureSeconds = 1.5f;
	Both.Setup = [=]() { LookAt(colony::CellPos{(State->Turbine.x + State->Solar.x) / 2, State->Turbine.y, 0}, 22.0, 30.0, 40.0, 1.0); };
	Both.OnFrame = Watch;
	Phases.Add(MoveTemp(Both));
}
