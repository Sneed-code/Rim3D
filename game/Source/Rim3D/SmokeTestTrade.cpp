// Smoke test `trade` (task U19): the trade strip and window, the gift and factions windows, and the thermostat panel, driven by the calls their buttons make, in the game, offscreen.
#include "SmokeTest.h"

#include "HAL/IConsoleManager.h"

#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "PanelsSubsystem.h"
#include "SimSubsystem.h"
#include "TradePanelSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"
#include "ColonySim/Trade.h"

#include <cmath>
#include <string>

namespace
{
	const FVector TradeSmokeOrigin(-780000.0, 0.0, 0.0);

	struct FTradeSmokeState
	{
		uint32 Visit = 0;
		int32 SilverBefore = 0;
		int32 SteelBefore = 0;
		float GoodwillBefore = 0.0f;
		colony::Entity Heater = colony::kNoEntity;
		colony::Entity Stove = colony::kNoEntity;
	};
}

void USmokeTestSubsystem::BuildTradePhases()
{
	const TSharedRef<FTradeSmokeState> State = MakeShared<FTradeSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Panels = [this]() { return GetWorld()->GetSubsystem<UPanelsSubsystem>(); };
	const auto Trade = [this]() { return GetWorld()->GetSubsystem<UTradePanelSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };
	const auto SimOf = [=, this]() -> const colony::Sim* { USimSubsystem* B = Bridge(); return B ? B->GetSim() : nullptr; };
	const auto Select = [=, this](colony::Entity Thing)
	{
		FSimPick Pick;
		Pick.Kind = ESimPickKind::Thing;
		Pick.Thing = Thing;
		Overseer()->Select(Pick);
	};
	const auto Home = [=, this]() -> colony::CellPos
	{
		const colony::Sim* Sim = SimOf();
		return Sim && Sim->stockpileCell() ? *Sim->stockpileCell() : colony::CellPos{128, 128, 0};
	};
	const auto Aim = [=, this]()
	{
		AOverseerPawn* Pawn = Overseer() ? Overseer()->GetOverseerPawn() : nullptr;
		if (Pawn)
		{
			Pawn->ResetView();
			Pawn->Zoom(2.0f);
			Pawn->FocusOn(Bridge()->SimToWorld(Home().x + 0.5f, Home().y + 0.5f, 0));
			Pawn->SnapCamera();
		}
	};
	const auto Count = [=, this](const char* Thing) -> int32 { return SimOf()->trade().colonyHas(Thing); };
	const auto StockOf = [=, this](const char* Thing) -> int32
	{
		for (const colony::VisitView& Visit : Bridge()->GetCurrentSnapshot().visits)
		{
			if (Visit.id == State->Visit)
			{
				for (const colony::StockView& Line : Visit.stock)
				{
					if (Line.thing == Thing)
					{
						return Line.count;
					}
				}
			}
		}
		return 0;
	};

	// 1. A new sim with the Crashlanded colonists, silver and steel on the ground, and a trade ship overhead.
	FPhase Setup;
	Setup.Name = TEXT("trade_setup");
	Setup.WarmupSeconds = 3.0f;
	Setup.MeasureSeconds = 0.5f;
	Setup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Panels() || !Trade() || !Overseer())
		{
			Fail(TEXT("USimSubsystem, UPanelsSubsystem, UTradePanelSubsystem or UOverseerSubsystem did not start"));
			return;
		}
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Terrain.Roofs")))
		{
			Var->Set(0, ECVF_SetByCode);
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 256;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(TradeSmokeOrigin);
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
		Aim();
		// Silver for the colony and steel to sell, in stacks next to the stockpile.
		const colony::CellPos Cell = Home();
		for (int32 Index = 0; Index < 6; ++Index)
		{
			B->Submit(colony::SpawnThingCommand{"Silver", colony::CellPos{Cell.x + Index, Cell.y + 4, 0}, 200, colony::Rot4::North});
			B->Submit(colony::SpawnThingCommand{"Steel", colony::CellPos{Cell.x + Index, Cell.y + 5, 0}, 75, colony::Rot4::North});
		}
		B->StepOnce();
		B->Submit(colony::StartVisitCommand{"FrayCompact", "Orbital", "BulkGoods", -1});
		B->StepOnce();
		for (const colony::VisitView& Visit : B->GetCurrentSnapshot().visits)
		{
			State->Visit = Visit.id;
		}
	};
	Setup.Verify = [=, this]()
	{
		if (State->Visit == 0)
		{
			Fail(TEXT("trade_setup: the trade ship did not come"));
			return;
		}
		if (Trade()->GetStats().StripFrames == 0)
		{
			Fail(TEXT("trade_setup: the trade strip was never drawn (ImGui not usable)"));
		}
		if (SimOf()->trade().silverOnMap() < 1000 || Count("Steel") < 300)
		{
			Fail(FString::Printf(TEXT("trade_setup: %d silver and %d steel on the map, wanted 1000 and 300"), SimOf()->trade().silverOnMap(), Count("Steel")));
		}
		Notes.Add(FString::Printf(TEXT("trade_setup: visit %u, %d silver, %d steel"), State->Visit, SimOf()->trade().silverOnMap(), Count("Steel")));
	};
	Phases.Add(MoveTemp(Setup));

	// 2. Buy steel: the quote follows the lines, the deal pays silver and the steel is put down on the map.
	FPhase Buy;
	Buy.Name = TEXT("trade_buy");
	Buy.WarmupSeconds = 1.5f;
	Buy.MeasureSeconds = 1.0f;
	Buy.Setup = [=, this]()
	{
		UTradePanelSubsystem* T = Trade();
		if (State->Visit == 0 || !T)
		{
			return;
		}
		if (!T->OpenTrade(State->Visit))
		{
			Fail(TEXT("trade_buy: the trade window would not open on the ship"));
			return;
		}
		State->SilverBefore = SimOf()->trade().silverOnMap();
		State->SteelBefore = Count("Steel");
		if (StockOf("Steel") < 40)
		{
			Fail(TEXT("trade_buy: the ship has too little steel to try"));
			return;
		}
		T->SetBuy(TEXT("Steel"), 100000);  // held to the stock
		if (T->GetBuy(TEXT("Steel")) != StockOf("Steel"))
		{
			Fail(TEXT("trade_buy: a count over the stock was not held to the stock"));
		}
		T->SetBuy(TEXT("Steel"), 40);
		FTradePanelQuote Quote;
		if (!T->GetQuote(Quote) || !Quote.bAnything || Quote.BuyCost <= 0 || Quote.SellGain != 0 || Quote.Net != Quote.BuyCost)
		{
			Fail(FString::Printf(TEXT("trade_buy: quote for 40 steel was buy %d sell %d net %d"), Quote.BuyCost, Quote.SellGain, Quote.Net));
			return;
		}
		Notes.Add(FString::Printf(TEXT("trade_buy: 40 steel cost %d silver"), Quote.BuyCost));
		if (!T->MakeTrade())
		{
			Fail(TEXT("trade_buy: Make the trade sent nothing"));
		}
	};
	Buy.Verify = [=, this]()
	{
		UTradePanelSubsystem* T = Trade();
		if (T->GetStats().TradeFrames == 0)
		{
			Fail(TEXT("trade_buy: the trade window was never drawn"));
		}
		const int32 Steel = Count("Steel");
		const int32 Silver = SimOf()->trade().silverOnMap();
		if (Steel != State->SteelBefore + 40)
		{
			Fail(FString::Printf(TEXT("trade_buy: steel went from %d to %d, wanted +40"), State->SteelBefore, Steel));
		}
		if (Silver >= State->SilverBefore)
		{
			Fail(FString::Printf(TEXT("trade_buy: silver went from %d to %d, wanted less"), State->SilverBefore, Silver));
		}
		if (T->GetBuy(TEXT("Steel")) != 0)
		{
			Fail(TEXT("trade_buy: the lines were not cleared after the deal"));
		}
		Notes.Add(FString::Printf(TEXT("trade_buy: silver %d -> %d, steel %d -> %d"), State->SilverBefore, Silver, State->SteelBefore, Steel));
	};
	Phases.Add(MoveTemp(Buy));

	// 3. Sell steel: the trader pays silver, the steel leaves the map, and a deal the colony can't pay for is flagged.
	FPhase Sell;
	Sell.Name = TEXT("trade_sell");
	Sell.WarmupSeconds = 1.5f;
	Sell.MeasureSeconds = 1.0f;
	Sell.Setup = [=, this]()
	{
		UTradePanelSubsystem* T = Trade();
		if (State->Visit == 0 || !T)
		{
			return;
		}
		// First, a buy the colony can't pay for shows as unaffordable and the button would stay off.
		for (const colony::VisitView& Visit : Bridge()->GetCurrentSnapshot().visits)
		{
			if (Visit.id == State->Visit)
			{
				for (const colony::StockView& Line : Visit.stock)
				{
					T->SetBuy(UTF8_TO_TCHAR(Line.thing.c_str()), Line.count);
				}
			}
		}
		FTradePanelQuote Quote;
		T->GetQuote(Quote);
		if (Quote.bAffordable || Quote.Problem.IsEmpty())
		{
			Fail(FString::Printf(TEXT("trade_sell: a buy of %d silver was not flagged as unaffordable (%d silver on the map)"), Quote.Net, SimOf()->trade().silverOnMap()));
		}
		T->ClearLines();
		State->SilverBefore = SimOf()->trade().silverOnMap();
		State->SteelBefore = Count("Steel");
		T->SetSell(TEXT("Steel"), 100);
		T->GetQuote(Quote);
		if (Quote.SellGain <= 0 || Quote.BuyCost != 0 || Quote.Net != -Quote.SellGain)
		{
			Fail(FString::Printf(TEXT("trade_sell: quote for 100 steel was buy %d sell %d net %d"), Quote.BuyCost, Quote.SellGain, Quote.Net));
			return;
		}
		State->GoodwillBefore = static_cast<float>(Quote.SellGain);  // reused: what the sale should bring
		if (!T->MakeTrade())
		{
			Fail(TEXT("trade_sell: Make the trade sent nothing"));
		}
	};
	Sell.Verify = [=, this]()
	{
		const int32 Steel = Count("Steel");
		const int32 Silver = SimOf()->trade().silverOnMap();
		if (Steel != State->SteelBefore - 100)
		{
			Fail(FString::Printf(TEXT("trade_sell: steel went from %d to %d, wanted -100"), State->SteelBefore, Steel));
		}
		if (Silver != State->SilverBefore + static_cast<int32>(State->GoodwillBefore))
		{
			Fail(FString::Printf(TEXT("trade_sell: silver went from %d to %d, wanted +%d"), State->SilverBefore, Silver, static_cast<int32>(State->GoodwillBefore)));
		}
		Notes.Add(FString::Printf(TEXT("trade_sell: silver %d -> %d, steel %d -> %d"), State->SilverBefore, Silver, State->SteelBefore, Steel));
	};
	Phases.Add(MoveTemp(Sell));

	// 4. A gift raises goodwill by its value over 40 silver.
	FPhase Gift;
	Gift.Name = TEXT("trade_gift");
	Gift.WarmupSeconds = 1.5f;
	Gift.MeasureSeconds = 1.0f;
	Gift.Setup = [=, this]()
	{
		UTradePanelSubsystem* T = Trade();
		if (State->Visit == 0 || !T)
		{
			return;
		}
		if (!T->OpenGift(State->Visit) || !T->IsGiftMode())
		{
			Fail(TEXT("trade_gift: the gift window would not open"));
			return;
		}
		// Bring goodwill down so a rise is not lost at the cap.
		Bridge()->Submit(colony::AdjustGoodwillCommand{"FrayCompact", -60.0f});
		Bridge()->StepOnce();
		State->GoodwillBefore = SimOf()->trade().goodwill("FrayCompact");
		T->SetGift(TEXT("Steel"), 30);
		const float Expected = T->GiftGoodwill();
		if (Expected <= 0.0f)
		{
			Fail(TEXT("trade_gift: 30 steel is worth no goodwill"));
			return;
		}
		T->MakeGift();
		Bridge()->StepOnce();
		const float After = SimOf()->trade().goodwill("FrayCompact");
		if (std::fabs((After - State->GoodwillBefore) - Expected) > 0.05f)
		{
			Fail(FString::Printf(TEXT("trade_gift: goodwill went from %.2f to %.2f, the panel promised +%.2f"), State->GoodwillBefore, After, Expected));
		}
		Notes.Add(FString::Printf(TEXT("trade_gift: goodwill %.2f -> %.2f (+%.2f promised)"), State->GoodwillBefore, After, Expected));
		T->SetGift(TEXT("Steel"), 20);  // left on screen for the picture
		T->SetGift(TEXT("Silver"), 80);
	};
	Gift.Verify = [=, this]()
	{
		if (Trade()->GetStats().GiftFrames == 0)
		{
			Fail(TEXT("trade_gift: the gift window was never drawn"));
		}
	};
	Phases.Add(MoveTemp(Gift));

	// 5. The factions window lists every faction.
	FPhase Factions;
	Factions.Name = TEXT("trade_factions");
	Factions.WarmupSeconds = 1.5f;
	Factions.MeasureSeconds = 1.0f;
	Factions.Setup = [=, this]()
	{
		UTradePanelSubsystem* T = Trade();
		if (!T)
		{
			return;
		}
		T->Close();
		T->SetFactionsOpen(true);
		if (Bridge()->GetCurrentSnapshot().goodwill.size() < 4)
		{
			Fail(TEXT("trade_factions: the snapshot lists fewer than 4 factions"));
		}
	};
	Factions.Verify = [=, this]()
	{
		if (Trade()->GetStats().FactionFrames == 0)
		{
			Fail(TEXT("trade_factions: the factions window was never drawn"));
		}
		Trade()->SetFactionsOpen(false);
	};
	Phases.Add(MoveTemp(Factions));

	// 6. The thermostat slider: a heater selected, its target set and reset; a stove has none.
	FPhase Thermostat;
	Thermostat.Name = TEXT("trade_thermostat");
	Thermostat.WarmupSeconds = 1.5f;
	Thermostat.MeasureSeconds = 1.0f;
	Thermostat.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		UPanelsSubsystem* P = Panels();
		if (!B || !P)
		{
			return;
		}
		for (int32 R = 6; R < 16 && (State->Heater == colony::kNoEntity || State->Stove == colony::kNoEntity); ++R)
		{
			const colony::CellPos Cell{Home().x + R, Home().y - R, 0};
			const char* Def = State->Heater == colony::kNoEntity ? "Heater" : "FueledStove";
			B->Submit(colony::SpawnThingCommand{Def, Cell, 1, colony::Rot4::North});
			B->StepOnce();
			for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
			{
				if (View.category == colony::ThingCategory::Building && View.stage == colony::ConstructionStage::Built)
				{
					const std::string& Name = SimOf()->defs().get(View.def).defName();
					if (Name == "Heater")
					{
						State->Heater = View.id;
					}
					else if (Name == "FueledStove")
					{
						State->Stove = View.id;
					}
				}
			}
		}
		if (State->Heater == colony::kNoEntity || State->Stove == colony::kNoEntity)
		{
			Fail(TEXT("trade_thermostat: no heater or stove could be placed"));
			return;
		}
		Select(State->Heater);
		const colony::TemperatureSystem& Temperature = SimOf()->temperature();
		if (Temperature.hasThermostat(State->Heater))
		{
			Fail(TEXT("trade_thermostat: a new heater already has its own target"));
		}
		if (!P->SetThermostat(State->Heater, 24.0f, false) || std::fabs(Temperature.thermostat(State->Heater) - 24.0f) > 0.01f)
		{
			Fail(FString::Printf(TEXT("trade_thermostat: the target after setting 24 is %.1f"), Temperature.thermostat(State->Heater)));
		}
		if (!P->SetThermostat(State->Heater, 500.0f, false) || std::fabs(Temperature.thermostat(State->Heater) - 50.0f) > 0.01f)
		{
			Fail(FString::Printf(TEXT("trade_thermostat: a target of 500 should be held to 50, got %.1f"), Temperature.thermostat(State->Heater)));
		}
		P->SetThermostat(State->Heater, 0.0f, true);
		if (Temperature.hasThermostat(State->Heater))
		{
			Fail(TEXT("trade_thermostat: Reset did not go back to the building's own limit"));
		}
		if (P->SetThermostat(State->Stove, 20.0f, false))
		{
			Fail(TEXT("trade_thermostat: a stove accepted a thermostat"));
		}
		P->SetThermostat(State->Heater, 18.0f, false);  // for the picture
	};
	Thermostat.Verify = [=, this]()
	{
		if (Panels()->GetStats().ThermostatPanelFrames == 0)
		{
			Fail(TEXT("trade_thermostat: the thermostat panel was never drawn"));
		}
		Notes.Add(FString::Printf(TEXT("trade_thermostat: heater target %.0f C, %d commands sent by the trade panel"), SimOf()->temperature().thermostat(State->Heater), Trade()->GetStats().CommandsSent));
	};
	Phases.Add(MoveTemp(Thermostat));
}
