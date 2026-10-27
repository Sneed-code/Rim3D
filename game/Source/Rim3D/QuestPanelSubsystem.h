#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Events.h"
#include "ColonySim/Snapshot.h"

#include "QuestPanelSubsystem.generated.h"

class USimSubsystem;

/** What the quest panel drew and did (smoke test `quests`). */
struct FQuestPanelStats
{
	int32 WindowFrames = 0;
	/** Quest rows drawn in the last frame the window was open, by section. */
	int32 OfferedRows = 0;
	int32 ActiveRows = 0;
	int32 FinishedRows = 0;
	/** Commands the panel sent since the sim started. */
	int32 CommandsSent = 0;
};

/**
 * The quest window (docs/design/panels.md "Quests", sim side docs/design/quests.md), ImGui, all through USimSubsystem:
 *
 * - A Quests button in the panels' menu row (UPanelsSubsystem draws it) shows how many offers wait, amber while one has
 *   not been seen yet.
 * - The window lists the offers waiting for an answer (what is asked, the def's text, who comes, the raid's size, the
 *   rewards and how long the offer stays open) with Accept and Decline (QuestCommand), the quests under way (when the
 *   raid comes, how long is left), and the finished ones, newest first, with how they ended.
 * - The panels' message log says when a quest is offered, answered and ended (UPanelsSubsystem::OnSimEvent).
 *
 * It only draws while the overseer view is the one in use. Console: Quests.UI 0|1, Quests.Open.
 */
UCLASS()
class RIM3D_API UQuestPanelSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	bool IsOpen() const { return bOpen; }
	/** Opening the window marks every offer as seen. */
	void SetOpen(bool bInOpen);

	// ---- What a click does (the window calls these; the smoke test calls them too) --------------
	/** Sends QuestCommand "accept" / "decline". False (nothing sent) if the quest is not an offer waiting for an answer. */
	bool Accept(uint32 Quest);
	bool Decline(uint32 Quest);

	/** Offers waiting for an answer / quests under way, from the snapshot. */
	int32 CountOffered() const;
	int32 CountActive() const;
	/** An offer came since the window was last open. */
	bool HasUnseenOffer() const { return bUnseenOffer; }
	/** How a finished quest ended, from its last event ("the raid is over", "the refugees died"); empty if not heard. */
	FString GetOutcome(uint32 Quest) const;
	/** The quest in the snapshot; nullptr if there is none. */
	const colony::QuestView* FindQuest(uint32 Quest) const;

	const FQuestPanelStats& GetStats() const { return Stats; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	USimSubsystem* GetBridge() const;
	bool IsOverseerActive() const;
	void Submit(colony::Command Command);
	void OnSimEvent(const colony::Event& Event);
	void SyncWithSim();
	void ResetForNewSim();

	void DrawWindow(USimSubsystem& Bridge);
	void DrawQuest(USimSubsystem& Bridge, const colony::QuestView& Quest, float Scale);

	bool bOpen = false;
	bool bUnseenOffer = false;
	TMap<uint32, FString> Outcomes;
	int32 ContentGeneration = -1;

	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> EventSource;

	FQuestPanelStats Stats;
};
