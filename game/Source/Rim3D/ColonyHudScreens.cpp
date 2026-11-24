// The manager windows the Assign tab opens (Outfits, Areas, Medical, Prisoners) and the main menu (new colony, load,
// settings, quit), docs/design/ui-layout.md "Screens". They read the sim and send its commands, like the tabs.

#include "ColonyHudSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include <imgui.h>

#include "ColonyHudStyle.h"
#include "HudIcons.h"

#include "OrdersSubsystem.h"
#include "OverseerSubsystem.h"
#include "SimPicking.h"
#include "SimSubsystem.h"
#include "WorldScreenSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Medicine.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Surgery.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace
{
	FString HudScrCapital(const std::string& Text)
	{
		FString Out = UTF8_TO_TCHAR(Text.c_str());
		if (Out.Len() > 0)
		{
			Out[0] = FChar::ToUpper(Out[0]);
		}
		return Out;
	}

	FString HudScrPawnName(const colony::Sim& Sim, const colony::ThingView& View)
	{
		if (const colony::Colonist* Colonist = Sim.colonistOf(View.id))
		{
			return UTF8_TO_TCHAR((Colonist->nickName.empty() ? Colonist->firstName : Colonist->nickName).c_str());
		}
		return HudScrCapital(Sim.defs().get(View.def).label());
	}

	int32 HudScrId(colony::Entity Id)
	{
		return static_cast<int32>(entt::to_integral(Id));
	}

	/** Selects a thing in the overseer view and centres on it. */
	void HudScrGoTo(UWorld* World, colony::Entity Thing)
	{
		UOverseerSubsystem* Overseer = World ? World->GetSubsystem<UOverseerSubsystem>() : nullptr;
		if (!Overseer)
		{
			return;
		}
		FSimPick Pick;
		Pick.Kind = ESimPickKind::Thing;
		Pick.Thing = Thing;
		Overseer->Select(Pick);
		Overseer->FocusOnSelection();
	}

	/** The window's place: centred above the bar, `Width` wide. */
	void HudScrPlace(float Scale, float Width)
	{
		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		const float W = FMath::Min(Width, Viewport->Size.x - 24.0f * Scale);
		ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, ColonyHud::AboveBar(Viewport, Scale)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(W, 0.0f), ImVec2(W, Viewport->Size.y * 0.75f));
	}

	/** The title line with a Close button at the right; true when closed. */
	bool HudScrTitle(const FString& Title, float Scale)
	{
		ColonyHud::Title(Title);
		ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 90.0f * Scale);
		return ColonyHud::Button(TEXT("Close##screen"), false, ImVec2(90.0f * Scale, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	}

	const TCHAR* const HudScrQualities[] = {TEXT("Awful"), TEXT("Poor"), TEXT("Normal"), TEXT("Good"), TEXT("Excellent"), TEXT("Masterwork"), TEXT("Legendary")};
	const char* const HudScrCareNames[] = {"NoCare", "NoMedicine", "Herbal", "Normal", "Best"};
	const TCHAR* const HudScrCareLabels[] = {TEXT("No care"), TEXT("No medicine"), TEXT("Herbal or worse"), TEXT("Normal or worse"), TEXT("Best")};
	// The prisoner modes the window offers, in PrisonerMode order.
	const TCHAR* const HudScrPrisonerModes[] = {TEXT("No interaction"), TEXT("Reduce resistance"), TEXT("Recruit"), TEXT("Release"), TEXT("Execute"),
		TEXT("Enslave"), TEXT("Reduce will")};
	constexpr int32 HudScrPrisonerModeCount = UE_ARRAY_COUNT(HudScrPrisonerModes);
	const TCHAR* const HudScrPrisonerModeTips[] = {
		TEXT("Wardens only feed it"),
		TEXT("Wardens talk its resistance down, but don't try to recruit it yet"),
		TEXT("Wardens talk its resistance down, then try to win it over to join the colony"),
		TEXT("A warden walks it out and lets it go home"),
		TEXT("A warden kills it"),
		TEXT("Wardens wear its will down to 0, then make it a slave of the colony"),
		TEXT("Wardens wear its will down, but don't enslave it yet")};

	/** Which group of the outfit editor an apparel def goes in, by its outermost layer. */
	FString HudScrApparelGroup(const colony::Def& Def)
	{
		bool bOverhead = false;
		bool bShell = false;
		bool bMiddle = false;
		bool bBelt = false;
		for (size_t Index = 0; Index < Def.count("apparel.layers"); ++Index)
		{
			const std::string Layer = Def.textAt("apparel.layers", Index);
			bOverhead = bOverhead || Layer == "Overhead" || Layer == "EyeCover";
			bShell = bShell || Layer == "Shell";
			bMiddle = bMiddle || Layer == "Middle";
			bBelt = bBelt || Layer == "Belt";
		}
		if (bOverhead)
		{
			return TEXT("Headgear");
		}
		if (bShell)
		{
			return TEXT("Outer layer");
		}
		if (bBelt)
		{
			return TEXT("Belts and packs");
		}
		if (bMiddle)
		{
			return TEXT("Middle layer");
		}
		return TEXT("On the skin");
	}
}

// ---- Dispatch ----------------------------------------------------------------------------------

void UColonyHudSubsystem::DrawScreen(USimSubsystem& Bridge, float Scale)
{
	if (OpenedScreen == EHudScreen::None)
	{
		return;
	}
	// A tab opened from the bar takes the window's place.
	if (GetOpenTab() != EHudTab::None)
	{
		OpenedScreen = EHudScreen::None;
		return;
	}
	switch (OpenedScreen)
	{
		case EHudScreen::Outfits: DrawOutfitsScreen(Bridge, Scale); break;
		case EHudScreen::Areas: DrawAreasScreen(Bridge, Scale); break;
		case EHudScreen::Medical: DrawMedicalScreen(Bridge, Scale); break;
		case EHudScreen::Prisoners: DrawPrisonersScreen(Bridge, Scale); break;
		default: break;
	}
	// Closed with its Close button (or Esc): back to the Assign tab it came from.
	if (OpenedScreen == EHudScreen::None)
	{
		OpenTab(EHudTab::Assign);
	}
}

// ---- Outfits -----------------------------------------------------------------------------------

void UColonyHudSubsystem::DrawOutfitsScreen(USimSubsystem& Bridge, float Scale)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	HudScrPlace(Scale, 1000.0f * Scale);
	ColonyHud::HoldWidth("##HudOutfits", Scale);
	if (!ImGui::Begin("##HudOutfits", nullptr, ColonyHud::BoxFlags()))
	{
		ImGui::End();
		return;
	}
	++Stats.ScreenFrames;
	if (HudScrTitle(TEXT("Outfits"), Scale))
	{
		OpenedScreen = EHudScreen::None;
	}
	ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("What colonists following an outfit may wear. Pick an outfit for each colonist in the Assign tab."));

	// Left: the outfits, a new one and removing one.
	const colony::OutfitPolicy* Policy = Sim.apparel().outfit(OutfitPicked);
	if (!Policy && !Sim.apparel().outfits().empty())
	{
		OutfitPicked = Sim.apparel().outfits().front().id;
		Policy = &Sim.apparel().outfits().front();
	}
	ImGui::BeginGroup();
	for (const colony::OutfitView& Outfit : Snapshot.outfits)
	{
		if (ColonyHud::Button(FString::Printf(TEXT("%s##outfit%u"), UTF8_TO_TCHAR(Outfit.name.c_str()), Outfit.id), Outfit.id == OutfitPicked, ImVec2(200.0f * Scale, 0.0f)))
		{
			OutfitPicked = Outfit.id;
		}
	}
	ImGui::Spacing();
	ImGui::SetNextItemWidth(200.0f * Scale);
	ImGui::InputTextWithHint("##NewOutfit", "New outfit's name", NewName, sizeof(NewName));
	if (ColonyHud::Button(TEXT("Add outfit"), false, ImVec2(200.0f * Scale, 0.0f)) && NewName[0] != 0)
	{
		Submit(colony::AddOutfitCommand{std::string(NewName)});
		NewName[0] = 0;
	}
	if (Policy && Snapshot.outfits.size() > 1 && !Snapshot.outfits.empty() && Policy->id != Snapshot.outfits.front().id)
	{
		if (ColonyHud::Button(TEXT("Remove this outfit"), false, ImVec2(200.0f * Scale, 0.0f)))
		{
			Submit(colony::RemoveOutfitCommand{Policy->id});
		}
		ColonyHud::Tip(TEXT("Colonists who followed it go back to the first outfit"));
	}
	ImGui::EndGroup();

	if (!Policy)
	{
		ImGui::End();
		return;
	}

	// Right: what it allows.
	ImGui::SameLine(0.0f, 20.0f * Scale);
	ImGui::BeginGroup();
	ColonyHud::Heading(UTF8_TO_TCHAR(Policy->name.c_str()));
	colony::EditOutfitCommand Edit;
	Edit.outfit = Policy->id;
	Edit.disallowed = Policy->disallowed;
	Edit.hitPointsMin = Policy->hitPointsMin;
	Edit.hitPointsMax = Policy->hitPointsMax;
	Edit.qualityMin = Policy->qualityMin;
	Edit.qualityMax = Policy->qualityMax;
	bool bChanged = false;

	// Condition: two sliders, sent when let go (while one is dragged it keeps its own value).
	if (!bHpDragging)
	{
		HpDrag[0] = Edit.hitPointsMin * 100.0f;
		HpDrag[1] = Edit.hitPointsMax * 100.0f;
	}
	bool bAnyActive = false;
	ImGui::SetNextItemWidth(200.0f * Scale);
	ImGui::SliderFloat("##hpmin", &HpDrag[0], 0.0f, 100.0f, "Condition from %.0f%%", ImGuiSliderFlags_AlwaysClamp);
	bAnyActive = bAnyActive || ImGui::IsItemActive();
	const bool bMinDone = ImGui::IsItemDeactivatedAfterEdit();
	ImGui::SameLine();
	ImGui::SetNextItemWidth(200.0f * Scale);
	ImGui::SliderFloat("##hpmax", &HpDrag[1], 0.0f, 100.0f, "to %.0f%%", ImGuiSliderFlags_AlwaysClamp);
	bAnyActive = bAnyActive || ImGui::IsItemActive();
	const bool bMaxDone = ImGui::IsItemDeactivatedAfterEdit();
	bHpDragging = bAnyActive;
	if (bMinDone || bMaxDone)
	{
		Edit.hitPointsMin = FMath::Min(HpDrag[0], HpDrag[1]) / 100.0f;
		Edit.hitPointsMax = FMath::Max(HpDrag[0], HpDrag[1]) / 100.0f;
		bChanged = true;
	}
	ColonyHud::Tip(TEXT("Worn-out clothes below the low end are swapped for better ones"));
	int32 QMin = Edit.qualityMin;
	int32 QMax = Edit.qualityMax;
	ColonyHud::Text(TEXT("Quality"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(150.0f * Scale);
	if (ImGui::BeginCombo("##qmin", TCHAR_TO_UTF8(HudScrQualities[FMath::Clamp(QMin, 0, 6)])))
	{
		for (int32 Q = 0; Q <= 6; ++Q)
		{
			if (ImGui::Selectable(TCHAR_TO_UTF8(HudScrQualities[Q]), Q == QMin))
			{
				Edit.qualityMin = Q;
				Edit.qualityMax = FMath::Max(Q, QMax);
				bChanged = true;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ColonyHud::Text(TEXT("to"));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(150.0f * Scale);
	if (ImGui::BeginCombo("##qmax", TCHAR_TO_UTF8(HudScrQualities[FMath::Clamp(QMax, 0, 6)])))
	{
		for (int32 Q = 0; Q <= 6; ++Q)
		{
			if (ImGui::Selectable(TCHAR_TO_UTF8(HudScrQualities[Q]), Q == QMax))
			{
				Edit.qualityMax = Q;
				Edit.qualityMin = FMath::Min(Q, QMin);
				bChanged = true;
			}
		}
		ImGui::EndCombo();
	}

	// Fight gear in the bag (docs/design/armour.md).
	bool bArmourInBag = Policy->armourInBag;
	if (ImGui::Checkbox("Keep armour in the bag, put it on to fight", &bArmourInBag))
	{
		Submit(colony::SetOutfitFightGearCommand{Policy->id, bArmourInBag, Policy->clothesInBag});
	}

	// The apparel, grouped by layer, a tick each.
	std::map<FString, std::vector<colony::DefId>> Groups;
	for (const colony::DefId Id : Sim.defs().idsOfKind("Things"))
	{
		const colony::Def& Def = Sim.defs().get(Id);
		if (Def.has("apparel"))
		{
			Groups[HudScrApparelGroup(Def)].push_back(Id);
		}
	}
	if (ColonyHud::Button(TEXT("Allow all")))
	{
		Edit.disallowed.clear();
		bChanged = true;
	}
	ImGui::SameLine();
	if (ColonyHud::Button(TEXT("Allow none")))
	{
		Edit.disallowed.clear();
		for (const auto& Group : Groups)
		{
			for (const colony::DefId Id : Group.second)
			{
				Edit.disallowed.push_back(Sim.defs().get(Id).defName());
			}
		}
		bChanged = true;
	}
	if (ImGui::BeginChild("##OutfitApparel", ImVec2(640.0f * Scale, ImGui::GetMainViewport()->Size.y * 0.4f), ImGuiChildFlags_Borders))
	{
		for (const auto& Group : Groups)
		{
			ColonyHud::Heading(Group.first);
			int32 Column = 0;
			for (const colony::DefId Id : Group.second)
			{
				const colony::Def& Def = Sim.defs().get(Id);
				const bool bWasAllowed = std::find(Policy->disallowed.begin(), Policy->disallowed.end(), Def.defName()) == Policy->disallowed.end();
				bool bAllowed = bWasAllowed;
				if (Column % 3 != 0)
				{
					ImGui::SameLine(Column % 3 * 205.0f * Scale);
				}
				ImGui::PushID(static_cast<int32>(Id.value));
				if (ImGui::Checkbox(TCHAR_TO_UTF8(*HudScrCapital(Def.label())), &bAllowed))
				{
					auto Found = std::find(Edit.disallowed.begin(), Edit.disallowed.end(), Def.defName());
					if (bAllowed && Found != Edit.disallowed.end())
					{
						Edit.disallowed.erase(Found);
					}
					else if (!bAllowed && Found == Edit.disallowed.end())
					{
						Edit.disallowed.push_back(Def.defName());
					}
					bChanged = true;
				}
				ImGui::PopID();
				++Column;
			}
		}
	}
	ImGui::EndChild();
	ImGui::EndGroup();
	if (bChanged)
	{
		std::sort(Edit.disallowed.begin(), Edit.disallowed.end());
		Edit.disallowed.erase(std::unique(Edit.disallowed.begin(), Edit.disallowed.end()), Edit.disallowed.end());
		Submit(Edit);
	}
	ImGui::End();
}

// ---- Allowed areas -----------------------------------------------------------------------------

void UColonyHudSubsystem::DrawAreasScreen(USimSubsystem& Bridge, float Scale)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>();
	HudScrPlace(Scale, 760.0f * Scale);
	ColonyHud::HoldWidth("##HudAreas", Scale);
	if (!ImGui::Begin("##HudAreas", nullptr, ColonyHud::BoxFlags()))
	{
		ImGui::End();
		return;
	}
	++Stats.ScreenFrames;
	if (HudScrTitle(TEXT("Allowed areas"), Scale))
	{
		OpenedScreen = EHudScreen::None;
		if (Orders && Orders->GetAreaToolArea() != 0)
		{
			Orders->SetAreaTool(0, true);
		}
	}
	ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Paint areas on the map, then limit colonists and animals to one in the Assign tab."));
	if (ImGui::BeginTable("##HudAreasTable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
	{
		for (const colony::AreaView& Area : Snapshot.areas)
		{
			ImGui::PushID(static_cast<int32>(Area.id));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ColonyHud::Text(UTF8_TO_TCHAR(Area.name.c_str()));
			ImGui::TableNextColumn();
			int32 Limited = 0;
			for (const colony::ThingView& View : Snapshot.things)
			{
				Limited += View.category == colony::ThingCategory::Pawn && View.area == Area.id ? 1 : 0;
			}
			ColonyHud::TextColored(ColonyHud::TextDim(), FString::Printf(TEXT("%u cells, %d limited to it"), Area.cellCount, Limited));
			ImGui::TableNextColumn();
			const bool bPainting = Orders && Orders->GetAreaToolArea() == Area.id && Orders->GetTool() == EOrderTool::PaintArea;
			const bool bErasing = Orders && Orders->GetAreaToolArea() == Area.id && Orders->GetTool() == EOrderTool::EraseArea;
			if (HudIcons::Button(HudIcons::Get(TEXT("area_paint"), ImGui::GetFrameHeight()), TEXT("Paint"), bPainting, ImVec2(0.0f, 0.0f)) && Orders)
			{
				Orders->SetAreaTool(bPainting ? 0 : Area.id, true);
			}
			ColonyHud::Tip(TEXT("Drag rectangles on the map to add them to the area. Right click or Esc stops"));
			ImGui::SameLine();
			if (HudIcons::Button(HudIcons::Get(TEXT("area_erase"), ImGui::GetFrameHeight()), TEXT("Erase"), bErasing, ImVec2(0.0f, 0.0f)) && Orders)
			{
				Orders->SetAreaTool(bErasing ? 0 : Area.id, false);
			}
			ColonyHud::Tip(TEXT("Drag rectangles on the map to take them out of the area"));
			ImGui::TableNextColumn();
			if (AreaPicked == Area.id)
			{
				ColonyHud::TextColored(ColonyHud::Warn(), TEXT("Delete?"));
				ImGui::SameLine();
				if (ColonyHud::Button(TEXT("Yes")))
				{
					Submit(colony::DeleteAreaCommand{Area.id});
					if (Orders && Orders->GetAreaToolArea() == Area.id)
					{
						Orders->SetAreaTool(0, true);
					}
					AreaPicked = 0;
				}
				ImGui::SameLine();
				if (ColonyHud::Button(TEXT("No")))
				{
					AreaPicked = 0;
				}
			}
			else if (ColonyHud::Button(TEXT("Delete")))
			{
				AreaPicked = Area.id;
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (Snapshot.areas.empty())
	{
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("No areas yet. Make one, then paint it."));
	}
	ImGui::Spacing();
	ImGui::SetNextItemWidth(260.0f * Scale);
	ImGui::InputTextWithHint("##NewArea", "New area's name", NewName, sizeof(NewName));
	ImGui::SameLine();
	if (ColonyHud::Button(TEXT("New area")))
	{
		const std::string Name = NewName[0] != 0 ? std::string(NewName) : "Area " + std::to_string(Snapshot.areas.size() + 1);
		Submit(colony::CreateAreaCommand{Name});
		NewName[0] = 0;
	}
	(void)Sim;
	ImGui::End();
}

// ---- Medical -----------------------------------------------------------------------------------

void UColonyHudSubsystem::DrawMedicalScreen(USimSubsystem& Bridge, float Scale)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	HudScrPlace(Scale, 1150.0f * Scale);
	ColonyHud::HoldWidth("##HudMedical", Scale);
	if (!ImGui::Begin("##HudMedical", nullptr, ColonyHud::BoxFlags()))
	{
		ImGui::End();
		return;
	}
	++Stats.ScreenFrames;
	if (HudScrTitle(TEXT("Medical"), Scale))
	{
		OpenedScreen = EHudScreen::None;
	}
	ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Colonists and prisoners: how they are, the care they get and operations planned for them."));

	// Left: the patients.
	TArray<const colony::ThingView*> Patients;
	for (const colony::ThingView& View : Snapshot.things)
	{
		if (View.category == colony::ThingCategory::Pawn && !View.dead && (Sim.colonistOf(View.id) || View.prisoner))
		{
			Patients.Add(&View);
		}
	}
	bool bPickedFound = false;
	for (const colony::ThingView* View : Patients)
	{
		bPickedFound = bPickedFound || View->id == PatientPicked;
	}
	if (!bPickedFound)
	{
		PatientPicked = Patients.Num() > 0 ? Patients[0]->id : colony::kNoEntity;
	}
	ImGui::BeginGroup();
	if (ImGui::BeginTable("##HudMedicalList", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
		ImVec2(470.0f * Scale, ImGui::GetMainViewport()->Size.y * 0.45f)))
	{
		for (const colony::ThingView* View : Patients)
		{
			ImGui::PushID(HudScrId(View->id));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			FString Name = HudScrPawnName(Sim, *View);
			if (View->prisoner)
			{
				Name += TEXT(" (prisoner)");
			}
			if (ImGui::Selectable(TCHAR_TO_UTF8(*Name), View->id == PatientPicked, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(200.0f * Scale, 0.0f)))
			{
				PatientPicked = View->id;
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					HudScrGoTo(GetWorld(), View->id);
				}
			}
			ImGui::TableNextColumn();
			if (View->downed)
			{
				ColonyHud::TextColored(ColonyHud::Bad(), TEXT("Down"));
			}
			else if (View->bleedPerDay > 0.0f)
			{
				ColonyHud::TextColored(ColonyHud::Warn(), FString::Printf(TEXT("Bleeding %.0f%%/day"), View->bleedPerDay * 100.0f));
			}
			else
			{
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Up"));
			}
			ImGui::TableNextColumn();
			const int32 Care = FMath::Clamp(static_cast<int32>(Sim.medicine().careOf(View->id)), 0, 4);
			ImGui::SetNextItemWidth(170.0f * Scale);
			if (ImGui::BeginCombo("##care", TCHAR_TO_UTF8(HudScrCareLabels[Care])))
			{
				for (int32 Index = 0; Index < 5; ++Index)
				{
					if (ImGui::Selectable(TCHAR_TO_UTF8(HudScrCareLabels[Index]), Index == Care))
					{
						Submit(colony::SetMedicalCareCommand{View->id, HudScrCareNames[Index]});
					}
				}
				ImGui::EndCombo();
			}
			ColonyHud::Tip(TEXT("The best medicine a doctor may use on them"));
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::EndGroup();

	// Right: the picked patient's health and operations.
	ImGui::SameLine(0.0f, 16.0f * Scale);
	ImGui::BeginGroup();
	colony::HealthReport Report;
	if (PatientPicked != colony::kNoEntity && Sim.healthReport(PatientPicked, Report))
	{
		const colony::ThingView* View = Snapshot.find(PatientPicked);
		ColonyHud::Heading(View ? HudScrPawnName(Sim, *View) : FString(TEXT("Patient")));
		ColonyHud::TextColored(ColonyHud::TextDim(), FString::Printf(TEXT("Pain %.0f%%   Blood lost %.0f%%   Bleeding %.0f%% a day"), Report.pain * 100.0f,
			Report.bloodLoss * 100.0f, Report.bleedPerDay * 100.0f));
		if (ImGui::BeginChild("##HudMedicalReport", ImVec2(600.0f * Scale, ImGui::GetMainViewport()->Size.y * 0.22f), ImGuiChildFlags_Borders))
		{
			if (Report.injuries.empty() && Report.missingParts.empty() && Report.diseases.empty() && Report.addedParts.empty())
			{
				ColonyHud::TextColored(ColonyHud::Good(), TEXT("Healthy"));
			}
			for (const colony::HealthReport::InjuryLine& Line : Report.injuries)
			{
				const ImVec4 Color = Line.permanent ? ColonyHud::TextDim() : (Line.tended ? ColonyHud::TextMain() : ColonyHud::Warn());
				ColonyHud::TextColored(Color, FString::Printf(TEXT("%s: %s%s%s"), *HudScrCapital(Line.part), UTF8_TO_TCHAR(Line.injury.c_str()),
					Line.tended ? TEXT(" (tended)") : TEXT(""), Line.permanent ? TEXT(" (old)") : TEXT("")));
			}
			for (const std::string& Part : Report.missingParts)
			{
				ColonyHud::TextColored(ColonyHud::Bad(), FString::Printf(TEXT("%s: missing"), *HudScrCapital(Part)));
			}
			for (const colony::HealthReport::AddedLine& Line : Report.addedParts)
			{
				ColonyHud::Text(FString::Printf(TEXT("%s: %s (%.0f%%)"), *HudScrCapital(Line.part), UTF8_TO_TCHAR(Line.label.c_str()), Line.efficiency * 100.0f));
			}
			for (const colony::HealthReport::DiseaseLine& Line : Report.diseases)
			{
				if (Line.visible)
				{
					ColonyHud::TextColored(Line.lifeThreatening ? ColonyHud::Bad() : ColonyHud::Warn(), FString::Printf(TEXT("%s (%s), immunity %.0f%%"),
						*HudScrCapital(Line.name), UTF8_TO_TCHAR(Line.stage.c_str()), Line.immunity * 100.0f));
				}
			}
		}
		ImGui::EndChild();

		// Operations planned, and a new one.
		ColonyHud::Heading(TEXT("Operations"));
		const std::vector<colony::OperationLine> Lines = Sim.surgery().lines(PatientPicked);
		if (Lines.empty())
		{
			ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("None planned."));
		}
		for (const colony::OperationLine& Line : Lines)
		{
			ImGui::PushID(static_cast<int32>(Line.id));
			ColonyHud::Text(FString::Printf(TEXT("%s: %s (Medicine %d)"), *HudScrCapital(Line.label), UTF8_TO_TCHAR(Line.part.c_str()), Line.minSkill));
			ImGui::SameLine();
			if (ColonyHud::Button(TEXT("Remove")))
			{
				Submit(colony::RemoveOperationCommand{PatientPicked, Line.id});
			}
			ImGui::PopID();
		}
		ImGui::SetNextItemWidth(420.0f * Scale);
		if (ImGui::BeginCombo("##AddOperation", "Add an operation..."))
		{
			const std::vector<std::string> PartLabels = View ? Sim.health().partLabels(View->def) : std::vector<std::string>();
			for (const colony::SurgeryRecipe& Recipe : Sim.surgery().recipes())
			{
				for (const int Part : Sim.surgery().targets(PatientPicked, Recipe))
				{
					const std::string PartLabel = Part >= 0 && Part < static_cast<int>(PartLabels.size()) ? PartLabels[static_cast<size_t>(Part)] : std::string();
					const FString Label = FString::Printf(TEXT("%s: %s##op%s%d"), *HudScrCapital(Recipe.label), UTF8_TO_TCHAR(PartLabel.c_str()),
						UTF8_TO_TCHAR(Recipe.defName.c_str()), Part);
					if (ImGui::Selectable(TCHAR_TO_UTF8(*Label)))
					{
						Submit(colony::AddOperationCommand{PatientPicked, Recipe.defName, Part});
					}
				}
			}
			ImGui::EndCombo();
		}
		ColonyHud::Tip(TEXT("Operations that can be done on this patient now. A doctor does them in a bed"));
	}
	else
	{
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("No patient."));
	}
	ImGui::EndGroup();
	ImGui::End();
}

// ---- Prisoners ---------------------------------------------------------------------------------

void UColonyHudSubsystem::DrawPrisonersScreen(USimSubsystem& Bridge, float Scale)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	HudScrPlace(Scale, 980.0f * Scale);
	ColonyHud::HoldWidth("##HudPrisoners", Scale);
	if (!ImGui::Begin("##HudPrisoners", nullptr, ColonyHud::BoxFlags()))
	{
		ImGui::End();
		return;
	}
	++Stats.ScreenFrames;
	if (HudScrTitle(TEXT("Prisoners"), Scale))
	{
		OpenedScreen = EHudScreen::None;
	}
	ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Downed enemies are carried to a prison bed by wardens. Here you say what wardens do with them."));
	ColonyHud::Heading(TEXT("Prisoners"));
	int32 Prisoners = 0;
	if (ImGui::BeginTable("##HudPrisonersTable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
	{
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category != colony::ThingCategory::Pawn || View.dead || !View.prisoner)
			{
				continue;
			}
			++Prisoners;
			ImGui::PushID(HudScrId(View.id));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			const FString Name = HudScrPawnName(Sim, View);
			if (ImGui::Selectable(TCHAR_TO_UTF8(*Name), false, ImGuiSelectableFlags_None, ImVec2(200.0f * Scale, 0.0f)))
			{
				HudScrGoTo(GetWorld(), View.id);
			}
			ColonyHud::Tip(TEXT("Click to go there"));
			ImGui::TableNextColumn();
			ColonyHud::TextColored(ColonyHud::TextDim(), FString::Printf(TEXT("Resistance %.1f  Will %.1f"), View.resistance, View.will));
			ColonyHud::Tip(TEXT("Resistance: wardens talk it down to 0, then a recruit attempt can win the prisoner over.\nWill: wardens wear it down to 0, then the prisoner can be enslaved."));
			ImGui::TableNextColumn();
			const int32 Mode = FMath::Clamp(static_cast<int32>(View.prisonerMode), 0, HudScrPrisonerModeCount - 1);
			ImGui::SetNextItemWidth(220.0f * Scale);
			if (ImGui::BeginCombo("##mode", TCHAR_TO_UTF8(HudScrPrisonerModes[Mode])))
			{
				for (int32 Index = 0; Index < HudScrPrisonerModeCount; ++Index)
				{
					if (ImGui::Selectable(TCHAR_TO_UTF8(HudScrPrisonerModes[Index]), Index == Mode))
					{
						Submit(colony::SetPrisonerModeCommand{View.id, colony::prisonerModeName(static_cast<colony::PrisonerMode>(Index))});
					}
					ColonyHud::Tip(HudScrPrisonerModeTips[Index]);
				}
				ImGui::EndCombo();
			}
			ImGui::TableNextColumn();
			if (View.downed)
			{
				ColonyHud::TextColored(ColonyHud::Warn(), TEXT("Down"));
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (Prisoners == 0)
	{
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("No prisoners."));
	}

	// Slaves (docs/design/prisoners.md, Slaves): their suppression, and freeing them.
	ImGui::Spacing();
	ColonyHud::Heading(TEXT("Slaves"));
	int32 Slaves = 0;
	if (ImGui::BeginTable("##HudSlavesTable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
	{
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category != colony::ThingCategory::Pawn || View.dead || !View.slave)
			{
				continue;
			}
			++Slaves;
			ImGui::PushID(HudScrId(View.id));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			const FString Name = HudScrPawnName(Sim, View);
			if (ImGui::Selectable(TCHAR_TO_UTF8(*Name), false, ImGuiSelectableFlags_None, ImVec2(200.0f * Scale, 0.0f)))
			{
				HudScrGoTo(GetWorld(), View.id);
			}
			ColonyHud::Tip(TEXT("Click to go there"));
			ImGui::TableNextColumn();
			const float Suppression = FMath::Clamp(View.suppression, 0.0f, 1.0f);
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Suppression < 0.333f ? ColonyHud::Bad() : (Suppression < 0.5f ? ColonyHud::Warn() : ColonyHud::Good()));
			ImGui::ProgressBar(Suppression, ImVec2(180.0f * Scale, 0.0f), TCHAR_TO_UTF8(*FString::Printf(TEXT("Suppression %d%%"), FMath::RoundToInt32(Suppression * 100.0f))));
			ImGui::PopStyleColor();
			ColonyHud::Tip(TEXT("Falls by itself; wardens raise it by talking to the slave. Low suppression: the slaves may rise up, more so when no free colonist is around."));
			ImGui::TableNextColumn();
			if (ColonyHud::Button(TEXT("Emancipate")))
			{
				Submit(colony::SetPrisonerModeCommand{View.id, "Emancipate"});
			}
			ColonyHud::Tip(TEXT("Free the slave: it stays and joins the colony as a colonist"));
			ImGui::SameLine();
			if (ColonyHud::Button(TEXT("Release")))
			{
				Submit(colony::SetPrisonerModeCommand{View.id, "Release"});
			}
			ColonyHud::Tip(TEXT("Free the slave and let it go home"));
			ImGui::TableNextColumn();
			if (View.downed)
			{
				ColonyHud::TextColored(ColonyHud::Warn(), TEXT("Down"));
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (Slaves == 0)
	{
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("No slaves. Set a prisoner to Enslave to make one."));
	}
	else
	{
		ColonyHud::TextColored(ColonyHud::TextFaint(), TEXT("Slaves work at 85% speed, can't be drafted and don't do warden, hunting, art or research work."));
	}

	// Beds: mark one for prisoners.
	ImGui::Spacing();
	ColonyHud::Heading(TEXT("Beds"));
	int32 Beds = 0;
	for (const colony::ThingView& View : Snapshot.things)
	{
		const colony::Def& Def = Sim.defs().get(View.def);
		if (View.category != colony::ThingCategory::Building || View.stage != colony::ConstructionStage::Built || !Def.flag("building.bed"))
		{
			continue;
		}
		++Beds;
		ImGui::PushID(HudScrId(View.id));
		bool bPrison = View.prisonBed;
		if (ImGui::Checkbox(TCHAR_TO_UTF8(*FString::Printf(TEXT("%s at %d, %d: for prisoners"), *HudScrCapital(Def.label()),
			FMath::FloorToInt32(View.x), FMath::FloorToInt32(View.y))), &bPrison))
		{
			Submit(colony::SetPrisonBedCommand{View.id, bPrison});
		}
		ImGui::SameLine();
		if (ColonyHud::Button(TEXT("Show")))
		{
			HudScrGoTo(GetWorld(), View.id);
		}
		ImGui::PopID();
	}
	if (Beds == 0)
	{
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("No beds built yet. A prison bed should stand in a closed room."));
	}
	ImGui::End();
}

// ---- The main menu -----------------------------------------------------------------------------

void UColonyHudSubsystem::SendPendingNarrator(USimSubsystem& Bridge)
{
	if (bNarratorPending && Bridge.GetSimGeneration() != NarratorGeneration)
	{
		bNarratorPending = false;
		Submit(colony::SetNarratorCommand{TCHAR_TO_UTF8(*NewNarrator), TCHAR_TO_UTF8(*NewDifficulty)});
	}
}

bool UColonyHudSubsystem::StartNewColony(bool bPickSite)
{
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge)
	{
		return false;
	}
	FSimStartOptions Options = Bridge->GetStartOptions();
	Options.bTitle = false;
	Options.Scenario = NewScenario;
	Options.Seed = static_cast<uint64>(FMath::Max(NewSeed, 0));
	Options.MapSize = NewMapSize;
	Options.Pawns = 0;
	Options.Items = 0;
	Options.bFlatMap = false;
	Options.WildAnimals = 100;
	Options.bWorld = true;
	Options.WorldSeed = 0;
	Options.WorldStartTile = -1;
	Options.DayMinutes = USimSubsystem::GetSavedDayMinutes();
	bNarratorPending = true;
	NarratorGeneration = Bridge->GetSimGeneration();
	// The biomes a site may be in: the scenario's `siteBiomes`, else its `biome` (docs/design/new-game.md).
	TArray<FString> Biomes;
	if (const colony::DefDatabase* Defs = Bridge->GetDefs().get())
	{
		if (const colony::Def* Scenario = Defs->find("Scenarios", TCHAR_TO_UTF8(*NewScenario)))
		{
			for (size_t i = 0; i < Scenario->count("siteBiomes"); ++i)
			{
				Biomes.Add(UTF8_TO_TCHAR(Scenario->textAt("siteBiomes", i).c_str()));
			}
			if (Biomes.IsEmpty() && !Scenario->text("biome").empty())
			{
				Biomes.Add(UTF8_TO_TCHAR(Scenario->text("biome").c_str()));
			}
		}
	}
	if (bPickSite)
	{
		// The world map picks the tile; its "Land here" starts the sim with these options and opens the people page.
		Bridge->SetStartOptions(Options);
		if (UWorldScreenSubsystem* WorldScreen = GetWorld()->GetSubsystem<UWorldScreenSubsystem>())
		{
			WorldScreen->MakeWorld(NewSeed);
			WorldScreen->SetNewGame(Biomes);
			WorldScreen->SetOpen(true);
			bAwaitingLanding = true;
			bMainMenuOpen = false;
			return true;
		}
		return false;
	}
	Bridge->SetSpeed(0);  // held at its first second for the character screen
	const bool bStarted = Bridge->StartSim(Options);
	if (bStarted)
	{
		EnterPeoplePage(*Bridge);
	}
	return bStarted;
}

void UColonyHudSubsystem::DrawMainMenu(USimSubsystem& Bridge)
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
	const float Scale = ColonyHud::ViewScale(Viewport);
	ColonyHud::ApplyFrameStyle(Scale);
	++Stats.MainMenuFrames;

	if (MainMenuPage == 3 || MainMenuPage == 4)
	{
		// The people page shows the colony's real 3D people: no veil over them.
		EndTitleCamera();
		DrawPeoplePage(Bridge, Scale);
	}
	else
	{
		EndPeopleCamera();
		// The title's own living backdrop (a wild rainforest map) gets a slow camera of its own; over a game in progress
		// the menu shows that game behind it.
		if (Bridge.IsTitleBackdrop())
		{
			UpdateTitleCamera(Bridge);
		}
		else
		{
			EndTitleCamera();
		}
		DrawMenuBackdrop(Bridge, Scale);
		if (MainMenuPage == 0)
		{
			DrawTitlePage(Bridge, Scale);
		}
		else if (MainMenuPage == 1)
		{
			DrawScenarioPage(Bridge, Scale);
		}
		else if (MainMenuPage == 5)
		{
			DrawDebugPage(Scale);
		}
		else
		{
			DrawLoadPage(Bridge, Scale);
		}
	}
	if (bSettingsOpen)
	{
		DrawSettings(Bridge, Scale);
	}
}
