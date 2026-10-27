#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Snapshot.h"

#include "TradePanelSubsystem.generated.h"

class USimSubsystem;

/** What a deal comes to at the negotiator's prices (see FTradePanelQuote). */
struct FTradePanelQuote
{
	/** Silver the colony pays for what it buys, rounded up. */
	int32 BuyCost = 0;
	/** Silver the trader pays for what the colony sells, rounded down. */
	int32 SellGain = 0;
	/** BuyCost - SellGain: positive means the colony pays, negative the trader does. */
	int32 Net = 0;
	/** The colony has the silver it owes (always true when the trader owes). */
	bool bAffordable = true;
	/** The trader can pay what it owes (else it takes the rest of the goods for nothing). */
	bool bTraderCanPay = true;
	/** Something is chosen to buy or sell. */
	bool bAnything = false;
	/** Why the deal can't be made, in plain words; empty when it can. */
	FString Problem;
};

/** What the trade panel drew and did (smoke test `trade`). */
struct FTradePanelStats
{
	int32 StripFrames = 0;
	int32 TradeFrames = 0;
	int32 GiftFrames = 0;
	int32 FactionFrames = 0;
	/** Commands the panel sent since the sim started. */
	int32 CommandsSent = 0;
};

/**
 * The trade window, gifts and the factions list (task U19, docs/design/panels.md "Trade"), all ImGui, all through
 * USimSubsystem:
 *
 * - A strip under the clock lists the visits that are here (visitors, a caravan, a trade ship) with a Trade and a
 *   Gift button each, and a Factions button.
 * - The trade window: what the trader sells on the left, what the colony can sell on the right (only what the
 *   trader takes), each with its price at the chosen negotiator's Social skill, a count stepper per line, and the
 *   total with the silver on both sides. "Make the trade" sends TradeCommand; the sim refuses a deal the colony
 *   can't pay for and the panels' message log says why.
 * - The gift window: pick anything lying on the map; the goodwill it will earn is shown. Sends GiftCommand.
 * - The factions window: every faction's goodwill and relation.
 *
 * It only draws while the overseer view is the one in use. Console: Trade.UI 0|1, Trade.Factions.
 */
UCLASS()
class RIM3D_API UTradePanelSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Opens the trade window (or the gift window) on a visit that is here. False if there is no such visit, or a visit with nobody to trade with. */
	bool OpenTrade(uint32 Visit);
	bool OpenGift(uint32 Visit);
	void Close();
	uint32 GetOpenVisit() const { return OpenVisit; }
	bool IsGiftMode() const { return bGiftMode; }
	bool IsFactionsOpen() const { return bFactionsOpen; }
	void SetFactionsOpen(bool bOpen) { bFactionsOpen = bOpen; }

	// ---- What a click does (the panel calls these; the smoke test calls them too) ---------------
	/** Sets how many of a thing (a Things defName) the colony buys from the trader. The count is held to what the trader has. */
	void SetBuy(const FString& Thing, int32 Count);
	/** Sets how many the colony sells; held to what lies on the map. */
	void SetSell(const FString& Thing, int32 Count);
	/** Sets how many of a thing the colony gives as a gift (gift window); held to what lies on the map. */
	void SetGift(const FString& Thing, int32 Count);
	int32 GetBuy(const FString& Thing) const;
	int32 GetSell(const FString& Thing) const;
	int32 GetGift(const FString& Thing) const;
	/** Forgets all chosen counts. */
	void ClearLines();
	void SetNegotiator(colony::Entity Pawn) { Negotiator = Pawn; }
	/** The colonist whose Social skill sets the prices: the one picked, else the best haggler who can act. */
	colony::Entity GetNegotiator() const;
	/** What the deal on screen comes to. False if no trade window is open. */
	bool GetQuote(FTradePanelQuote& Out) const;
	/** Sends the TradeCommand for the chosen lines, then forgets them. False if there is nothing to send. */
	bool MakeTrade();
	/** Sends the GiftCommand for the chosen gift lines, then forgets them. False if there is nothing to send. */
	bool MakeGift();
	/** Goodwill the chosen gift would earn. */
	float GiftGoodwill() const;

	const FTradePanelStats& GetStats() const { return Stats; }
	/** The colonists who can act, for the negotiator list. */
	void GetNegotiators(TArray<colony::Entity>& Out) const;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	USimSubsystem* GetBridge() const;
	bool IsOverseerActive() const;
	void Submit(colony::Command Command);
	FString NameOf(colony::Entity Pawn) const;
	const colony::VisitView* FindVisit(uint32 Id) const;
	void ResetForNewSim();

	void DrawAll(USimSubsystem& Bridge);
	void DrawStrip(USimSubsystem& Bridge, float Scale, float Margin);
	void DrawTradeWindow(USimSubsystem& Bridge, const colony::VisitView& Visit, float Scale);
	void DrawGiftWindow(USimSubsystem& Bridge, const colony::VisitView& Visit, float Scale);
	void DrawFactionsWindow(USimSubsystem& Bridge, float Scale);

	uint32 OpenVisit = 0;
	bool bGiftMode = false;
	bool bFactionsOpen = false;
	colony::Entity Negotiator = colony::kNoEntity;
	TMap<FString, int32> Buy;
	TMap<FString, int32> Sell;
	TMap<FString, int32> Gift;
	int32 ContentGeneration = -1;

	FTradePanelStats Stats;
};
