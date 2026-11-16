#include "InventoryPanelSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"

#include <imgui.h>

#include "OverseerSubsystem.h"
#include "ProtagonistSubsystem.h"
#include "SimPicking.h"
#include "SimSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
	TAutoConsoleVariable<int32> GInventoryShowUi(
		TEXT("Inventory.UI"), 1, TEXT("1 lets I open a pawn's inventory window, 0 hides it."), ECVF_Default);

	FAutoConsoleCommandWithWorld GInventoryOpenCommand(
		TEXT("Inventory.Open"), TEXT("Opens or closes the inventory window of the steered or selected pawn."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (UInventoryPanelSubsystem* Panel = World ? World->GetSubsystem<UInventoryPanelSubsystem>() : nullptr)
			{
				Panel->SetOpen(!Panel->IsOpen());
			}
		}));

	const ImVec4 InvGrey(0.7f, 0.7f, 0.7f, 1.0f);
	const ImVec4 InvGood(0.55f, 0.85f, 0.55f, 1.0f);
	const ImVec4 InvWarm(0.95f, 0.65f, 0.30f, 1.0f);
	const ImVec4 InvBad(0.95f, 0.45f, 0.35f, 1.0f);

	/** Within this many metres a lying item shows under Nearby (the sim's reach for Take is 2 m). */
	constexpr float InvReachMetres = 2.0f;
	constexpr int32 InvBagColumns = 8;

	void InvText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}

	void InvTextColored(const ImVec4& Color, const FString& Text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Color);
		InvText(Text);
		ImGui::PopStyleColor();
	}

	FString InvLabel(const colony::Sim& Sim, colony::DefId Def)
	{
		if (!Def.valid())
		{
			return FString(TEXT("nothing"));
		}
		FString Out = UTF8_TO_TCHAR(Sim.defs().get(Def).label().c_str());
		if (Out.Len() > 0)
		{
			Out[0] = FChar::ToUpper(Out[0]);
		}
		return Out;
	}

	double InvMass(const colony::Sim& Sim, colony::DefId Def)
	{
		return Def.valid() ? Sim.defs().get(Def).number("stats.mass", 1.0) : 0.0;
	}

	bool InvHas(const std::vector<std::string>& List, const char* Name)
	{
		return std::find(List.begin(), List.end(), Name) != List.end();
	}

	/** The body slot a piece of apparel shows under, from its body part groups and layers (RimWorld's names). */
	const TCHAR* InvSlotOf(const colony::Sim& Sim, colony::DefId Def)
	{
		const colony::ApparelStats& Stats = Sim.apparel().stats();
		const std::vector<std::string>& Groups = Stats.groups(Def);
		const std::vector<std::string>& Layers = Stats.layers(Def);
		if (InvHas(Layers, "Belt") || InvHas(Groups, "Neck") || InvHas(Groups, "Waist"))
		{
			return TEXT("Accessories");
		}
		if (InvHas(Groups, "FullHead") || InvHas(Groups, "UpperHead") || InvHas(Groups, "Eyes") || InvHas(Layers, "Overhead"))
		{
			return TEXT("Head");
		}
		if (InvHas(Groups, "Torso") || InvHas(Groups, "Shoulders") || InvHas(Groups, "Arms"))
		{
			return TEXT("Torso");
		}
		if (InvHas(Groups, "Legs"))
		{
			return TEXT("Legs");
		}
		if (InvHas(Groups, "Feet"))
		{
			return TEXT("Feet");
		}
		if (InvHas(Groups, "Hands"))
		{
			return TEXT("Hands");
		}
		return TEXT("Accessories");
	}
}

// ---- Setup -------------------------------------------------------------------------------------

bool UInventoryPanelSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UInventoryPanelSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UInventoryPanelSubsystem, STATGROUP_Tickables);
}

USimSubsystem* UInventoryPanelSubsystem::GetBridge() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<USimSubsystem>() : nullptr;
}

colony::Entity UInventoryPanelSubsystem::GetShownPawn() const
{
	UWorld* World = GetWorld();
	const USimSubsystem* Bridge = GetBridge();
	if (!World || !Bridge || !Bridge->GetSim())
	{
		return colony::kNoEntity;
	}
	const UProtagonistSubsystem* Protagonist = World->GetSubsystem<UProtagonistSubsystem>();
	if (Protagonist && Protagonist->IsDirectMode() && Protagonist->GetControlled() != colony::kNoEntity)
	{
		return Protagonist->GetControlled();
	}
	const UOverseerSubsystem* Overseer = World->GetSubsystem<UOverseerSubsystem>();
	if (Overseer && Overseer->IsOverseerActive() && Overseer->GetSelection().Kind == ESimPickKind::Thing)
	{
		const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Overseer->GetSelection().Thing);
		if (View && View->category == colony::ThingCategory::Pawn && !View->dead)
		{
			return View->id;
		}
	}
	return colony::kNoEntity;
}

void UInventoryPanelSubsystem::Submit(colony::Command Command)
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

bool UInventoryPanelSubsystem::Take(colony::Entity Item, int32 Count)
{
	const colony::Entity Pawn = GetShownPawn();
	if (Pawn == colony::kNoEntity)
	{
		return false;
	}
	Submit(colony::InventoryCommand{Pawn, "Take", Item, Count});
	return true;
}

bool UInventoryPanelSubsystem::Drop(colony::Entity Item, int32 Count)
{
	const colony::Entity Pawn = GetShownPawn();
	if (Pawn == colony::kNoEntity)
	{
		return false;
	}
	Submit(colony::InventoryCommand{Pawn, "Drop", Item, Count});
	return true;
}

bool UInventoryPanelSubsystem::DropAll()
{
	const colony::Entity Pawn = GetShownPawn();
	if (Pawn == colony::kNoEntity)
	{
		return false;
	}
	Submit(colony::InventoryCommand{Pawn, "DropAll", colony::kNoEntity, 0});
	return true;
}

// ---- The tick and the window -------------------------------------------------------------------

void UInventoryPanelSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim() || GInventoryShowUi.GetValueOnGameThread() == 0)
	{
		return;
	}
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (PC->WasInputKeyJustPressed(EKeys::I))
		{
			bOpen = !bOpen;
		}
	}
	if (!bOpen)
	{
		return;
	}
	const colony::Entity Pawn = GetShownPawn();
	if (Pawn == colony::kNoEntity)
	{
		return;  // stays open, and shows again once a pawn is steered or selected
	}
	DrawWindow(*Bridge, Pawn);
}

void UInventoryPanelSubsystem::DrawWindow(USimSubsystem& Bridge, colony::Entity Pawn)
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

	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Snapshot& Snap = Bridge.GetCurrentSnapshot();
	const colony::ThingView* View = Snap.find(Pawn);
	if (!View)
	{
		return;
	}

	const float Width = 720.0f * Scale;
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.5f),
		ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(Width, 0.0f), ImVec2(Width, Viewport->Size.y * 0.85f));
	const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	if (!ImGui::Begin("##InventoryWindow", nullptr, Flags))
	{
		ImGui::End();
		return;
	}
	++Stats.WindowFrames;
	Stats.WornRows = 0;
	Stats.WeaponRows = 0;
	Stats.BagCells = 0;

	// Title: whose inventory, and Close.
	FString Name = InvLabel(Sim, View->def);
	if (const colony::Colonist* Colonist = Sim.colonistOf(Pawn))
	{
		Name = UTF8_TO_TCHAR((Colonist->nickName.empty() ? Colonist->firstName : Colonist->nickName).c_str());
	}
	InvText(FString::Printf(TEXT("Inventory: %s"), *Name));
	ImGui::SameLine(Width - 90.0f * Scale);
	if (ImGui::SmallButton("Close (I)"))
	{
		bOpen = false;
		ImGui::End();
		return;
	}

	// The load bar.
	const float Capacity = FMath::Max(View->capacityKg, 0.001f);
	const float Share = View->loadKg / Capacity;
	const ImVec4 BarColor = Share > 1.0f ? InvBad : (Share > 0.2f ? InvWarm : InvGood);
	ImGui::PushStyleColor(ImGuiCol_PlotHistogram, BarColor);
	const FTCHARToUTF8 LoadText(*FString::Printf(TEXT("%.1f of %.0f kg"), View->loadKg, View->capacityKg));
	ImGui::ProgressBar(FMath::Clamp(Share, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), LoadText.Get());
	ImGui::PopStyleColor();
	if (Share > 1.0f)
	{
		InvTextColored(InvBad, TEXT("Overloaded: walking much slower. Drop something."));
	}
	else if (Share > 0.2f)
	{
		InvTextColored(InvGrey, FString::Printf(TEXT("Walking at %d%% speed."), FMath::RoundToInt(100.0f * (1.0f - 0.25f * (Share - 0.2f) / 0.8f))));
	}

	if (ImGui::BeginTable("##InvColumns", 2, ImGuiTableFlags_SizingStretchSame))
	{
		ImGui::TableNextColumn();
		// Worn, by body slot.
		ImGui::SeparatorText("Worn");
		const TCHAR* Slots[] = {TEXT("Head"), TEXT("Torso"), TEXT("Legs"), TEXT("Feet"), TEXT("Hands"), TEXT("Accessories")};
		bool bAnyWorn = false;
		for (const TCHAR* Slot : Slots)
		{
			bool bHeader = false;
			for (uint32 i = 0; i < View->apparelCount; ++i)
			{
				const colony::ApparelView& Piece = Snap.apparel[View->firstApparel + i];
				if (FCString::Strcmp(InvSlotOf(Sim, Piece.def), Slot) != 0)
				{
					continue;
				}
				if (!bHeader)
				{
					InvTextColored(InvGrey, Slot);
					bHeader = true;
				}
				bAnyWorn = true;
				++Stats.WornRows;
				const ImVec4 Color = Piece.durability <= 0.0f ? InvBad : (Piece.durability < 0.5f ? InvWarm : ImVec4(1, 1, 1, 1));
				InvTextColored(Color, FString::Printf(TEXT("  %s  %d%%  %.1f kg%s"), *InvLabel(Sim, Piece.def),
					FMath::RoundToInt(100.0f * Piece.durability), InvMass(Sim, Piece.def), Piece.durability <= 0.0f ? TEXT("  (tattered)") : TEXT("")));
			}
		}
		if (!bAnyWorn)
		{
			InvTextColored(InvGrey, TEXT("Nothing worn."));
		}

		// Weapons, by slot.
		ImGui::SeparatorText("Weapons");
		const TCHAR* WeaponSlots[] = {TEXT("Primary"), TEXT("Sidearm"), TEXT("Melee")};
		for (int32 Slot = 0; Slot < 3; ++Slot)
		{
			const colony::DefId Def = View->carried[Slot];
			if (!Def.valid())
			{
				continue;
			}
			++Stats.WeaponRows;
			const bool bInHands = Def == View->weapon;
			InvText(FString::Printf(TEXT("%s: %s  %.1f kg%s"), WeaponSlots[Slot], *InvLabel(Sim, Def), InvMass(Sim, Def),
				bInHands ? TEXT("  (in hands)") : TEXT("")));
		}
		if (Stats.WeaponRows == 0)
		{
			InvTextColored(InvGrey, TEXT("No weapons."));
		}

		ImGui::TableNextColumn();
		// The bag: a big grid; a click drops the stack.
		ImGui::SeparatorText("Bag");
		const float Cell = 72.0f * Scale;
		for (uint32 i = 0; i < View->inventoryCount; ++i)
		{
			const colony::InventoryItemView& Item = Snap.inventory[View->firstInventory + i];
			if (i % InvBagColumns != 0)
			{
				ImGui::SameLine();
			}
			ImGui::PushID(static_cast<int>(i));
			FString Short = InvLabel(Sim, Item.def).Left(8);
			const FTCHARToUTF8 CellText(*FString::Printf(TEXT("%s\nx%d"), *Short, Item.count));
			if (ImGui::Button(CellText.Get(), ImVec2(Cell, Cell)))
			{
				Drop(Item.item, 0);
			}
			if (ImGui::IsItemHovered())
			{
				const FTCHARToUTF8 Tip(*FString::Printf(TEXT("%s x%d, %.2f kg\nClick to drop"), *InvLabel(Sim, Item.def), Item.count, Item.massKg));
				ImGui::SetTooltip("%s", Tip.Get());
			}
			ImGui::PopID();
			++Stats.BagCells;
		}
		// Empty cells to the end of the row and one more, so the grid reads as room to fill.
		const int32 Filled = static_cast<int32>(View->inventoryCount);
		const int32 Shown = (Filled / InvBagColumns + 2) * InvBagColumns;
		ImGui::BeginDisabled();
		for (int32 i = Filled; i < Shown; ++i)
		{
			if (i % InvBagColumns != 0)
			{
				ImGui::SameLine();
			}
			ImGui::PushID(1000 + i);
			ImGui::Button("##empty", ImVec2(Cell, Cell));
			ImGui::PopID();
		}
		ImGui::EndDisabled();
		if (View->inventoryCount > 0 && ImGui::Button("Drop all"))
		{
			DropAll();
		}

		// Nearby: items lying within reach, a click takes them.
		ImGui::SeparatorText("Nearby");
		int32 NearbyRows = 0;
		for (const colony::ThingView& Thing : Snap.things)
		{
			if (Thing.category != colony::ThingCategory::Item || Thing.level != View->level)
			{
				continue;
			}
			const float Dx = Thing.x - View->x;
			const float Dy = Thing.y - View->y;
			if (Dx * Dx + Dy * Dy > InvReachMetres * InvReachMetres)
			{
				continue;
			}
			ImGui::PushID(static_cast<int>(entt::to_integral(Thing.id)));
			const FTCHARToUTF8 Row(*FString::Printf(TEXT("Take %s x%d (%.1f kg)"), *InvLabel(Sim, Thing.def), FMath::Max(Thing.stackCount, 1),
				InvMass(Sim, Thing.def) * FMath::Max(Thing.stackCount, 1)));
			if (ImGui::SmallButton(Row.Get()))
			{
				Take(Thing.id, 0);
			}
			ImGui::PopID();
			if (++NearbyRows >= 12)
			{
				break;
			}
		}
		if (NearbyRows == 0)
		{
			InvTextColored(InvGrey, TEXT("Nothing within reach."));
		}
		ImGui::EndTable();
	}
	ImGui::End();
}
