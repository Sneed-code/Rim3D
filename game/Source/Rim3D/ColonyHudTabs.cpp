// The bottom bar's tabs that UColonyHudSubsystem draws itself (docs/design/ui-layout.md): Assign, Animals, Wildlife, Mechs,
// Research and Inventory. Build, Work, Schedule, Quests and World are drawn by their own subsystems.

#include "ColonyHudSubsystem.h"

#include "Engine/World.h"

#include <imgui.h>

#include "ColonyHudStyle.h"

#include "OverseerSubsystem.h"
#include "PanelsSubsystem.h"
#include "SimPicking.h"
#include "SimSubsystem.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Drugs.h"
#include "ColonySim/Medicine.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace
{
	FString HudTabCapital(const std::string& Text)
	{
		FString Out = UTF8_TO_TCHAR(Text.c_str());
		if (Out.Len() > 0)
		{
			Out[0] = FChar::ToUpper(Out[0]);
		}
		return Out;
	}

	std::string HudTabCapitalUtf8(const std::string& Text)
	{
		std::string Out = Text;
		if (!Out.empty() && Out[0] >= 'a' && Out[0] <= 'z')
		{
			Out[0] = static_cast<char>(Out[0] - 'a' + 'A');
		}
		return Out;
	}

	int32 HudTabId(colony::Entity Id)
	{
		return static_cast<int32>(entt::to_integral(Id));
	}

	FString HudTabPawnName(const colony::Sim& Sim, const colony::ThingView& View)
	{
		if (const colony::Colonist* Colonist = Sim.colonistOf(View.id))
		{
			return UTF8_TO_TCHAR((Colonist->nickName.empty() ? Colonist->firstName : Colonist->nickName).c_str());
		}
		if (!View.animalName.empty())
		{
			return UTF8_TO_TCHAR(View.animalName.c_str());
		}
		return HudTabCapital(Sim.defs().get(View.def).label());
	}

	/** A column header with its tooltip. */
	void HudTabHeader(const TCHAR* Label, const TCHAR* Tip)
	{
		ImGui::TableNextColumn();
		ColonyHud::TextColored(ColonyHud::TextDim(), Label);
		ColonyHud::Tip(Tip);
	}

	/** Selects a thing in the overseer view (and centres on it with `bFocus`). */
	void HudTabSelect(UWorld* World, colony::Entity Thing, bool bFocus)
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
		if (bFocus)
		{
			Overseer->FocusOnSelection();
		}
	}

	/** A name button: click selects, double click centres the camera on it. */
	void HudTabNameButton(UWorld* World, const FString& Name, colony::Entity Thing, float Width)
	{
		const FTCHARToUTF8 Utf8(*Name);
		if (ImGui::Selectable(Utf8.Get(), false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(Width, 0.0f)))
		{
			HudTabSelect(World, Thing, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
		}
		ColonyHud::Tip(TEXT("Click to select, double click to go there"));
	}

	const char* const HudTabCareNames[] = {"NoCare", "NoMedicine", "Herbal", "Normal", "Best"};
	const TCHAR* const HudTabCareLabels[] = {TEXT("No care"), TEXT("No medicine"), TEXT("Herbal or worse"), TEXT("Normal or worse"), TEXT("Best")};
	const char* const HudTabTrainables[] = {"Obedience", "Release", "Rescue", "Haul"};
}

void UColonyHudSubsystem::PlaceTabWindow(float Scale, float Width, float MaxHeightShare) const
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float W = FMath::Min(Width, Viewport->Size.x - 24.0f * Scale);
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, ColonyHud::AboveBar(Viewport, Scale)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(W, 0.0f), ImVec2(W, Viewport->Size.y * MaxHeightShare));
}

// ---- Assign ------------------------------------------------------------------------------------

void UColonyHudSubsystem::DrawAssignTab(USimSubsystem& Bridge, float Scale)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	PlaceTabWindow(Scale, 1100.0f * Scale);
	if (ImGui::Begin("##HudAssign", nullptr, ColonyHud::BoxFlags()))
	{
		++Stats.TabWindowFrames;
		ColonyHud::Title(TEXT("Assign"));
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("What each colonist may wear, take and be treated with, and where they may go."));
		// The manager windows (ColonyHudScreens.cpp).
		const struct
		{
			const TCHAR* Label;
			EHudScreen Screen;
			const TCHAR* Tip;
		} Managers[] = {
			{TEXT("Manage outfits"), EHudScreen::Outfits, TEXT("Make outfits and say what each allows")},
			{TEXT("Manage areas"), EHudScreen::Areas, TEXT("Make allowed areas and paint them on the map")},
			{TEXT("Medical"), EHudScreen::Medical, TEXT("Health, medical care and operations of colonists and prisoners")},
			{TEXT("Prisoners"), EHudScreen::Prisoners, TEXT("What wardens do with each prisoner, and prison beds")},
		};
		for (const auto& Manager : Managers)
		{
			if (ColonyHud::Button(Manager.Label, OpenedScreen == Manager.Screen))
			{
				// The window takes the tab's place; its Close brings the tab back.
				OpenedScreen = Manager.Screen;
				CloseTabs();
			}
			ColonyHud::Tip(Manager.Tip);
			ImGui::SameLine();
		}
		ImGui::NewLine();
		const std::vector<colony::DefId> Policies = Sim.defs().idsOfKind("DrugPolicies");
		const std::string DefaultPolicy = Sim.drugs().rules().defaultPolicy;
		const float Combo = 190.0f * Scale;
		if (ImGui::BeginTable("##HudAssignTable", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
			ImVec2(0.0f, FMath::Min(ImGui::GetMainViewport()->Size.y * 0.55f, 60.0f * Scale + ImGui::GetFrameHeightWithSpacing() * 12.0f))))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			HudTabHeader(TEXT("Colonist"), TEXT("Click to select, double click to go there"));
			HudTabHeader(TEXT("Outfit"), TEXT("Which clothes they put on. Outfits say what may be worn and how worn out it may be"));
			HudTabHeader(TEXT("Drug policy"), TEXT("Which drugs they take by themselves, and when"));
			HudTabHeader(TEXT("Medical care"), TEXT("The best medicine a doctor may use on them"));
			HudTabHeader(TEXT("Allowed area"), TEXT("The area they stay in. Unrestricted: anywhere"));
			for (const colony::ThingView& View : Snapshot.things)
			{
				const colony::Colonist* Colonist = Sim.colonistOf(View.id);
				if (View.category != colony::ThingCategory::Pawn || View.dead || !Colonist || View.faction != 1)
				{
					continue;
				}
				ImGui::PushID(HudTabId(View.id));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				HudTabNameButton(GetWorld(), HudTabPawnName(Sim, View), View.id, 150.0f * Scale);

				// Outfit.
				ImGui::TableNextColumn();
				FString OutfitName = TEXT("(none)");
				for (const colony::OutfitView& Outfit : Snapshot.outfits)
				{
					OutfitName = Outfit.id == View.outfit ? FString(UTF8_TO_TCHAR(Outfit.name.c_str())) : OutfitName;
				}
				ImGui::SetNextItemWidth(Combo);
				if (ImGui::BeginCombo("##outfit", TCHAR_TO_UTF8(*OutfitName)))
				{
					for (const colony::OutfitView& Outfit : Snapshot.outfits)
					{
						if (ImGui::Selectable(Outfit.name.c_str(), Outfit.id == View.outfit))
						{
							Submit(colony::SetOutfitCommand{View.id, Outfit.id});
						}
					}
					ImGui::EndCombo();
				}

				// Drug policy.
				ImGui::TableNextColumn();
				const colony::DrugState* Drugs = Sim.things().registry().try_get<colony::DrugState>(View.id);
				const std::string Current = Drugs && !Drugs->policy.empty() ? Drugs->policy : DefaultPolicy;
				const colony::Def* CurrentDef = Sim.defs().find("DrugPolicies", Current);
				ImGui::SetNextItemWidth(Combo);
				if (ImGui::BeginCombo("##drugs", CurrentDef ? HudTabCapitalUtf8(CurrentDef->label()).c_str() : Current.c_str()))
				{
					for (const colony::DefId Policy : Policies)
					{
						const colony::Def& Def = Sim.defs().get(Policy);
						if (ImGui::Selectable(HudTabCapitalUtf8(Def.label()).c_str(), Def.defName() == Current))
						{
							Submit(colony::SetDrugPolicyCommand{View.id, Def.defName()});
						}
					}
					ImGui::EndCombo();
				}

				// Medical care.
				ImGui::TableNextColumn();
				const int32 Care = FMath::Clamp(static_cast<int32>(Sim.medicine().careOf(View.id)), 0, 4);
				ImGui::SetNextItemWidth(Combo);
				if (ImGui::BeginCombo("##care", TCHAR_TO_UTF8(HudTabCareLabels[Care])))
				{
					for (int32 Index = 0; Index < 5; ++Index)
					{
						if (ImGui::Selectable(TCHAR_TO_UTF8(HudTabCareLabels[Index]), Index == Care))
						{
							Submit(colony::SetMedicalCareCommand{View.id, HudTabCareNames[Index]});
						}
					}
					ImGui::EndCombo();
				}

				// Allowed area.
				ImGui::TableNextColumn();
				FString AreaName = TEXT("Unrestricted");
				for (const colony::AreaView& Area : Snapshot.areas)
				{
					AreaName = Area.id == View.area ? FString(UTF8_TO_TCHAR(Area.name.c_str())) : AreaName;
				}
				ImGui::SetNextItemWidth(Combo);
				if (ImGui::BeginCombo("##area", TCHAR_TO_UTF8(*AreaName)))
				{
					if (ImGui::Selectable("Unrestricted", View.area == 0))
					{
						Submit(colony::AssignAreaCommand{View.id, 0});
					}
					for (const colony::AreaView& Area : Snapshot.areas)
					{
						if (ImGui::Selectable(Area.name.c_str(), Area.id == View.area))
						{
							Submit(colony::AssignAreaCommand{View.id, Area.id});
						}
					}
					ImGui::EndCombo();
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (Snapshot.areas.empty())
		{
			ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("No allowed areas yet."));
		}
	}
	ImGui::End();
}

// ---- Animals and Wildlife ----------------------------------------------------------------------

void UColonyHudSubsystem::DrawAnimalsTab(USimSubsystem& Bridge, float Scale, bool bWild)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	PlaceTabWindow(Scale, (bWild ? 820.0f : 1420.0f) * Scale);
	if (ImGui::Begin(bWild ? "##HudWildlife" : "##HudAnimals", nullptr, ColonyHud::BoxFlags()))
	{
		++Stats.TabWindowFrames;
		ColonyHud::Title(bWild ? TEXT("Wildlife") : TEXT("Animals"));
		ColonyHud::TextColored(ColonyHud::TextDim(), bWild ? TEXT("Wild animals on the map. Tame needs a colonist with Handling work; Hunt one with Hunting work and a gun.")
			: TEXT("The colony's tame animals: what they learn, the zone or pen they stay in, breeding, and their names."));
		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		const int32 Columns = bWild ? 5 : 12;
		int32 Rows = 0;
		if (ImGui::BeginTable("##HudAnimalTable", Columns, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
			ImVec2(0.0f, Viewport->Size.y * 0.5f)))
		{
			// Fixed widths, so the headers read in full even with no rows under them (empty columns would get no width).
			static const float WildWidths[] = {150.0f, 110.0f, 110.0f, 90.0f, 130.0f};
			static const float TameWidths[] = {150.0f, 110.0f, 110.0f, 90.0f, 80.0f, 70.0f, 70.0f, 60.0f, 120.0f, 50.0f, 180.0f, 150.0f};
			for (int32 Column = 0; Column < Columns; ++Column)
			{
				ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, (bWild ? WildWidths[Column] : TameWidths[Column]) * Scale);
			}
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			HudTabHeader(TEXT("Name"), TEXT("Click to select, double click to go there"));
			HudTabHeader(TEXT("Kind"), TEXT("What animal it is"));
			HudTabHeader(TEXT("Age"), TEXT("Baby, young or grown, and its sex"));
			HudTabHeader(TEXT("Health"), TEXT("Hit points"));
			if (bWild)
			{
				HudTabHeader(TEXT("Marks"), TEXT("Tame: a handler tries to tame it. Hunt: a hunter shoots it for meat"));
			}
			else
			{
				for (const char* Trick : HudTabTrainables)
				{
					const FString Label = UTF8_TO_TCHAR(Trick);
					HudTabHeader(*Label, TEXT("Tick to have a handler teach it this. Green: learned"));
				}
				HudTabHeader(TEXT("Zone"), TEXT("The animal zone or pen it stays in. None: it roams near home"));
				HudTabHeader(TEXT("Breed"), TEXT("Tick to let it mate. Its zone can switch breeding off for all its animals too"));
				HudTabHeader(TEXT("Products"), TEXT("Carrying young, and how full its milk or wool is; handlers milk and shear it when full"));
				HudTabHeader(TEXT("Bed"), TEXT("The animal bed it sleeps in. Build animal sleeping spots, boxes or beds (Furniture)"));
			}
			for (const colony::ThingView& View : Snapshot.things)
			{
				const bool bAnimal = View.category == colony::ThingCategory::Pawn && View.lifeStage >= 0 && !View.dead;
				if (!bAnimal || (bWild ? View.faction != 100 : View.faction != 1))
				{
					continue;
				}
				++Rows;
				const colony::Def& Def = Sim.defs().get(View.def);
				ImGui::PushID(HudTabId(View.id));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (!bWild && RenamingAnimal == View.id)
				{
					ImGui::SetNextItemWidth(150.0f * Scale);
					if (ImGui::InputText("##name", AnimalName, sizeof(AnimalName), ImGuiInputTextFlags_EnterReturnsTrue))
					{
						Submit(colony::SetAnimalNameCommand{View.id, std::string(AnimalName)});
						RenamingAnimal = colony::kNoEntity;
					}
					if (ImGui::IsItemDeactivated() && RenamingAnimal == View.id)
					{
						RenamingAnimal = colony::kNoEntity;
					}
				}
				else
				{
					HudTabNameButton(GetWorld(), HudTabPawnName(Sim, View), View.id, 150.0f * Scale);
					if (!bWild && ImGui::BeginPopupContextItem("##rename"))
					{
						if (ImGui::Selectable("Rename"))
						{
							RenamingAnimal = View.id;
							FCStringAnsi::Strncpy(AnimalName, View.animalName.c_str(), sizeof(AnimalName));
						}
						if (ImGui::Selectable(View.markSlaughter ? "Don't slaughter" : "Slaughter"))
						{
							Submit(colony::MarkAnimalCommand{View.id, "Slaughter", !View.markSlaughter});
						}
						if (ImGui::Selectable("Release to the wild"))
						{
							Submit(colony::MarkAnimalCommand{View.id, "Release", true});
						}
						ImGui::EndPopup();
					}
				}
				ImGui::TableNextColumn();
				ColonyHud::Text(HudTabCapital(Def.label()));
				ImGui::TableNextColumn();
				const TCHAR* const Stages[] = {TEXT("Baby"), TEXT("Young"), TEXT("Grown")};
				ColonyHud::Text(FString::Printf(TEXT("%s%s"), Stages[FMath::Clamp(View.lifeStage, 0, 2)],
					View.sex == 1 ? TEXT(", female") : (View.sex == 0 ? TEXT(", male") : TEXT(""))));
				ImGui::TableNextColumn();
				if (View.maxHitPoints > 0 && (View.downed || View.hitPoints < View.maxHitPoints))
				{
					const float Share = static_cast<float>(View.hitPoints) / static_cast<float>(View.maxHitPoints);
					ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Share < 0.5f ? ColonyHud::Bad() : ColonyHud::Good());
					ImGui::ProgressBar(Share, ImVec2(90.0f * Scale, 0.0f), View.downed ? "down" : "");
					ImGui::PopStyleColor();
				}
				else
				{
					ColonyHud::TextColored(ColonyHud::Good(), TEXT("Healthy"));
				}
				if (bWild)
				{
					ImGui::TableNextColumn();
					if (ColonyHud::Button(TEXT("Tame"), View.markTame))
					{
						Submit(colony::MarkAnimalCommand{View.id, "Tame", !View.markTame});
					}
					ImGui::SameLine();
					if (ColonyHud::Button(TEXT("Hunt"), View.markHunt))
					{
						Submit(colony::MarkAnimalCommand{View.id, "Hunt", !View.markHunt});
					}
				}
				else
				{
					for (int32 Trick = 0; Trick < 4; ++Trick)
					{
						ImGui::TableNextColumn();
						const uint32 Bit = 1u << Trick;
						bool bWanted = (View.wantedTricks & Bit) != 0;
						const bool bLearned = (View.learnedTricks & Bit) != 0;
						if (bLearned)
						{
							ImGui::PushStyleColor(ImGuiCol_CheckMark, ColonyHud::Good());
						}
						ImGui::PushID(Trick);
						if (ImGui::Checkbox("##trick", &bWanted))
						{
							Submit(colony::SetTrainingCommand{View.id, HudTabTrainables[Trick], bWanted});
						}
						ImGui::PopID();
						if (bLearned)
						{
							ImGui::PopStyleColor();
							ColonyHud::Tip(TEXT("Learned"));
						}
					}
					ImGui::TableNextColumn();
					bool bPen = false;
					for (const colony::AnimalZoneView& Zone : Snapshot.animalZones)
					{
						bPen = bPen || (Zone.id == View.animalZone && Zone.pen);
					}
					const FString ZoneName = View.animalZone == 0 ? FString(TEXT("None")) : FString::Printf(TEXT("%s %u"), bPen ? TEXT("Pen") : TEXT("Zone"), View.animalZone);
					ImGui::SetNextItemWidth(120.0f * Scale);
					if (ImGui::BeginCombo("##zone", TCHAR_TO_UTF8(*ZoneName)))
					{
						if (ImGui::Selectable("None", View.animalZone == 0))
						{
							Submit(colony::AssignAnimalZoneCommand{View.id, 0});
						}
						for (const colony::AnimalZoneView& Zone : Snapshot.animalZones)
						{
							const FTCHARToUTF8 Label(*FString::Printf(TEXT("%s %u (%u cells)"), Zone.pen ? TEXT("Pen") : TEXT("Zone"), Zone.id, Zone.cellCount));
							if (ImGui::Selectable(Label.Get(), Zone.id == View.animalZone))
							{
								Submit(colony::AssignAnimalZoneCommand{View.id, Zone.id});
							}
						}
						ImGui::EndCombo();
					}
					// Breeding: its own switch.
					ImGui::TableNextColumn();
					bool bBreed = !View.breedingOff;
					if (ImGui::Checkbox("##breed", &bBreed))
					{
						Submit(colony::SetBreedingCommand{View.id, 0, bBreed});
					}
					// Products: young on the way, milk and wool.
					ImGui::TableNextColumn();
					FString Products;
					if (View.pregnant)
					{
						Products += TEXT("With young  ");
					}
					if (View.milkFullness >= 0.0f)
					{
						Products += FString::Printf(TEXT("Milk %d%%  "), FMath::RoundToInt32(View.milkFullness * 100.0f));
					}
					if (View.woolFullness >= 0.0f)
					{
						Products += FString::Printf(TEXT("Wool %d%%"), FMath::RoundToInt32(View.woolFullness * 100.0f));
					}
					ColonyHud::TextColored(ColonyHud::TextDim(), Products.IsEmpty() ? FString(TEXT("-")) : Products.TrimEnd());
					// Bed.
					ImGui::TableNextColumn();
					if (View.animalBed != colony::kNoEntity)
					{
						const colony::ThingView* Bed = Snapshot.find(View.animalBed);
						const FString BedName = Bed ? HudTabCapital(Sim.defs().get(Bed->def).label()) : FString(TEXT("Bed"));
						HudTabNameButton(GetWorld(), BedName, View.animalBed, 150.0f * Scale);
					}
					else
					{
						ColonyHud::TextColored(ColonyHud::TextFaint(), TEXT("Ground"));
					}
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (Rows == 0)
		{
			ColonyHud::TextColored(ColonyHud::TextDim(), bWild ? TEXT("No wild animals on the map.") : TEXT("No tame animals yet. Tame some in the Wildlife tab."));
		}
		else if (!bWild)
		{
			ColonyHud::TextColored(ColonyHud::TextFaint(), TEXT("Right click a name to rename it, mark it for slaughter or release it to the wild."));
		}
		if (!bWild)
		{
			// Zones and pens (docs/design/husbandry.md): paint them with Build > Zone > Animal zone; a zone walled or fenced in
			// all round, with gates, is a pen. Breeding can be switched off for a whole zone here.
			ImGui::Spacing();
			ColonyHud::Heading(TEXT("Zones and pens"));
			if (Snapshot.animalZones.empty())
			{
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("No animal zones yet. Paint one with Build > Zone > Animal zone, then ring it with fence and a gate to make a pen."));
			}
			for (const colony::AnimalZoneView& Zone : Snapshot.animalZones)
			{
				ImGui::PushID(static_cast<int32>(Zone.id));
				int32 Count = 0;
				for (const colony::ThingView& View : Snapshot.things)
				{
					Count += (View.category == colony::ThingCategory::Pawn && !View.dead && View.lifeStage >= 0 && View.faction == 1 && View.animalZone == Zone.id) ? 1 : 0;
				}
				ColonyHud::Text(FString::Printf(TEXT("%s %u: %u cells, %d animals"), Zone.pen ? TEXT("Pen") : TEXT("Zone"), Zone.id, Zone.cellCount, Count));
				if (!Zone.pen)
				{
					ImGui::SameLine();
					ColonyHud::TextColored(ColonyHud::TextFaint(), TEXT("(open: not fenced or walled in all round)"));
				}
				ImGui::SameLine();
				bool bBreed = !Zone.breedingOff;
				if (ImGui::Checkbox("Breeding", &bBreed))
				{
					Submit(colony::SetBreedingCommand{colony::kNoEntity, Zone.id, bBreed});
				}
				ImGui::PopID();
			}
		}
	}
	ImGui::End();
}

// ---- Mechs -------------------------------------------------------------------------------------

void UColonyHudSubsystem::DrawMechsTab(USimSubsystem& Bridge, float Scale)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	PlaceTabWindow(Scale, 620.0f * Scale);
	if (ImGui::Begin("##HudMechs", nullptr, ColonyHud::BoxFlags()))
	{
		++Stats.TabWindowFrames;
		ColonyHud::Title(TEXT("Mechs"));
		int32 Rows = 0;
		if (ImGui::BeginTable("##HudMechTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
		{
			for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
			{
				if (View.category != colony::ThingCategory::Pawn || View.dead || View.faction != 1 ||
					!Sim.defs().get(View.def).flag("race.mechanical"))
				{
					continue;
				}
				++Rows;
				ImGui::PushID(HudTabId(View.id));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				HudTabNameButton(GetWorld(), HudTabPawnName(Sim, View), View.id, 180.0f * Scale);
				ImGui::TableNextColumn();
				ColonyHud::Text(HudTabCapital(Sim.defs().get(View.def).label()));
				ImGui::TableNextColumn();
				if (View.maxHitPoints > 0)
				{
					ImGui::ProgressBar(static_cast<float>(View.hitPoints) / static_cast<float>(View.maxHitPoints), ImVec2(120.0f * Scale, 0.0f));
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (Rows == 0)
		{
			ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("The colony has no robots yet."));
		}
	}
	ImGui::End();
}

// ---- Research ----------------------------------------------------------------------------------

void UColonyHudSubsystem::DrawResearchTab(USimSubsystem& Bridge, float Scale)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::DefDatabase& Defs = Sim.defs();
	const std::vector<colony::ResearchView>& Projects = Bridge.GetCurrentSnapshot().research;
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	PlaceTabWindow(Scale, Viewport->Size.x - 40.0f * Scale, 0.78f);
	if (!ImGui::Begin("##HudResearch", nullptr, ColonyHud::BoxFlags()))
	{
		ImGui::End();
		return;
	}
	++Stats.TabWindowFrames;

	// Where each project sits in the tree: its column is the longest chain of prerequisites before it.
	std::map<std::string, int32> Depth;
	std::map<std::string, const colony::ResearchView*> ByName;
	for (const colony::ResearchView& Project : Projects)
	{
		ByName[Defs.get(Project.project).defName()] = &Project;
	}
	for (int32 Pass = 0; Pass < 16; ++Pass)
	{
		bool bChanged = false;
		for (const colony::ResearchView& Project : Projects)
		{
			const colony::Def& Def = Defs.get(Project.project);
			int32 D = 0;
			for (size_t Index = 0; Index < Def.count("prerequisites"); ++Index)
			{
				const auto Found = Depth.find(Def.textAt("prerequisites", Index));
				D = FMath::Max(D, Found == Depth.end() ? 1 : Found->second + 1);
			}
			int32& Slot = Depth[Def.defName()];
			if (Slot != D)
			{
				Slot = D;
				bChanged = true;
			}
		}
		if (!bChanged)
		{
			break;
		}
	}

	// The project under way, as a bar along the top.
	const colony::ResearchView* Current = nullptr;
	for (const colony::ResearchView& Project : Projects)
	{
		Current = Project.current ? &Project : Current;
	}
	ColonyHud::Title(TEXT("Research"));
	ImGui::SameLine(0.0f, 24.0f * Scale);
	if (Current)
	{
		const FTCHARToUTF8 Overlay(*FString::Printf(TEXT("%s   %.0f / %.0f"), *HudTabCapital(Defs.get(Current->project).label()), Current->progress, Current->cost));
		ImGui::ProgressBar(Current->cost > 0.0 ? static_cast<float>(Current->progress / Current->cost) : 0.0f, ImVec2(420.0f * Scale, 0.0f), Overlay.Get());
	}
	else
	{
		ColonyHud::TextColored(ColonyHud::Warn(), TEXT("Nothing is being researched. Pick a project in the tree."));
	}
	ImGui::Separator();

	// Left: the picked project. Right: the tree.
	const float InfoWidth = 320.0f * Scale;
	const float BodyHeight = Viewport->Size.y * 0.6f;
	if (ImGui::BeginChild("##ResearchInfo", ImVec2(InfoWidth, BodyHeight), ImGuiChildFlags_Borders))
	{
		const auto Picked = ByName.find(std::string(TCHAR_TO_UTF8(*ResearchPicked)));
		if (Picked == ByName.end())
		{
			ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Click a project to read about it."));
		}
		else
		{
			const colony::ResearchView& Project = *Picked->second;
			const colony::Def& Def = Defs.get(Project.project);
			ColonyHud::Title(HudTabCapital(Def.label()));
			ColonyHud::TextColored(ColonyHud::TextDim(), FString::Printf(TEXT("%s, %.0f points"), UTF8_TO_TCHAR(Def.text("techLevel").c_str()), Project.cost));
			const FTCHARToUTF8 Overlay(*FString::Printf(TEXT("%.0f / %.0f"), Project.progress, Project.cost));
			ImGui::ProgressBar(Project.cost > 0.0 ? static_cast<float>(Project.progress / Project.cost) : 0.0f, ImVec2(-1.0f, 0.0f), Overlay.Get());
			const std::string About = Def.text("description");
			if (!About.empty())
			{
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextUnformatted(About.c_str());
				ImGui::PopTextWrapPos();
			}
			if (Def.count("prerequisites") > 0)
			{
				ColonyHud::Heading(TEXT("Needs"));
				for (size_t Index = 0; Index < Def.count("prerequisites"); ++Index)
				{
					const std::string Name = Def.textAt("prerequisites", Index);
					const auto Found = ByName.find(Name);
					const bool bDone = Found != ByName.end() && Found->second->done;
					const colony::Def* Need = Defs.find("Research", Name);
					ColonyHud::TextColored(bDone ? ColonyHud::Good() : ColonyHud::Bad(), FString::Printf(TEXT("%s %s"), bDone ? TEXT("done:") : TEXT("not yet:"),
						Need ? *HudTabCapital(Need->label()) : UTF8_TO_TCHAR(Name.c_str())));
				}
			}
			ColonyHud::Heading(TEXT("Unlocks"));
			bool bAny = false;
			for (const char* Kind : {"Things", "Recipes", "Terrain"})
			{
				const std::string Path = std::string("unlocks.") + Kind;
				for (size_t Index = 0; Index < Def.count(Path.c_str()); ++Index)
				{
					const std::string Name = Def.textAt(Path.c_str(), Index);
					const colony::Def* Unlocked = Defs.find(Kind, Name);
					ColonyHud::Text(Unlocked ? HudTabCapital(Unlocked->label()) : FString(UTF8_TO_TCHAR(Name.c_str())));
					bAny = true;
				}
			}
			if (!bAny)
			{
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Nothing on its own; it leads to other projects."));
			}
			ImGui::Spacing();
			if (Project.done)
			{
				ColonyHud::TextColored(ColonyHud::Good(), TEXT("Finished."));
			}
			else if (Project.current)
			{
				if (ColonyHud::Button(TEXT("Stop researching"), true))
				{
					Submit(colony::SetResearchCommand{std::string()});
				}
			}
			else
			{
				ImGui::BeginDisabled(!Project.available);
				ColonyHud::PushLarge();
				if (ColonyHud::Button(TEXT("Research this")))
				{
					Submit(colony::SetResearchCommand{Def.defName()});
				}
				ColonyHud::PopLarge();
				ImGui::EndDisabled();
				if (!Project.available)
				{
					ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Finish what it needs first."));
				}
			}
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();
	if (ImGui::BeginChild("##ResearchTree", ImVec2(0.0f, BodyHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
	{
		const float NodeW = 190.0f * Scale;
		const float NodeH = ImGui::GetFrameHeight() * 1.6f;
		const float GapX = 70.0f * Scale;
		const float GapY = 10.0f * Scale;
		const ImVec2 Origin = ImGui::GetCursorScreenPos();
		std::map<int32, int32> RowsInColumn;
		std::map<std::string, ImVec2> Where;
		// Columns by depth, rows in def order within each column.
		for (const colony::ResearchView& Project : Projects)
		{
			const std::string Name = Defs.get(Project.project).defName();
			const int32 Column = Depth[Name];
			const int32 Row = RowsInColumn[Column]++;
			Where[Name] = ImVec2(Origin.x + Column * (NodeW + GapX), Origin.y + Row * (NodeH + GapY));
		}
		// Lines from each prerequisite to the project that needs it.
		ImDrawList* Draw = ImGui::GetWindowDrawList();
		for (const colony::ResearchView& Project : Projects)
		{
			const colony::Def& Def = Defs.get(Project.project);
			const ImVec2 To = Where[Def.defName()];
			for (size_t Index = 0; Index < Def.count("prerequisites"); ++Index)
			{
				const auto From = Where.find(Def.textAt("prerequisites", Index));
				if (From == Where.end())
				{
					continue;
				}
				const auto Need = ByName.find(Def.textAt("prerequisites", Index));
				const ImU32 Color = ImGui::GetColorU32(Need != ByName.end() && Need->second->done ? ColonyHud::Good() : ColonyHud::ButtonBorder());
				const ImVec2 A(From->second.x + NodeW, From->second.y + NodeH * 0.5f);
				const ImVec2 B(To.x, To.y + NodeH * 0.5f);
				Draw->AddBezierCubic(A, ImVec2(A.x + GapX * 0.5f, A.y), ImVec2(B.x - GapX * 0.5f, B.y), B, Color, 1.5f * Scale);
			}
		}
		// The nodes.
		for (const colony::ResearchView& Project : Projects)
		{
			const colony::Def& Def = Defs.get(Project.project);
			const FString Name = UTF8_TO_TCHAR(Def.defName().c_str());
			ImGui::SetCursorScreenPos(Where[Def.defName()]);
			ImGui::PushID(static_cast<int32>(Project.project.value));
			const ImVec4 Fill = Project.done ? ColonyHud::Rgb(0x1f2a1f) : (Project.current ? ColonyHud::AccentDark() : (Project.available ? ColonyHud::ButtonBg() : ColonyHud::Rgb(0x15181b)));
			ImGui::PushStyleColor(ImGuiCol_Button, Fill);
			ImGui::PushStyleColor(ImGuiCol_Border, ResearchPicked == Name ? ColonyHud::Accent() : ColonyHud::ButtonBorder());
			ImGui::PushStyleColor(ImGuiCol_Text, Project.available || Project.done || Project.current ? ColonyHud::TextMain() : ColonyHud::TextFaint());
			const FTCHARToUTF8 Label(*FString::Printf(TEXT("%s\n%.0f"), *HudTabCapital(Def.label()), Project.cost));
			if (ImGui::Button(Label.Get(), ImVec2(NodeW, NodeH)))
			{
				ResearchPicked = Name;
			}
			ImGui::PopStyleColor(3);
			if (!Project.done && Project.progress > 0.0 && Project.cost > 0.0)
			{
				const ImVec2 Min = ImGui::GetItemRectMin();
				const ImVec2 Max = ImGui::GetItemRectMax();
				Draw->AddRectFilled(ImVec2(Min.x + 2.0f, Max.y - 4.0f * Scale), ImVec2(Min.x + 2.0f + (Max.x - Min.x - 4.0f) * static_cast<float>(Project.progress / Project.cost), Max.y - 2.0f),
					ImGui::GetColorU32(ColonyHud::Accent()));
			}
			ImGui::PopID();
		}
		ImGui::Dummy(ImVec2(0.0f, 0.0f));
	}
	ImGui::EndChild();
	ImGui::End();
}

// ---- Inventory ---------------------------------------------------------------------------------

void UColonyHudSubsystem::DrawInventoryTab(USimSubsystem& Bridge, float Scale)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	PlaceTabWindow(Scale, 720.0f * Scale);
	if (!ImGui::Begin("##HudInventory", nullptr, ColonyHud::BoxFlags()))
	{
		ImGui::End();
		return;
	}
	++Stats.TabWindowFrames;
	ColonyHud::Title(TEXT("Inventory"));
	ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Everything the colony has lying on the map. A star pins it to the top left of the screen."));
	ImGui::SetNextItemWidth(260.0f * Scale);
	ImGui::InputTextWithHint("##InvSearch", "Search", InventorySearch, sizeof(InventorySearch));

	// Add up the stacks by kind of thing.
	struct FLine
	{
		colony::DefId Def;
		FString Label;
		FString DefName;
		FString Group;
		int32 Count = 0;
		double Mass = 0.0;
	};
	TMap<uint32, FLine> Lines;
	for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
	{
		if (View.category != colony::ThingCategory::Item)
		{
			continue;
		}
		FLine& Line = Lines.FindOrAdd(View.def.value);
		if (!Line.Def.valid())
		{
			const colony::Def& Def = Sim.defs().get(View.def);
			Line.Def = View.def;
			Line.Label = HudTabCapital(Def.label());
			Line.DefName = UTF8_TO_TCHAR(Def.defName().c_str());
			Line.Group = Def.count("thingCategories") > 0 ? HudTabCapital(Def.textAt("thingCategories", 0)) : FString(TEXT("Other"));
		}
		Line.Count += FMath::Max(View.stackCount, 1);
		Line.Mass += Sim.defs().get(View.def).number("stats.mass", 0.0) * FMath::Max(View.stackCount, 1);
	}
	TArray<FLine> Sorted;
	const FString Search = UTF8_TO_TCHAR(InventorySearch);
	for (const TPair<uint32, FLine>& Pair : Lines)
	{
		if (Search.IsEmpty() || Pair.Value.Label.Contains(Search))
		{
			Sorted.Add(Pair.Value);
		}
	}
	// Pinned first, then by group and name.
	Sorted.Sort([this](const FLine& A, const FLine& B)
	{
		const bool PinA = IsPinned(A.DefName);
		const bool PinB = IsPinned(B.DefName);
		if (PinA != PinB)
		{
			return PinA;
		}
		return A.Group != B.Group ? A.Group < B.Group : A.Label < B.Label;
	});
	if (ImGui::BeginTable("##HudInvTable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
		ImVec2(0.0f, ImGui::GetMainViewport()->Size.y * 0.5f)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("pin", ImGuiTableColumnFlags_WidthFixed, 34.0f * Scale);
		ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("count", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("mass", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		HudTabHeader(TEXT(""), TEXT("Pinned things show at the top left of the screen"));
		HudTabHeader(TEXT("Thing"), TEXT("What it is"));
		HudTabHeader(TEXT("How many"), TEXT("On the map, in stockpiles or lying about"));
		HudTabHeader(TEXT("Weight"), TEXT("All of it together"));
		FString LastGroup;
		for (const FLine& Line : Sorted)
		{
			const bool bPinned = IsPinned(Line.DefName);
			if (!bPinned && Line.Group != LastGroup)
			{
				LastGroup = Line.Group;
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TableNextColumn();
				ColonyHud::TextColored(ColonyHud::Accent(), Line.Group);
			}
			ImGui::PushID(static_cast<int32>(Line.Def.value));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushStyleColor(ImGuiCol_Text, bPinned ? ColonyHud::Accent() : ColonyHud::TextFaint());
			if (ImGui::SmallButton(bPinned ? "*" : "+"))
			{
				TogglePin(Line.DefName);
			}
			ImGui::PopStyleColor();
			ColonyHud::Tip(bPinned ? TEXT("Unpin from the top left") : TEXT("Pin to the top left"));
			ImGui::TableNextColumn();
			ColonyHud::Text(Line.Label);
			ImGui::TableNextColumn();
			ColonyHud::Text(FString::FromInt(Line.Count));
			ImGui::TableNextColumn();
			ColonyHud::TextColored(ColonyHud::TextDim(), FString::Printf(TEXT("%.1f kg"), Line.Mass));
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (Sorted.IsEmpty())
	{
		ColonyHud::TextColored(ColonyHud::TextDim(), Search.IsEmpty() ? TEXT("Nothing lying on the map.") : TEXT("Nothing by that name."));
	}
	ImGui::End();
}
