#include "WorldScreenSubsystem.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#include <imgui.h>

#include "OverseerSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
	FAutoConsoleCommandWithWorldAndArgs GWorldScreenCommand(
		TEXT("World.Screen"), TEXT("World.Screen 0|1 closes or opens the world map screen (no argument toggles it)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UWorldScreenSubsystem* Screen = World ? World->GetSubsystem<UWorldScreenSubsystem>() : nullptr)
			{
				Screen->SetOpen(Args.Num() > 0 ? FCString::Atoi(*Args[0]) != 0 : !Screen->IsOpen());
			}
		}));

	std::string Lower(const std::string& Text)
	{
		std::string Out;
		for (const char C : Text)
		{
			if (C != ' ' && C != '_')
			{
				Out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(C))));
			}
		}
		return Out;
	}

	bool Has(const std::string& Name, const char* Part)
	{
		return Name.find(Part) != std::string::npos;
	}

	/** Base colour of a biome (r, g, b in 0..255). Unknown biomes get a grey. */
	void BiomeRgb(const std::string& DefName, int& R, int& G, int& B)
	{
		const std::string N = Lower(DefName);
		if (Has(N, "seaice")) { R = 200; G = 225; B = 240; }
		else if (Has(N, "icesheet")) { R = 238; G = 244; B = 250; }
		else if (Has(N, "tundra")) { R = 150; G = 160; B = 130; }
		else if (Has(N, "boreal")) { R = 62; G = 110; B = 80; }
		else if (Has(N, "coldbog")) { R = 98; G = 118; B = 100; }
		else if (Has(N, "temperateforest")) { R = 70; G = 140; B = 62; }
		else if (Has(N, "temperateswamp")) { R = 88; G = 120; B = 60; }
		else if (Has(N, "tropicalrain")) { R = 20; G = 120; B = 50; }
		else if (Has(N, "tropicalswamp")) { R = 40; G = 100; B = 80; }
		else if (Has(N, "extremedesert")) { R = 214; G = 178; B = 110; }
		else if (Has(N, "desert")) { R = 218; G = 190; B = 120; }
		else if (Has(N, "aridshrub")) { R = 170; G = 160; B = 90; }
		else { R = 140; G = 140; B = 140; }
	}

	float HillFactor(colony::Hilliness H)
	{
		switch (H)
		{
		case colony::Hilliness::Flat: return 1.0f;
		case colony::Hilliness::SmallHills: return 0.85f;
		case colony::Hilliness::LargeHills: return 0.7f;
		case colony::Hilliness::Mountainous: return 0.55f;
		default: return 0.35f;
		}
	}

	ImU32 TileColor(const colony::World& W, const colony::WorldTile& T)
	{
		int R = 0, G = 0, B = 0;
		if (T.biome == colony::kNoBiome)
		{
			// Open water: darker the deeper it is.
			const float Depth = FMath::Clamp(-static_cast<float>(T.elevation) / 4000.0f, 0.0f, 1.0f);
			R = static_cast<int>(FMath::Lerp(60.0f, 12.0f, Depth));
			G = static_cast<int>(FMath::Lerp(120.0f, 40.0f, Depth));
			B = static_cast<int>(FMath::Lerp(190.0f, 100.0f, Depth));
		}
		else
		{
			BiomeRgb(W.biomeName(T), R, G, B);
			if (!T.water)
			{
				const float F = HillFactor(T.hilliness);
				R = static_cast<int>(R * F);
				G = static_cast<int>(G * F);
				B = static_cast<int>(B * F);
			}
		}
		return IM_COL32(FMath::Clamp(R, 0, 255), FMath::Clamp(G, 0, 255), FMath::Clamp(B, 0, 255), 255);
	}

	ImU32 RoadColor(colony::RoadKind Kind)
	{
		switch (Kind)
		{
		case colony::RoadKind::Dirt: return IM_COL32(150, 100, 50, 255);
		case colony::RoadKind::Stone: return IM_COL32(90, 70, 50, 255);
		default: return IM_COL32(30, 25, 25, 255);
		}
	}

	FString Words(const std::string& DefName)
	{
		// "TemperateForest" -> "temperate forest".
		FString Out;
		for (const char C : DefName)
		{
			if (std::isupper(static_cast<unsigned char>(C)) && !Out.IsEmpty())
			{
				Out.AppendChar(TEXT(' '));
			}
			Out.AppendChar(static_cast<TCHAR>(std::tolower(static_cast<unsigned char>(C))));
		}
		return Out;
	}

	FString HillsWords(colony::Hilliness H)
	{
		switch (H)
		{
		case colony::Hilliness::Flat: return TEXT("flat");
		case colony::Hilliness::SmallHills: return TEXT("small hills");
		case colony::Hilliness::LargeHills: return TEXT("large hills");
		case colony::Hilliness::Mountainous: return TEXT("mountainous");
		default: return TEXT("impassable mountains");
		}
	}

	void WText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}
}

void UWorldScreenSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<USimSubsystem>();
}

bool UWorldScreenSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UWorldScreenSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWorldScreenSubsystem, STATGROUP_Tickables);
}

USimSubsystem* UWorldScreenSubsystem::GetBridge() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<USimSubsystem>() : nullptr;
}

void UWorldScreenSubsystem::SetOpen(bool bNewOpen)
{
	bOpen = bNewOpen;
}

const colony::World* UWorldScreenSubsystem::GetShownWorld() const
{
	const USimSubsystem* Bridge = GetBridge();
	const colony::World* Shown = nullptr;
	if (!bPreview && Bridge && Bridge->GetSim())
	{
		Shown = Bridge->GetSim()->world();
	}
	else if (Preview)
	{
		Shown = &*Preview;
	}
	return Shown && !Shown->empty() ? Shown : nullptr;
}

void UWorldScreenSubsystem::MakeWorld(int32 NewSeed)
{
	const USimSubsystem* Bridge = GetBridge();
	Selected = -1;
	Seed = NewSeed;
	SeedInput = NewSeed;
	bPreview = true;
	Preview.reset();
	if (Bridge && Bridge->GetDefs())
	{
		Preview = colony::World::generate(*Bridge->GetDefs(), static_cast<uint64>(NewSeed));
	}
}

bool UWorldScreenSubsystem::SelectTile(int32 Tile)
{
	const colony::World* World = GetShownWorld();
	if (!World || Tile < 0 || Tile >= World->tileCount())
	{
		return false;
	}
	Selected = Tile;
	++Stats.TilesPicked;
	return true;
}

int32 UWorldScreenSubsystem::TileAt(const colony::World& World, float MapLeft, float MapTop, float TileSize, float X, float Y)
{
	if (TileSize <= 0.0f)
	{
		return -1;
	}
	const int32 TX = static_cast<int32>(std::floor((X - MapLeft) / TileSize));
	const int32 TY = static_cast<int32>(std::floor((Y - MapTop) / TileSize));
	if (TX < 0 || TY < 0 || TX >= World.width() || TY >= World.height())
	{
		return -1;
	}
	return World.index(TX, TY);
}

FString UWorldScreenSubsystem::WhyNotStart(int32 Tile) const
{
	const colony::World* World = GetShownWorld();
	if (!World || Tile < 0 || Tile >= World->tileCount())
	{
		return TEXT("No tile is picked.");
	}
	if (World->canStartAt(Tile))
	{
		return FString();
	}
	const colony::WorldTile& T = World->tile(Tile);
	if (T.water || T.biome == colony::kNoBiome)
	{
		return TEXT("A colony can't start on water.");
	}
	if (T.hilliness == colony::Hilliness::Impassable)
	{
		return TEXT("Impassable mountains.");
	}
	if (T.settlement != colony::kNoSettlement)
	{
		return TEXT("A settlement is already here.");
	}
	return TEXT("Too steep for a colony.");
}

TArray<FString> UWorldScreenSubsystem::DescribeTile(int32 Tile) const
{
	TArray<FString> Lines;
	const colony::World* World = GetShownWorld();
	if (!World || Tile < 0 || Tile >= World->tileCount())
	{
		return Lines;
	}
	const colony::WorldTile& T = World->tile(Tile);
	Lines.Add(FString::Printf(TEXT("Tile %d (%d, %d)"), Tile, World->tileX(Tile), World->tileY(Tile)));
	if (T.biome == colony::kNoBiome)
	{
		Lines.Add(TEXT("Biome: open water"));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("Biome: %s%s"), *Words(World->biomeName(T)), T.water ? TEXT(" (frozen water)") : TEXT("")));
	}
	if (!T.water)
	{
		Lines.Add(FString::Printf(TEXT("Terrain: %s"), *HillsWords(T.hilliness)));
	}
	Lines.Add(FString::Printf(TEXT("Elevation: %d m"), static_cast<int32>(T.elevation)));
	Lines.Add(FString::Printf(TEXT("Temperature: %.1f C a year"), T.temperature));
	Lines.Add(FString::Printf(TEXT("Rainfall: %.0f mm a year"), T.rainfall));
	if (T.river != colony::RiverSize::None)
	{
		Lines.Add(FString::Printf(TEXT("River: %s"), UTF8_TO_TCHAR(colony::riverSizeName(T.river))));
	}
	if (T.road != colony::RoadKind::None)
	{
		Lines.Add(FString::Printf(TEXT("Road: %s"), UTF8_TO_TCHAR(colony::roadKindName(T.road))));
	}
	if (T.settlement != colony::kNoSettlement)
	{
		const colony::WorldSettlement& S = World->settlements()[static_cast<size_t>(T.settlement)];
		Lines.Add(FString::Printf(TEXT("Settlement: %s (%s)"), UTF8_TO_TCHAR(S.name.c_str()), *Words(S.faction)));
	}
	return Lines;
}

bool UWorldScreenSubsystem::StartHere()
{
	USimSubsystem* Bridge = GetBridge();
	const colony::World* World = GetShownWorld();
	if (!Bridge || !World || Selected < 0 || !World->canStartAt(Selected))
	{
		return false;
	}
	FSimStartOptions Options = Bridge->GetStartOptions();
	Options.bWorld = true;
	Options.WorldSeed = World->seed();
	Options.WorldWidth = World->width();
	Options.WorldHeight = World->height();
	Options.WorldStartTile = Selected;
	if (Options.Scenario.IsEmpty())
	{
		Options.Scenario = TEXT("Crashlanded");
	}
	const int32 Wanted = Selected;
	Bridge->StartSim(Options);
	const colony::World* Made = Bridge->GetSim() ? Bridge->GetSim()->world() : nullptr;
	if (!Made || Made->startTile() != Wanted)
	{
		return false;
	}
	// The sim's world is now the one on screen; the picked tile stays so the colony's tile is marked.
	bPreview = false;
	Preview.reset();
	SimGeneration = Bridge->GetSimGeneration();
	bOpen = false;
	++Stats.ColoniesStarted;
	return true;
}

void UWorldScreenSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	if (SimGeneration != Bridge->GetSimGeneration())
	{
		// The sim was replaced (new start, load): show its world, or a preview of the same seed without one.
		SimGeneration = Bridge->GetSimGeneration();
		const colony::World* SimWorld = Bridge->GetSim()->world();
		bPreview = !SimWorld || SimWorld->empty();
		Preview.reset();
		Selected = SimWorld ? SimWorld->startTile() : -1;
		if (bPreview)
		{
			MakeWorld(static_cast<int32>(Bridge->GetStartOptions().Seed & 0x7fffffff));
		}
	}
	else if (bPreview && !Preview && Bridge->GetDefs())
	{
		MakeWorld(Seed);
	}
	const UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
	if (!bOpen && !(Overseer && Overseer->IsOverseerActive()))
	{
		return;
	}
	DrawScreen(*Bridge);
}

void UWorldScreenSubsystem::DrawOpenButton(float Scale, float Margin)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - Margin, Viewport->Pos.y + Viewport->Size.y - Margin), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
	const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	if (ImGui::Begin("##WorldScreenOpen", nullptr, Flags))
	{
		if (ImGui::Button("World map (M)"))
		{
			SetOpen(true);
		}
	}
	ImGui::End();
}

void UWorldScreenSubsystem::DrawScreen(USimSubsystem& Bridge)
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

	ImGuiIO& IO = ImGui::GetIO();
	if (!IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_M, false))
	{
		bOpen = !bOpen;
	}
	if (bOpen && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		bOpen = false;
	}
	if (!bOpen)
	{
		DrawOpenButton(Scale, Margin);
		return;
	}

	const colony::World* World = GetShownWorld();
	ImGui::SetNextWindowPos(Viewport->Pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(Viewport->Size, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.96f);
	const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;
	if (ImGui::Begin("##WorldScreen", nullptr, Flags))
	{
		const float PanelWidth = 360.0f * Scale;
		ImGui::TextUnformatted("World map");
		ImGui::SameLine();
		if (ImGui::Button("Close"))
		{
			bOpen = false;
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(110.0f * Scale);
		ImGui::InputInt("##seed", &SeedInput, 0, 0);
		ImGui::SameLine();
		if (ImGui::Button("Make world"))
		{
			MakeWorld(FMath::Max(SeedInput, 1));
			World = GetShownWorld();
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(bPreview ? "(preview - the colony is not on it yet)" : "(the colony's world)");

		const ImVec2 Origin = ImGui::GetCursorScreenPos();
		const float MapW = FMath::Max(1.0f, Viewport->Pos.x + Viewport->Size.x - PanelWidth - Margin - Origin.x);
		const float MapH = FMath::Max(1.0f, Viewport->Pos.y + Viewport->Size.y - Margin - Origin.y);
		Stats.TilesDrawn = 0;
		if (!World)
		{
			ImGui::TextUnformatted("No world: the data folder has no WorldGen def.");
		}
		else
		{
			const float TileSize = std::min(MapW / static_cast<float>(World->width()), MapH / static_cast<float>(World->height()));
			const float Left = Origin.x;
			const float Top = Origin.y;
			ImDrawList* Draw = ImGui::GetWindowDrawList();
			const auto Center = [&](int32 Tile)
			{
				return ImVec2(Left + (World->tileX(Tile) + 0.5f) * TileSize, Top + (World->tileY(Tile) + 0.5f) * TileSize);
			};
			for (int32 Tile = 0; Tile < World->tileCount(); ++Tile)
			{
				const float X = Left + World->tileX(Tile) * TileSize;
				const float Y = Top + World->tileY(Tile) * TileSize;
				Draw->AddRectFilled(ImVec2(X, Y), ImVec2(X + TileSize + 0.5f, Y + TileSize + 0.5f), TileColor(*World, World->tile(Tile)));
				++Stats.TilesDrawn;
			}
			// Rivers run to the tile they flow into (not across the wrap).
			for (int32 Tile = 0; Tile < World->tileCount(); ++Tile)
			{
				const colony::WorldTile& T = World->tile(Tile);
				if (T.river == colony::RiverSize::None || T.riverFlowsTo == colony::kNoDirection || T.water)
				{
					continue;
				}
				const int32 To = World->neighbour(Tile, T.riverFlowsTo);
				if (To < 0 || std::abs(World->tileX(To) - World->tileX(Tile)) > 1)
				{
					continue;
				}
				Draw->AddLine(Center(Tile), Center(To), IM_COL32(70, 130, 230, 255), 0.7f + 0.6f * static_cast<float>(static_cast<int32>(T.river)) * TileSize / 8.0f);
			}
			for (const colony::WorldRoad& Road : World->roads())
			{
				if (std::abs(World->tileX(Road.tileA) - World->tileX(Road.tileB)) > 1)
				{
					continue;
				}
				Draw->AddLine(Center(Road.tileA), Center(Road.tileB), RoadColor(Road.kind), FMath::Max(1.0f, TileSize * 0.3f));
			}
			for (const colony::WorldSettlement& Settlement : World->settlements())
			{
				Draw->AddCircleFilled(Center(Settlement.tile), FMath::Max(2.0f, TileSize * 0.45f), IM_COL32(220, 40, 40, 255));
			}
			if (World->startTile() >= 0 && !bPreview)
			{
				Draw->AddCircle(Center(World->startTile()), FMath::Max(4.0f, TileSize * 0.9f), IM_COL32(255, 230, 0, 255), 0, 2.0f);
			}
			if (Selected >= 0)
			{
				const ImVec2 C = Center(Selected);
				const float H = TileSize * 0.5f;
				Draw->AddRect(ImVec2(C.x - H, C.y - H), ImVec2(C.x + H, C.y + H), IM_COL32(255, 255, 255, 255), 0.0f, 2.0f);
			}

			// The map takes the mouse: hover shows the tile's name, a click picks it.
			ImGui::SetCursorScreenPos(ImVec2(Left, Top));
			ImGui::InvisibleButton("##map", ImVec2(TileSize * World->width(), TileSize * World->height()));
			const ImVec2 Mouse = ImGui::GetMousePos();
			const int32 Hover = ImGui::IsItemHovered() ? TileAt(*World, Left, Top, TileSize, Mouse.x, Mouse.y) : -1;
			if (Hover >= 0)
			{
				const colony::WorldTile& T = World->tile(Hover);
				const FString Name = T.biome == colony::kNoBiome ? FString(TEXT("open water")) : Words(World->biomeName(T));
				ImGui::SetTooltip("%s", TCHAR_TO_UTF8(*FString::Printf(TEXT("%s, %s"), *Name, T.water ? TEXT("water") : *HillsWords(T.hilliness))));
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				{
					SelectTile(Hover);
				}
			}

			// The facts of the picked tile, and the button that starts a colony on it.
			ImGui::SetCursorScreenPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - PanelWidth, Origin.y));
			ImGui::BeginChild("##facts", ImVec2(PanelWidth - Margin, MapH), ImGuiChildFlags_Borders);
			if (Selected < 0)
			{
				ImGui::TextUnformatted("Click a tile to see what is there.");
			}
			else
			{
				for (const FString& Line : DescribeTile(Selected))
				{
					WText(Line);
				}
				ImGui::Separator();
				const FString Why = WhyNotStart(Selected);
				if (Why.IsEmpty())
				{
					if (ImGui::Button("Start colony here"))
					{
						StartHere();
					}
					if (!bPreview && Selected != World->startTile())
					{
						ImGui::TextUnformatted("This starts a new colony.");
					}
				}
				else
				{
					WText(Why);
				}
			}
			ImGui::Separator();
			ImGui::TextUnformatted("Red dots: settlements. Blue lines: rivers.");
			ImGui::TextUnformatted("Brown to black lines: roads.");
			if (World->startTile() >= 0 && !bPreview)
			{
				ImGui::TextUnformatted("Yellow ring: the colony's tile.");
			}
			ImGui::EndChild();
		}
	}
	ImGui::End();
	++Stats.FramesDrawn;
}
