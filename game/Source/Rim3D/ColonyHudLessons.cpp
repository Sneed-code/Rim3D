// The learning helper (docs/design/ui-layout.md, Learning helper): short lessons in plain words, each shown when it matters
// (the first time the colony is hungry, strangers attack, a trader is here...), in a box at the top right. "Got it" marks one
// learned for good (saved with the HUD's settings); the book on the bottom bar hides or shows the helper.
#include "ColonyHudSubsystem.h"

#include <imgui.h>

#include "ColonyHudStyle.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

namespace
{
	/** What the lessons look at, gathered once a frame from the snapshot. */
	struct FHudLessonContext
	{
		bool bHungry = false;      // a colonist's food under 30%
		bool bTired = false;       // a colonist's rest under 30%
		bool bDown = false;        // a colonist is down
		bool bStrangers = false;   // armed strangers (not visitors, prisoners or wild animals) on the map
		bool bVisitors = false;    // a visit is here
		bool bPrisoner = false;
		bool bTameAnimal = false;
		bool bCold = false;        // outdoors under 0 C
		bool bHot = false;         // outdoors over 35 C
		int64 Day = 0;
		bool bHasStockpile = false;
	};

	struct FHudLesson
	{
		const TCHAR* Id;
		const TCHAR* Title;
		const TCHAR* Text;
		bool (*When)(const FHudLessonContext&);
	};

	bool HudLessonAlways(const FHudLessonContext&) { return true; }

	// In the order a new player meets them; the first unlearned one whose moment has come shows.
	const FHudLesson HudLessons[] = {
		{TEXT("camera"), TEXT("Looking around"),
			TEXT("WASD moves the camera, Q and E turn it, the mouse wheel zooms and the middle button drag orbits. Home puts it back."),
			HudLessonAlways},
		{TEXT("select"), TEXT("Your colonists"),
			TEXT("Click a colonist (or a face at the top left) to see what they are doing, their needs and skills. F centres the camera on them."),
			HudLessonAlways},
		{TEXT("stockpile"), TEXT("A stockpile"),
			TEXT("Build > Zone > Stockpile, then drag on the ground. Colonists carry food, materials and loot there, out of the rain."),
			[](const FHudLessonContext& C) { return !C.bHasStockpile; }},
		{TEXT("build"), TEXT("Building"),
			TEXT("Build opens the menu. Pick a category, then a tile, then drag on the map. Colonists with Construction come and build it from the stockpile's materials."),
			HudLessonAlways},
		{TEXT("work"), TEXT("Who does what"),
			TEXT("The Work tab sets each colonist's jobs and their order. Low numbers come first. Someone has to cook, haul and grow."),
			HudLessonAlways},
		{TEXT("food"), TEXT("Hungry colonists"),
			TEXT("Someone is getting hungry. Plant a growing zone (Build > Zone), hunt animals (Wildlife tab) and build a stove so a cook makes meals."),
			[](const FHudLessonContext& C) { return C.bHungry; }},
		{TEXT("sleep"), TEXT("Beds"),
			TEXT("Tired colonists sleep on the ground and hate it. Build beds (Furniture) inside walls with a roof; the Schedule tab sets when they sleep."),
			[](const FHudLessonContext& C) { return C.bTired; }},
		{TEXT("draft"), TEXT("Strangers attack"),
			TEXT("Select colonists and press R to draft them, then right click to move or attack. Cover (walls, sandbags) stops bullets. Alarm drafts everyone."),
			[](const FHudLessonContext& C) { return C.bStrangers; }},
		{TEXT("downed"), TEXT("Someone is down"),
			TEXT("A colonist who is down bleeds. Others carry them to a bed and a doctor tends them; the Medical window (Assign tab) sets the care and medicine."),
			[](const FHudLessonContext& C) { return C.bDown; }},
		{TEXT("takeover"), TEXT("Playing as a colonist"),
			TEXT("Select a colonist and press Y to take over and play them yourself, in third person. O goes back to the overseer view."),
			[](const FHudLessonContext& C) { return C.Day >= 1; }},
		{TEXT("research"), TEXT("Research"),
			TEXT("Build a research bench (Production), then pick a project in the Research tab. Projects unlock new buildings, weapons and crops."),
			[](const FHudLessonContext& C) { return C.Day >= 1; }},
		{TEXT("trade"), TEXT("Traders and visitors"),
			TEXT("A visit is here. Traders buy and sell for silver: the trade strip at the top lists them. Gifts raise a faction's goodwill (Factions)."),
			[](const FHudLessonContext& C) { return C.bVisitors; }},
		{TEXT("prisoners"), TEXT("Prisoners"),
			TEXT("Mark a bed for prisoners, and wardens carry downed enemies to it. The Prisoners window (Assign tab) says whether to recruit, enslave or release them."),
			[](const FHudLessonContext& C) { return C.bPrisoner; }},
		{TEXT("animals"), TEXT("Tame animals"),
			TEXT("The Animals tab names your animals, sets their training and the zone they stay in. A fence ring with a gate makes a pen."),
			[](const FHudLessonContext& C) { return C.bTameAnimal; }},
		{TEXT("cold"), TEXT("Cold"),
			TEXT("It is freezing outside. Build walls and a roof, a heater or a campfire inside, and give colonists warm clothes (Outfits)."),
			[](const FHudLessonContext& C) { return C.bCold; }},
		{TEXT("heat"), TEXT("Heat"),
			TEXT("It is very hot outside. A roof and a cooler in the wall keep a room cool; colonists can get heatstroke out in the sun."),
			[](const FHudLessonContext& C) { return C.bHot; }},
	};
}

int32 UColonyHudSubsystem::CountLessons()
{
	return UE_ARRAY_COUNT(HudLessons);
}

void UColonyHudSubsystem::DrawLessons(USimSubsystem& Bridge, float Scale)
{
	if (!bLessonsOn)
	{
		return;
	}
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	FHudLessonContext Context;
	for (const colony::ThingView& View : Snapshot.things)
	{
		if (View.category != colony::ThingCategory::Pawn || View.dead)
		{
			continue;
		}
		const bool bAnimal = View.lifeStage >= 0;
		if (View.faction == 1 && !bAnimal && Sim.colonistOf(View.id))
		{
			Context.bHungry = Context.bHungry || (View.food >= 0.0f && View.food < 0.3f);
			Context.bTired = Context.bTired || (View.rest >= 0.0f && View.rest < 0.3f);
			Context.bDown = Context.bDown || View.downed;
		}
		Context.bTameAnimal = Context.bTameAnimal || (bAnimal && View.faction == 1);
		Context.bPrisoner = Context.bPrisoner || View.prisoner;
		Context.bStrangers = Context.bStrangers || (!bAnimal && View.faction != 1 && View.faction != 100 && !View.visitor && !View.prisoner && !View.downed);
	}
	Context.bVisitors = !Snapshot.visits.empty();
	Context.bCold = Snapshot.outdoorTemperature < 0.0f;
	Context.bHot = Snapshot.outdoorTemperature > 35.0f;
	Context.Day = Bridge.GetGameSeconds() / colony::kSecondsPerDay;
	Context.bHasStockpile = Sim.stockpileCell().has_value();

	const FHudLesson* Showing = nullptr;
	int32 Learned = 0;
	for (const FHudLesson& Lesson : HudLessons)
	{
		if (LearnedLessons.Contains(Lesson.Id))
		{
			++Learned;
		}
		else if (!Showing && Lesson.When(Context))
		{
			Showing = &Lesson;
		}
	}
	if (!Showing)
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float Width = 380.0f * Scale;
	// Top right, under the alarm button and the event line.
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - 12.0f * Scale, Viewport->Pos.y + 110.0f * Scale), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(Width, 0.0f), ImVec2(Width, Viewport->Size.y * 0.4f));
	ColonyHud::HoldWidth("##HudLesson", Scale);
	if (ImGui::Begin("##HudLesson", nullptr, ColonyHud::BoxFlags()))
	{
		++Stats.LessonFrames;
		ColonyHud::TextColored(ColonyHud::TextFaint(), FString::Printf(TEXT("Learning helper  %d / %d"), Learned, CountLessons()));
		ColonyHud::Heading(Showing->Title);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Width - 2.0f * ImGui::GetStyle().WindowPadding.x);
		ColonyHud::Text(Showing->Text);
		ImGui::PopTextWrapPos();
		if (ColonyHud::Button(TEXT("Got it")))
		{
			LearnedLessons.Add(Showing->Id);
			SavePrefs();
		}
		ImGui::SameLine();
		if (ColonyHud::Button(TEXT("Hide the helper")))
		{
			bLessonsOn = false;
			SavePrefs();
		}
		ColonyHud::Tip(TEXT("The book on the bottom bar shows it again"));
	}
	ImGui::End();
}
