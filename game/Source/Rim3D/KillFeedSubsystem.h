#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "KillFeedSubsystem.generated.h"

class USimSubsystem;

/** One line of the kill feed. Names and sides are copied when the death happens (the bodies may be gone later). */
struct FKillFeedLine
{
	FString Killer;  // empty: died of something, not someone ("Name killed")
	FString Target;
	uint8 KillerSide = 0;  // colony::Sim::KillSide: 0 friendly (blue), 1 enemy (red), 2 neutral (yellow)
	uint8 TargetSide = 0;
	bool bMine = false;  // the pawn the player controls made the kill
	float Age = 0.0f;    // real seconds since it came up
	float Y = -1.0f;     // where it is drawn now, px under the feed's top (slides up as lines above it go)
};

/**
 * The kill feed (sneed, 2026-11-17; docs/design/kill-feed.md): every death flashes up at the top left as
 * "Killer  Target", each name on a coloured band that fades to the right (the colony and its tame animals blue, enemies
 * red, wild animals, visitors and prisoners yellow). A death that was nobody's doing shows "Name killed". A line stays
 * 5 real seconds and fades out; more deaths stack under it and slide up as the ones above go. When the pawn the player
 * controls makes the kill, a short kill-confirm sound plays (cue Ui_KillConfirm) and the line gets a white edge.
 *
 * All of it reads the sim's PawnDied events (`target` is the killer, Sim::creditKills) and Sim::killSideOf/feedName.
 * Console: KillFeed.Enable (0 hides it), KillFeed.Test (puts up three sample lines).
 */
UCLASS()
class RIM3D_API UKillFeedSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Adds a line as if a death had come in (tests, KillFeed.Test). */
	void AddLine(const FString& Killer, uint8 KillerSide, const FString& Target, uint8 TargetSide, bool bMine);

	const TArray<FKillFeedLine>& GetLines() const { return Lines; }
	int32 GetLinesShown() const { return LinesShown; }
	int32 GetPersonalKills() const { return PersonalKills; }
	int32 GetFramesDrawn() const { return FramesDrawn; }

	static constexpr float LineSeconds = 5.0f;
	static constexpr float FadeSeconds = 0.7f;
	static constexpr int32 MaxLines = 6;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void OnSimEvent(const colony::Event& Event);
	void DrawFeed(float DeltaTime);

	TArray<FKillFeedLine> Lines;
	int32 LinesShown = 0;
	int32 PersonalKills = 0;
	int32 FramesDrawn = 0;
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> BridgeSource;
	int32 LastGeneration = -1;
};
