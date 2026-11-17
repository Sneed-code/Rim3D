#include "KillFeedSubsystem.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#include <imgui.h>

#include "PanelsSubsystem.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "SoundSubsystem.h"

#include "ColonySim/Sim.h"

namespace
{
	TAutoConsoleVariable<int32> GKillFeedEnable(
		TEXT("KillFeed.Enable"), 1, TEXT("1 shows the kill feed at the top left, 0 hides it."), ECVF_Default);

	FAutoConsoleCommandWithWorld GKillFeedTestCommand(
		TEXT("KillFeed.Test"), TEXT("Puts up sample kill feed lines: a kill of yours, an enemy's kill and a death by itself."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (UKillFeedSubsystem* Feed = World ? World->GetSubsystem<UKillFeedSubsystem>() : nullptr)
			{
				Feed->AddLine(TEXT("Ace"), 0, TEXT("Raider"), 1, true);
				Feed->AddLine(TEXT("Raider"), 1, TEXT("Bo"), 0, false);
				Feed->AddLine(FString(), 0, TEXT("Deer"), 2, false);
			}
		}));

	/** The band colour of a side: friendly blue, enemy red, neutral yellow. */
	void KillFeedSideColour(uint8 Side, int32& R, int32& G, int32& B)
	{
		switch (Side)
		{
			case 0: R = 40; G = 115; B = 235; break;
			case 1: R = 215; G = 40; B = 40; break;
			default: R = 225; G = 175; B = 25; break;
		}
	}

	/** One name on its band: the colour full on the left fading out to the right, white text with a shadow. Returns its width. */
	float KillFeedDrawName(ImDrawList* Draw, ImFont* Font, float Size, ImVec2 At, const FString& Name, uint8 Side, float Alpha, float Pad)
	{
		const FTCHARToUTF8 Utf8(*Name);
		const ImVec2 Text = Font->CalcTextSizeA(Size, FLT_MAX, 0.0f, Utf8.Get());
		const float W = Text.x + Pad * 4.0f;
		const float H = Text.y + Pad;
		int32 R = 0, G = 0, B = 0;
		KillFeedSideColour(Side, R, G, B);
		const ImU32 Left = IM_COL32(R, G, B, static_cast<int32>(215 * Alpha));
		const ImU32 Right = IM_COL32(R, G, B, static_cast<int32>(25 * Alpha));
		Draw->AddRectFilledMultiColor(At, ImVec2(At.x + W, At.y + H), Left, Right, Right, Left);
		const ImVec2 TextAt(At.x + Pad * 1.5f, At.y + Pad * 0.5f);
		Draw->AddText(Font, Size, ImVec2(TextAt.x + 1.5f, TextAt.y + 1.5f), IM_COL32(0, 0, 0, static_cast<int32>(190 * Alpha)), Utf8.Get());
		Draw->AddText(Font, Size, TextAt, IM_COL32(255, 255, 255, static_cast<int32>(255 * Alpha)), Utf8.Get());
		return W;
	}
}

bool UKillFeedSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UKillFeedSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UKillFeedSubsystem, STATGROUP_Tickables);
}

void UKillFeedSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		BridgeSource = Bridge;
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &UKillFeedSubsystem::OnSimEvent);
	}
	Collection.InitializeDependency<UProtagonistSubsystem>();
	Collection.InitializeDependency<USoundSubsystem>();
}

void UKillFeedSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = BridgeSource.Get())
	{
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	Super::Deinitialize();
}

void UKillFeedSubsystem::AddLine(const FString& Killer, uint8 KillerSide, const FString& Target, uint8 TargetSide, bool bMine)
{
	FKillFeedLine Line;
	Line.Killer = Killer;
	Line.KillerSide = KillerSide;
	Line.Target = Target;
	Line.TargetSide = TargetSide;
	Line.bMine = bMine;
	Lines.Add(MoveTemp(Line));
	// Too many at once: the oldest go first.
	while (Lines.Num() > MaxLines)
	{
		Lines.RemoveAt(0);
	}
	++LinesShown;
	if (bMine)
	{
		++PersonalKills;
		if (USoundSubsystem* Sound = GetWorld()->GetSubsystem<USoundSubsystem>())
		{
			Sound->PlayCue(TEXT("Ui_KillConfirm"), FVector::ZeroVector, true);
		}
	}
}

void UKillFeedSubsystem::OnSimEvent(const colony::Event& Event)
{
	if (Event.type != colony::EventType::PawnDied)
	{
		return;
	}
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const colony::Entity Mine = Protagonist ? Protagonist->GetControlled() : colony::kNoEntity;
	const bool bKiller = Event.target != colony::kNoEntity;
	AddLine(bKiller ? FString(UTF8_TO_TCHAR(Sim.feedName(Event.target).c_str())) : FString(),
		bKiller ? static_cast<uint8>(Sim.killSideOf(Event.target)) : 0,
		UTF8_TO_TCHAR(Sim.feedName(Event.thing).c_str()), static_cast<uint8>(Sim.killSideOf(Event.thing)),
		bKiller && Mine != colony::kNoEntity && Event.target == Mine);
}

void UKillFeedSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (USimSubsystem* Bridge = BridgeSource.Get(); Bridge && Bridge->GetSimGeneration() != LastGeneration)
	{
		LastGeneration = Bridge->GetSimGeneration();
		Lines.Reset();  // a new or loaded game starts with an empty feed
	}
	// Real time: the feed keeps fading while the game is paused or sped up.
	for (FKillFeedLine& Line : Lines)
	{
		Line.Age += DeltaTime;
	}
	Lines.RemoveAll([](const FKillFeedLine& Line) { return Line.Age >= LineSeconds; });
	if (GKillFeedEnable.GetValueOnGameThread() != 0 && !Lines.IsEmpty())
	{
		DrawFeed(DeltaTime);
	}
}

void UKillFeedSubsystem::DrawFeed(float DeltaTime)
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
	const float Margin = 14.0f * Scale;
	float Top = Margin;
	// In the overseer's view the menu and colonist bar hold the corner: start under them.
	if (const UPanelsSubsystem* Panels = GetWorld()->GetSubsystem<UPanelsSubsystem>())
	{
		const float Below = Panels->GetTopLeftBottom();
		Top = Below > 0.0f ? Below + 8.0f * Scale : Top;
	}
	ImDrawList* Draw = ImGui::GetForegroundDrawList();
	ImFont* Font = ImGui::GetFont();
	const float Size = 19.0f * Scale;
	const float Pad = 4.0f * Scale;
	const float Row = Size + Pad + 6.0f * Scale;
	const float Slide = 1.0f - FMath::Exp(-14.0f * DeltaTime);  // eases each line to its place
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		FKillFeedLine& Line = Lines[i];
		const float Want = i * Row;
		Line.Y = Line.Y < 0.0f ? Want : FMath::Lerp(Line.Y, Want, Slide);
		// Slides in from the left over the first 0.15 s, fades over the last FadeSeconds.
		const float In = FMath::Clamp(Line.Age / 0.15f, 0.0f, 1.0f);
		const float Alpha = FMath::Clamp((LineSeconds - Line.Age) / FadeSeconds, 0.0f, 1.0f) * In;
		ImVec2 At(Viewport->Pos.x + Margin - (1.0f - In) * 40.0f * Scale, Viewport->Pos.y + Top + Line.Y);
		const float Start = At.x;
		if (!Line.Killer.IsEmpty())
		{
			At.x += KillFeedDrawName(Draw, Font, Size, At, Line.Killer, Line.KillerSide, Alpha, Pad) + 6.0f * Scale;
			// A small blade-like arrow between killer and target.
			const float Mid = At.y + (Size + Pad) * 0.5f;
			const float Len = 18.0f * Scale;
			const ImU32 White = IM_COL32(245, 245, 245, static_cast<int32>(235 * Alpha));
			Draw->AddLine(ImVec2(At.x, Mid), ImVec2(At.x + Len - 6.0f * Scale, Mid), White, 2.5f * Scale);
			Draw->AddTriangleFilled(ImVec2(At.x + Len - 7.0f * Scale, Mid - 5.0f * Scale), ImVec2(At.x + Len, Mid),
				ImVec2(At.x + Len - 7.0f * Scale, Mid + 5.0f * Scale), White);
			At.x += Len + 6.0f * Scale;
			At.x += KillFeedDrawName(Draw, Font, Size, At, Line.Target, Line.TargetSide, Alpha, Pad);
		}
		else
		{
			At.x += KillFeedDrawName(Draw, Font, Size, At, Line.Target, Line.TargetSide, Alpha, Pad) + 6.0f * Scale;
			const FTCHARToUTF8 Word(TEXT("killed"));
			const ImVec2 WordAt(At.x, At.y + Pad * 0.5f);
			Draw->AddText(Font, Size, ImVec2(WordAt.x + 1.5f, WordAt.y + 1.5f), IM_COL32(0, 0, 0, static_cast<int32>(190 * Alpha)), Word.Get());
			Draw->AddText(Font, Size, WordAt, IM_COL32(235, 235, 235, static_cast<int32>(255 * Alpha)), Word.Get());
			At.x += Font->CalcTextSizeA(Size, FLT_MAX, 0.0f, Word.Get()).x;
		}
		if (Line.bMine)
		{
			// Your own kill: a thin white edge round the line, brightest when it comes up.
			const float Glow = 0.55f + 0.45f * FMath::Clamp(1.0f - Line.Age / 0.6f, 0.0f, 1.0f);
			Draw->AddRect(ImVec2(Start - 3.0f * Scale, At.y - 2.0f * Scale), ImVec2(At.x + 3.0f * Scale, At.y + Size + Pad + 2.0f * Scale),
				IM_COL32(255, 255, 255, static_cast<int32>(200 * Alpha * Glow)), 3.0f * Scale, 0, 1.5f * Scale);
		}
	}
	++FramesDrawn;
}
