#include "TradePanelSubsystem.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#include <imgui.h>

#include "OverseerSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/Trade.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace
{
	TAutoConsoleVariable<int32> GTradeShowUi(
		TEXT("Trade.UI"), 1, TEXT("1 shows the trade strip, trade, gift and factions windows, 0 hides them."), ECVF_Default);

	FAutoConsoleCommandWithWorld GTradeFactionsCommand(
		TEXT("Trade.Factions"), TEXT("Toggles the factions window (goodwill with every faction)."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (UTradePanelSubsystem* Trade = World ? World->GetSubsystem<UTradePanelSubsystem>() : nullptr)
			{
				Trade->SetFactionsOpen(!Trade->IsFactionsOpen());
			}
		}));

	void TpText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}

	void TpTextColored(const ImVec4& Color, const FString& Text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Color);
		TpText(Text);
		ImGui::PopStyleColor();
	}

	FString TpCapital(const std::string& Text)
	{
		FString Out = UTF8_TO_TCHAR(Text.c_str());
		if (Out.Len() > 0)
		{
			Out[0] = FChar::ToUpper(Out[0]);
		}
		return Out;
	}

	ImGuiWindowFlags TpFlags()
	{
		return ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	}

	/** A count with -10 -1 +1 +10 buttons round it. Returns the new value (held to 0..Max) and whether a button changed it. */
	bool TpStepper(int32& Value, int32 Max)
	{
		const int32 Before = Value;
		const auto Step = [&](const char* Label, int32 Delta)
		{
			if (ImGui::SmallButton(Label))
			{
				Value = FMath::Clamp(Value + Delta, 0, Max);
			}
			ImGui::SameLine();
		};
		Step("-10", -10);
		Step("-1", -1);
		TpText(FString::Printf(TEXT("%3d"), Value));
		ImGui::SameLine();
		Step("+1", 1);
		Step("+10", 10);
		ImGui::NewLine();
		return Value != Before;
	}

	const char* TpKindWord(uint8 Kind)
	{
		switch (Kind)
		{
			case 1: return "trade caravan";
			case 2: return "trade ship";
			default: return "visitors";
		}
	}

	ImVec4 TpRelationColor(uint8 Relation)
	{
		switch (Relation)
		{
			case 0: return ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
			case 2: return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
			default: return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
		}
	}

	const char* TpRelationWord(uint8 Relation)
	{
		switch (Relation)
		{
			case 0: return "hostile";
			case 2: return "ally";
			default: return "neutral";
		}
	}

	/** A thing worth money that lies on the map, with the label to show. */
	struct FTpItem
	{
		FString Name;
		FString Label;
		const colony::Def* Def = nullptr;
		int32 Have = 0;
	};
}

// ---- Setup -------------------------------------------------------------------------------------

bool UTradePanelSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UTradePanelSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UTradePanelSubsystem, STATGROUP_Tickables);
}

void UTradePanelSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<USimSubsystem>();
}

USimSubsystem* UTradePanelSubsystem::GetBridge() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<USimSubsystem>() : nullptr;
}

bool UTradePanelSubsystem::IsOverseerActive() const
{
	UWorld* World = GetWorld();
	const UOverseerSubsystem* Overseer = World ? World->GetSubsystem<UOverseerSubsystem>() : nullptr;
	return Overseer && Overseer->IsOverseerActive();
}

void UTradePanelSubsystem::ResetForNewSim()
{
	OpenVisit = 0;
	bGiftMode = false;
	Negotiator = colony::kNoEntity;
	ClearLines();
	Stats.CommandsSent = 0;
}

void UTradePanelSubsystem::Submit(colony::Command Command)
{
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge)
	{
		return;
	}
	Bridge->Submit(std::move(Command));
	++Stats.CommandsSent;
	// Commands are applied by the next step; while the game is paused, one step makes the change show at once.
	if (Bridge->GetSpeed() == 0)
	{
		Bridge->StepOnce();
	}
}

FString UTradePanelSubsystem::NameOf(colony::Entity Pawn) const
{
	const USimSubsystem* Bridge = GetBridge();
	const colony::Colonist* Colonist = Bridge && Bridge->GetSim() ? Bridge->GetSim()->colonistOf(Pawn) : nullptr;
	if (!Colonist)
	{
		return TEXT("Someone");
	}
	return UTF8_TO_TCHAR((Colonist->nickName.empty() ? Colonist->firstName : Colonist->nickName).c_str());
}

const colony::VisitView* UTradePanelSubsystem::FindVisit(uint32 Id) const
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || Id == 0)
	{
		return nullptr;
	}
	for (const colony::VisitView& Visit : Bridge->GetCurrentSnapshot().visits)
	{
		if (Visit.id == Id)
		{
			return &Visit;
		}
	}
	return nullptr;
}

void UTradePanelSubsystem::GetNegotiators(TArray<colony::Entity>& Out) const
{
	Out.Reset();
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		if (View.category == colony::ThingCategory::Pawn && View.faction == 1 && !View.dead && !View.downed && Bridge->GetSim()->colonistOf(View.id))
		{
			Out.Add(View.id);
		}
	}
}

colony::Entity UTradePanelSubsystem::GetNegotiator() const
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return colony::kNoEntity;
	}
	TArray<colony::Entity> Pawns;
	GetNegotiators(Pawns);
	if (Pawns.Contains(Negotiator))
	{
		return Negotiator;
	}
	colony::Entity Best = colony::kNoEntity;
	float BestImprovement = -1000.0f;
	for (const colony::Entity Pawn : Pawns)
	{
		const float Improvement = Bridge->GetSim()->trade().priceImprovement(Pawn);
		if (Improvement > BestImprovement)
		{
			BestImprovement = Improvement;
			Best = Pawn;
		}
	}
	return Best;
}

// ---- What a click does -------------------------------------------------------------------------

bool UTradePanelSubsystem::OpenTrade(uint32 Visit)
{
	const colony::VisitView* View = FindVisit(Visit);
	if (!View || View->phase != 0 || View->kind == 0)
	{
		return false;
	}
	if (OpenVisit != Visit)
	{
		ClearLines();
	}
	OpenVisit = Visit;
	bGiftMode = false;
	return true;
}

bool UTradePanelSubsystem::OpenGift(uint32 Visit)
{
	const colony::VisitView* View = FindVisit(Visit);
	if (!View || View->phase != 0)
	{
		return false;
	}
	if (OpenVisit != Visit)
	{
		ClearLines();
	}
	OpenVisit = Visit;
	bGiftMode = true;
	return true;
}

void UTradePanelSubsystem::Close()
{
	OpenVisit = 0;
	bGiftMode = false;
	ClearLines();
}

void UTradePanelSubsystem::ClearLines()
{
	Buy.Reset();
	Sell.Reset();
	Gift.Reset();
}

void UTradePanelSubsystem::SetBuy(const FString& Thing, int32 Count)
{
	const colony::VisitView* Visit = FindVisit(OpenVisit);
	int32 Stock = 0;
	if (Visit)
	{
		for (const colony::StockView& Line : Visit->stock)
		{
			if (UTF8_TO_TCHAR(Line.thing.c_str()) == Thing)
			{
				Stock = Line.count;
			}
		}
	}
	Count = FMath::Clamp(Count, 0, Stock);
	if (Count == 0)
	{
		Buy.Remove(Thing);
	}
	else
	{
		Buy.Add(Thing, Count);
	}
}

void UTradePanelSubsystem::SetSell(const FString& Thing, int32 Count)
{
	const USimSubsystem* Bridge = GetBridge();
	const int32 Have = Bridge && Bridge->GetSim() ? Bridge->GetSim()->trade().colonyHas(TCHAR_TO_UTF8(*Thing)) : 0;
	Count = FMath::Clamp(Count, 0, Have);
	if (Count == 0)
	{
		Sell.Remove(Thing);
	}
	else
	{
		Sell.Add(Thing, Count);
	}
}

void UTradePanelSubsystem::SetGift(const FString& Thing, int32 Count)
{
	const USimSubsystem* Bridge = GetBridge();
	int32 Have = 0;
	if (Bridge && Bridge->GetSim())
	{
		Have = Thing == TEXT("Silver") ? Bridge->GetSim()->trade().silverOnMap() : Bridge->GetSim()->trade().colonyHas(TCHAR_TO_UTF8(*Thing));
	}
	Count = FMath::Clamp(Count, 0, Have);
	if (Count == 0)
	{
		Gift.Remove(Thing);
	}
	else
	{
		Gift.Add(Thing, Count);
	}
}

int32 UTradePanelSubsystem::GetBuy(const FString& Thing) const
{
	const int32* Count = Buy.Find(Thing);
	return Count ? *Count : 0;
}

int32 UTradePanelSubsystem::GetSell(const FString& Thing) const
{
	const int32* Count = Sell.Find(Thing);
	return Count ? *Count : 0;
}

int32 UTradePanelSubsystem::GetGift(const FString& Thing) const
{
	const int32* Count = Gift.Find(Thing);
	return Count ? *Count : 0;
}

bool UTradePanelSubsystem::GetQuote(FTradePanelQuote& Out) const
{
	Out = FTradePanelQuote();
	const USimSubsystem* Bridge = GetBridge();
	const colony::VisitView* Visit = FindVisit(OpenVisit);
	if (!Bridge || !Bridge->GetSim() || !Visit || bGiftMode)
	{
		return false;
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	const colony::TradeSystem& Trade = Sim.trade();
	const float Improvement = Trade.priceImprovement(GetNegotiator());
	double BuyTotal = 0.0;
	double SellTotal = 0.0;
	for (const TPair<FString, int32>& Line : Buy)
	{
		if (const colony::Def* Def = Sim.defs().find("Things", TCHAR_TO_UTF8(*Line.Key)))
		{
			BuyTotal += static_cast<double>(Trade.unitPrice(*Def, true, Improvement)) * Line.Value;
			Out.bAnything = true;
		}
	}
	for (const TPair<FString, int32>& Line : Sell)
	{
		if (const colony::Def* Def = Sim.defs().find("Things", TCHAR_TO_UTF8(*Line.Key)))
		{
			SellTotal += static_cast<double>(Trade.unitPrice(*Def, false, Improvement)) * Line.Value;
			Out.bAnything = true;
		}
	}
	Out.BuyCost = static_cast<int32>(std::ceil(BuyTotal - 1e-6));
	Out.SellGain = static_cast<int32>(std::floor(SellTotal + 1e-6));
	Out.Net = Out.BuyCost - Out.SellGain;
	if (!Out.bAnything)
	{
		Out.Problem = TEXT("Nothing is chosen yet.");
	}
	else if (GetNegotiator() == colony::kNoEntity)
	{
		Out.Problem = TEXT("No colonist is free to haggle.");
	}
	if (Out.Net > 0 && Trade.silverOnMap() < Out.Net)
	{
		Out.bAffordable = false;
		if (Out.Problem.IsEmpty())
		{
			Out.Problem = FString::Printf(TEXT("The colony is %d silver short."), Out.Net - Trade.silverOnMap());
		}
	}
	if (Out.Net < 0 && Visit->silver < -Out.Net)
	{
		Out.bTraderCanPay = false;
	}
	return true;
}

bool UTradePanelSubsystem::MakeTrade()
{
	FTradePanelQuote Quote;
	if (!GetQuote(Quote) || !Quote.bAnything)
	{
		return false;
	}
	colony::TradeCommand Command;
	Command.visit = OpenVisit;
	Command.negotiator = GetNegotiator();
	for (const TPair<FString, int32>& Line : Buy)
	{
		Command.buy.push_back(colony::TradeLine{std::string(TCHAR_TO_UTF8(*Line.Key)), Line.Value});
	}
	for (const TPair<FString, int32>& Line : Sell)
	{
		Command.sell.push_back(colony::TradeLine{std::string(TCHAR_TO_UTF8(*Line.Key)), Line.Value});
	}
	Submit(std::move(Command));
	ClearLines();
	return true;
}

float UTradePanelSubsystem::GiftGoodwill() const
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return 0.0f;
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	double Value = 0.0;
	for (const TPair<FString, int32>& Line : Gift)
	{
		if (const colony::Def* Def = Sim.defs().find("Things", TCHAR_TO_UTF8(*Line.Key)))
		{
			Value += Def->number("stats.marketValue", Line.Key == TEXT("Silver") ? 1.0 : 0.0) * Line.Value;
		}
	}
	return static_cast<float>(Value / Sim.trade().rules().giftSilverPerPoint);
}

bool UTradePanelSubsystem::MakeGift()
{
	if (OpenVisit == 0 || !bGiftMode || Gift.IsEmpty())
	{
		return false;
	}
	colony::GiftCommand Command;
	Command.visit = OpenVisit;
	for (const TPair<FString, int32>& Line : Gift)
	{
		Command.items.push_back(colony::TradeLine{std::string(TCHAR_TO_UTF8(*Line.Key)), Line.Value});
	}
	Submit(std::move(Command));
	Gift.Reset();
	return true;
}

// ---- The tick and the windows ------------------------------------------------------------------

void UTradePanelSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	if (ContentGeneration != Bridge->GetSimGeneration())
	{
		ContentGeneration = Bridge->GetSimGeneration();
		ResetForNewSim();
	}
	// The visit left (or was never real): close its window.
	if (OpenVisit != 0)
	{
		const colony::VisitView* Visit = FindVisit(OpenVisit);
		if (!Visit || Visit->phase != 0)
		{
			Close();
		}
	}
	if (!IsOverseerActive() || GTradeShowUi.GetValueOnGameThread() == 0)
	{
		return;
	}
	IConsoleVariable* ShowUi = IConsoleManager::Get().FindConsoleVariable(TEXT("Overseer.UI"));
	if (ShowUi && ShowUi->GetInt() == 0)
	{
		return;
	}
	DrawAll(*Bridge);
}

void UTradePanelSubsystem::DrawAll(USimSubsystem& Bridge)
{
	const ImGui::FScopedContext ScopedContext;
	if (!ScopedContext)
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	if (Viewport->Size.x <= 0.0f || Viewport->Size.y <= 0.0f)
	{
		return;
	}
	const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
	ImGui::GetStyle().FontScaleMain = 1.2f * Scale;
	const float Margin = 12.0f * Scale;

	DrawStrip(Bridge, Scale, Margin);
	if (const colony::VisitView* Visit = FindVisit(OpenVisit))
	{
		if (bGiftMode)
		{
			DrawGiftWindow(Bridge, *Visit, Scale);
		}
		else
		{
			DrawTradeWindow(Bridge, *Visit, Scale);
		}
	}
	if (bFactionsOpen)
	{
		DrawFactionsWindow(Bridge, Scale);
	}
}

void UTradePanelSubsystem::DrawStrip(USimSubsystem& Bridge, float Scale, float Margin)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	const colony::Sim& Sim = *Bridge.GetSim();
	// Under the clock at the top, in the middle.
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Margin + 100.0f * Scale), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.6f);
	if (ImGui::Begin("##TradeStrip", nullptr, TpFlags()))
	{
		++Stats.StripFrames;
		if (ImGui::SmallButton(bFactionsOpen ? "Factions (hide)" : "Factions"))
		{
			bFactionsOpen = !bFactionsOpen;
		}
		for (const colony::VisitView& Visit : Snapshot.visits)
		{
			const colony::Def* Faction = Sim.defs().find("Factions", Visit.faction);
			const FString FactionName = Faction ? TpCapital(Faction->label()) : UTF8_TO_TCHAR(Visit.faction.c_str());
			const double HoursLeft = FMath::Max(0.0, static_cast<double>(Visit.leaveAt - Bridge.GetGameSeconds()) / colony::kSecondsPerHour);
			const FString When = Visit.phase == 0 ? FString::Printf(TEXT("leaves in %.0f h"), HoursLeft) : FString(TEXT("leaving"));
			ImGui::PushID(static_cast<int>(Visit.id));
			TpText(FString::Printf(TEXT("%s %s (%s)"), *FactionName, UTF8_TO_TCHAR(TpKindWord(Visit.kind)), *When));
			if (Visit.phase == 0)
			{
				if (Visit.kind != 0)
				{
					ImGui::SameLine();
					if (ImGui::SmallButton("Trade"))
					{
						OpenTrade(Visit.id);
					}
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Gift"))
				{
					OpenGift(Visit.id);
				}
			}
			ImGui::PopID();
		}
	}
	ImGui::End();
}

void UTradePanelSubsystem::DrawTradeWindow(USimSubsystem& Bridge, const colony::VisitView& Visit, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::TradeSystem& Trade = Sim.trade();
	const colony::Visit* SimVisit = Trade.find(Visit.id);
	const colony::Def* Faction = Sim.defs().find("Factions", Visit.faction);
	const colony::Def* TraderKind = Sim.defs().find("TraderKinds", Visit.traderKind);

	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(760.0f * Scale, 0.0f), ImVec2(Viewport->Size.x * 0.85f, Viewport->Size.y * 0.8f));
	if (ImGui::Begin("##TradeWindow", nullptr, TpFlags() & ~ImGuiWindowFlags_NoScrollbar))
	{
		++Stats.TradeFrames;
		const float Goodwill = Trade.goodwill(Visit.faction);
		TpText(FString::Printf(TEXT("Trade with the %s of %s"), TraderKind ? UTF8_TO_TCHAR(TraderKind->label().c_str()) : TEXT("trader"),
			Faction ? UTF8_TO_TCHAR(Faction->label().c_str()) : UTF8_TO_TCHAR(Visit.faction.c_str())));
		ImGui::SameLine();
		TpTextColored(TpRelationColor(static_cast<uint8>(Trade.relation(Visit.faction))), FString::Printf(TEXT("  goodwill %.0f"), Goodwill));
		ImGui::SameLine();
		if (ImGui::SmallButton("Close"))
		{
			Close();
			ImGui::End();
			return;
		}

		// Who haggles.
		TArray<colony::Entity> Pawns;
		GetNegotiators(Pawns);
		const colony::Entity Chosen = GetNegotiator();
		const float Improvement = Trade.priceImprovement(Chosen);
		TpText(TEXT("Negotiator"));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(200.0f * Scale);
		if (ImGui::BeginCombo("##Negotiator", TCHAR_TO_UTF8(*NameOf(Chosen))))
		{
			for (const colony::Entity Pawn : Pawns)
			{
				if (ImGui::Selectable(TCHAR_TO_UTF8(*NameOf(Pawn)), Pawn == Chosen))
				{
					Negotiator = Pawn;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		TpText(FString::Printf(TEXT("Prices %s%.1f%% for the colony"), Improvement >= 0.0f ? TEXT("better by ") : TEXT("worse by "), FMath::Abs(Improvement) * 100.0f));
		TpText(FString::Printf(TEXT("Colony silver on the map: %d     The trader has %d silver"), Trade.silverOnMap(), Visit.silver));
		ImGui::Separator();

		if (ImGui::BeginTable("##TradeColumns", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame))
		{
			ImGui::TableNextColumn();
			TpText(TEXT("They sell"));
			if (Visit.stock.empty())
			{
				TpTextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), TEXT("Nothing."));
			}
			for (const colony::StockView& Line : Visit.stock)
			{
				const colony::Def* Def = Sim.defs().find("Things", Line.thing);
				if (!Def || Line.count <= 0)
				{
					continue;
				}
				const FString Name = UTF8_TO_TCHAR(Line.thing.c_str());
				int32 Count = GetBuy(Name);
				ImGui::PushID(TCHAR_TO_UTF8(*Name));
				TpText(FString::Printf(TEXT("%s  (%d)   %.0f each"), *TpCapital(Def->label()), Line.count, Trade.unitPrice(*Def, true, Improvement)));
				if (TpStepper(Count, Line.count))
				{
					SetBuy(Name, Count);
				}
				ImGui::PopID();
			}

			ImGui::TableNextColumn();
			TpText(TEXT("You sell"));
			// What lies on the map that this trader takes.
			TArray<FTpItem> Items;
			TSet<FString> Seen;
			for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
			{
				if (View.category != colony::ThingCategory::Item)
				{
					continue;
				}
				const colony::Def& Def = Sim.defs().get(View.def);
				const FString Name = UTF8_TO_TCHAR(Def.defName().c_str());
				if (Seen.Contains(Name) || Name == TEXT("Silver") || Def.number("stats.marketValue", 0.0) <= 0.0)
				{
					continue;
				}
				Seen.Add(Name);
				if (SimVisit && !Trade.accepts(*SimVisit, Def))
				{
					continue;
				}
				FTpItem Item;
				Item.Name = Name;
				Item.Label = TpCapital(Def.label());
				Item.Def = &Def;
				Item.Have = Trade.colonyHas(Def.defName());
				if (Item.Have > 0)
				{
					Items.Add(MoveTemp(Item));
				}
			}
			Items.Sort([](const FTpItem& A, const FTpItem& B) { return A.Label < B.Label; });
			if (Items.IsEmpty())
			{
				TpTextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), TEXT("Nothing this trader wants."));
			}
			for (const FTpItem& Item : Items)
			{
				int32 Count = GetSell(Item.Name);
				ImGui::PushID(TCHAR_TO_UTF8(*Item.Name));
				TpText(FString::Printf(TEXT("%s  (%d)   %.0f each"), *Item.Label, Item.Have, Trade.unitPrice(*Item.Def, false, Improvement)));
				if (TpStepper(Count, Item.Have))
				{
					SetSell(Item.Name, Count);
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		ImGui::Separator();
		FTradePanelQuote Quote;
		GetQuote(Quote);
		TpText(FString::Printf(TEXT("You pay %d silver     You get %d silver"), Quote.BuyCost, Quote.SellGain));
		if (Quote.Net > 0)
		{
			TpTextColored(Quote.bAffordable ? ImVec4(0.95f, 0.85f, 0.35f, 1.0f) : ImVec4(0.95f, 0.35f, 0.30f, 1.0f), FString::Printf(TEXT("The colony pays %d silver."), Quote.Net));
		}
		else if (Quote.Net < 0)
		{
			TpTextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), FString::Printf(TEXT("The trader pays %d silver."), -Quote.Net));
			if (!Quote.bTraderCanPay)
			{
				TpTextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f), FString::Printf(TEXT("It has only %d silver and takes the rest of the goods for nothing."), Visit.silver));
			}
		}
		if (!Quote.Problem.IsEmpty())
		{
			TpTextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), Quote.Problem);
		}
		const bool bCanTrade = Quote.bAnything && Quote.bAffordable && Chosen != colony::kNoEntity;
		ImGui::BeginDisabled(!bCanTrade);
		if (ImGui::Button("Make the trade"))
		{
			MakeTrade();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Clear"))
		{
			ClearLines();
		}
	}
	ImGui::End();
}

void UTradePanelSubsystem::DrawGiftWindow(USimSubsystem& Bridge, const colony::VisitView& Visit, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::TradeSystem& Trade = Sim.trade();
	const colony::Def* Faction = Sim.defs().find("Factions", Visit.faction);

	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f * Scale, 0.0f), ImVec2(Viewport->Size.x * 0.6f, Viewport->Size.y * 0.8f));
	if (ImGui::Begin("##GiftWindow", nullptr, TpFlags() & ~ImGuiWindowFlags_NoScrollbar))
	{
		++Stats.GiftFrames;
		TpText(FString::Printf(TEXT("Gift to %s"), Faction ? UTF8_TO_TCHAR(Faction->label().c_str()) : UTF8_TO_TCHAR(Visit.faction.c_str())));
		ImGui::SameLine();
		TpTextColored(TpRelationColor(static_cast<uint8>(Trade.relation(Visit.faction))), FString::Printf(TEXT("  goodwill %.0f"), Trade.goodwill(Visit.faction)));
		ImGui::SameLine();
		if (ImGui::SmallButton("Close"))
		{
			Close();
			ImGui::End();
			return;
		}
		TpTextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), FString::Printf(TEXT("Every %.0f silver of value earns 1 goodwill."), Trade.rules().giftSilverPerPoint));
		ImGui::Separator();

		TArray<FTpItem> Items;
		TSet<FString> Seen;
		if (Trade.silverOnMap() > 0)
		{
			FTpItem Silver;
			Silver.Name = TEXT("Silver");
			Silver.Label = TEXT("Silver");
			Silver.Have = Trade.silverOnMap();
			Items.Add(MoveTemp(Silver));
			Seen.Add(TEXT("Silver"));
		}
		for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Item)
			{
				continue;
			}
			const colony::Def& Def = Sim.defs().get(View.def);
			const FString Name = UTF8_TO_TCHAR(Def.defName().c_str());
			if (Seen.Contains(Name) || Def.number("stats.marketValue", 0.0) <= 0.0)
			{
				continue;
			}
			Seen.Add(Name);
			FTpItem Item;
			Item.Name = Name;
			Item.Label = TpCapital(Def.label());
			Item.Have = Trade.colonyHas(Def.defName());
			if (Item.Have > 0)
			{
				Items.Add(MoveTemp(Item));
			}
		}
		Items.Sort([](const FTpItem& A, const FTpItem& B) { return A.Label < B.Label; });
		if (Items.IsEmpty())
		{
			TpTextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), TEXT("Nothing lies on the map worth giving."));
		}
		ImGui::BeginChild("##GiftItems", ImVec2(0.0f, 320.0f * Scale), false);
		for (const FTpItem& Item : Items)
		{
			int32 Count = GetGift(Item.Name);
			ImGui::PushID(TCHAR_TO_UTF8(*Item.Name));
			TpText(FString::Printf(TEXT("%s  (%d)"), *Item.Label, Item.Have));
			if (TpStepper(Count, Item.Have))
			{
				SetGift(Item.Name, Count);
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
		ImGui::Separator();
		TpText(FString::Printf(TEXT("This gift earns %.1f goodwill."), GiftGoodwill()));
		ImGui::BeginDisabled(Gift.IsEmpty());
		if (ImGui::Button("Give the gift"))
		{
			MakeGift();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Clear"))
		{
			Gift.Reset();
		}
	}
	ImGui::End();
}

void UTradePanelSubsystem::DrawFactionsWindow(USimSubsystem& Bridge, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const colony::Sim& Sim = *Bridge.GetSim();
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.30f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f * Scale, 0.0f), ImVec2(Viewport->Size.x * 0.5f, Viewport->Size.y * 0.5f));
	if (ImGui::Begin("##FactionsWindow", nullptr, TpFlags() & ~ImGuiWindowFlags_NoScrollbar))
	{
		++Stats.FactionFrames;
		TpText(TEXT("Factions"));
		ImGui::SameLine();
		if (ImGui::SmallButton("Close"))
		{
			bFactionsOpen = false;
		}
		ImGui::Separator();
		for (const colony::GoodwillView& Line : Bridge.GetCurrentSnapshot().goodwill)
		{
			const colony::Def* Faction = Sim.defs().find("Factions", Line.faction);
			const FString Name = Faction ? TpCapital(Faction->label()) : UTF8_TO_TCHAR(Line.faction.c_str());
			TpTextColored(TpRelationColor(Line.relation), FString::Printf(TEXT("%-26s %5.0f   %s"), *Name, Line.goodwill, UTF8_TO_TCHAR(TpRelationWord(Line.relation))));
		}
	}
	ImGui::End();
}
