// The title screen and the new game (docs/design/new-game.md): title, scenario, landing site (the world screen in its
// new-game mode), then character creation in two parts on the colonists' real 3D bodies: "You" (who the amnesiac
// player is: looks and a name only) and "Your companions" (the two 18-year-old strangers: looks, names and traits bought
// from the scenario's point pool). Our own look, "Canopy" (sneed 2026-11-21: no reference UIs), described at NgStyle.

#include "ColonyHudSubsystem.h"

#include "Camera/CameraActor.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/PointLight.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"

#include <imgui.h>

#include "ColonyHudStyle.h"
#include "CreatorPeople.h"
#include "DebugScenes.h"
#include "OverseerSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"
#include "SkySubsystem.h"
#include "TerrainSubsystem.h"
#include "WorldScreenSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
	// The character screen's stage (cosmetic): the hour the sky is held at, the key light (candelas, real units like the
	// sun's lux in data/look.json), and how far out from the body the camera's line must be clear of trees.
	constexpr float NgPreviewHour = 10.5f;
	constexpr float NgPreviewLampIntensity = 60000.0f;
	constexpr float NgPreviewClearMetres = 5.5f;
	// Plants within this many metres of a person on show are not drawn while the screen is open (a cosmetic clearing).
	constexpr float NgPreviewClearingMetres = 3.5f;
	// The engine warns on screen when its exposure passes 12 (cached lighting would clip): the screen's hour is the one
	// nearest NgPreviewHour, earlier in the morning, whose exposure (ours, which reads a little above the engine's) stays
	// under this. At 6:30, 12.1 of ours showed no warning; 10:30 did.
	constexpr float NgPreviewMaxEV = 12.25f;

	/**
	 * "Canopy", the new-game screens' own look: dark green-black glass panels with square corners and a short jade bar on
	 * each panel's top edge; bone-white text; one jade accent for what is chosen and one ember accent for what costs.
	 * Three text sizes only (titles, headings, body), one gap and one pad, so every screen lines up the same way.
	 */
	namespace NgStyle
	{
		const ImVec4 Panel(0.030f, 0.050f, 0.043f, 0.90f);
		const ImVec4 PanelEdge(0.37f, 0.83f, 0.66f, 0.16f);
		const ImVec4 Text(0.91f, 0.89f, 0.84f, 1.0f);
		const ImVec4 Dim(0.62f, 0.66f, 0.63f, 1.0f);
		const ImVec4 Faint(0.40f, 0.45f, 0.42f, 1.0f);
		const ImVec4 Jade(0.37f, 0.83f, 0.66f, 1.0f);
		const ImVec4 JadeDeep(0.08f, 0.26f, 0.20f, 0.95f);
		const ImVec4 Ember(1.00f, 0.56f, 0.36f, 1.0f);
		const ImVec4 Bad(0.93f, 0.38f, 0.34f, 1.0f);
		const ImVec4 Field(0.015f, 0.025f, 0.022f, 0.95f);
		constexpr float kTitle = 2.2f;
		constexpr float kHeading = 1.25f;
		constexpr float kPad = 24.0f;   // round the screen and between panels (times the view scale)
		constexpr float kGap = 10.0f;   // between things in a panel
	}

	FString NgCapital(const std::string& Text)
	{
		FString Out = UTF8_TO_TCHAR(Text.c_str());
		if (!Out.IsEmpty())
		{
			Out[0] = FChar::ToUpper(Out[0]);
		}
		return Out;
	}

	FString NgDefLabel(const colony::DefDatabase& Defs, const char* Kind, const std::string& Name, const char* Field = "label")
	{
		const colony::Def* Def = Defs.find(Kind, Name);
		if (!Def)
		{
			return NgCapital(Name);
		}
		const std::string Text = Def->text(Field, Def->label());
		return NgCapital(Text.empty() ? Name : Text);
	}

	/** A trait degree's label and description. */
	void NgTraitText(const colony::DefDatabase& Defs, const colony::TraitRecord& Trait, FString& OutLabel, FString& OutDescription)
	{
		const colony::Def* Def = Defs.find("Traits", Trait.def);
		std::string Label = Def ? Def->label() : Trait.def;
		std::string Description;
		for (size_t Index = 0; Def && Index < Def->count("degreeDatas"); ++Index)
		{
			const std::string Base = "degreeDatas." + std::to_string(Index);
			if (static_cast<int>(Def->integer((Base + ".degree").c_str(), 0)) == Trait.degree)
			{
				Label = Def->text((Base + ".label").c_str(), Label);
				Description = Def->text((Base + ".description").c_str());
			}
		}
		OutLabel = NgCapital(Label.empty() ? Trait.def : Label);
		OutDescription = UTF8_TO_TCHAR(Description.c_str());
	}

	/** Text in one of the three sizes. */
	void NgSized(float Size, const ImVec4& Colour, const FString& Text)
	{
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * Size);
		ColonyHud::TextColored(Colour, Text);
		ImGui::PopFont();
	}

	void NgTitle(const FString& Text) { NgSized(NgStyle::kTitle, NgStyle::Text, Text.ToUpper()); }

	/** A thin jade rule across the panel. */
	void NgRule()
	{
		const ImVec2 P = ImGui::GetCursorScreenPos();
		const float W = ImGui::GetContentRegionAvail().x;
		ImGui::GetWindowDrawList()->AddLine(P, ImVec2(P.x + W, P.y), ImGui::GetColorU32(ImVec4(NgStyle::Jade.x, NgStyle::Jade.y, NgStyle::Jade.z, 0.28f)), 1.0f);
		ImGui::Dummy(ImVec2(W, 6.0f));
	}

	/** A section heading: jade capitals and a rule under them. */
	void NgHeading(const FString& Text)
	{
		ImGui::Dummy(ImVec2(1.0f, 4.0f));
		NgSized(NgStyle::kHeading, NgStyle::Jade, Text.ToUpper());
		NgRule();
	}

	/** A field's small label: dim capitals. */
	void NgLabel(const FString& Text) { ColonyHud::TextColored(NgStyle::Dim, Text.ToUpper()); }

	/** A panel: Canopy glass with the jade bar on top. Pair with NgEndPanel. */
	bool NgBeginPanel(const char* Id, const ImVec2& Pos, const ImVec2& Size)
	{
		ImGui::SetNextWindowPos(Pos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(Size, ImGuiCond_Always);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, NgStyle::Panel);
		ImGui::PushStyleColor(ImGuiCol_Border, NgStyle::PanelEdge);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, NgStyle::Field);
		ImGui::PushStyleColor(ImGuiCol_SliderGrab, NgStyle::Jade);
		ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, NgStyle::Text);
		ImGui::PushStyleColor(ImGuiCol_CheckMark, NgStyle::Jade);
		ImGui::PushStyleColor(ImGuiCol_Text, NgStyle::Text);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
		const bool bOpen = ImGui::Begin(Id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
		const ImVec2 At = ImGui::GetWindowPos();
		ImGui::GetWindowDrawList()->AddRectFilled(At, ImVec2(At.x + ImGui::GetFontSize() * 3.0f, At.y + 3.0f), ImGui::GetColorU32(NgStyle::Jade));
		return bOpen;
	}

	void NgEndPanel()
	{
		ImGui::End();
		ImGui::PopStyleVar(4);
		ImGui::PopStyleColor(7);
	}

	/** A Canopy button: outlined; jade-filled while `bChosen`; solid jade with dark text when `bPrimary`. */
	bool NgButton(const FString& Label, bool bChosen = false, const ImVec2& Size = ImVec2(0.0f, 0.0f), bool bPrimary = false)
	{
		const ImVec4 Fill = bPrimary ? NgStyle::Jade : (bChosen ? NgStyle::JadeDeep : ImVec4(0.06f, 0.09f, 0.08f, 0.85f));
		const ImVec4 Hover = bPrimary ? ImVec4(0.52f, 0.92f, 0.77f, 1.0f) : ImVec4(0.12f, 0.30f, 0.24f, 0.95f);
		ImGui::PushStyleColor(ImGuiCol_Button, Fill);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hover);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, NgStyle::Jade);
		ImGui::PushStyleColor(ImGuiCol_Border, bChosen || bPrimary ? NgStyle::Jade : NgStyle::Faint);
		ImGui::PushStyleColor(ImGuiCol_Text, bPrimary ? ImVec4(0.02f, 0.06f, 0.05f, 1.0f) : NgStyle::Text);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
		const FTCHARToUTF8 Utf8(*Label);
		const bool bClicked = ImGui::Button(Utf8.Get(), Size);
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(5);
		return bClicked;
	}

	/** The new game's steps, the one we are on lit. */
	void NgSteps(int32 Current)
	{
		static const TCHAR* const Steps[] = {TEXT("Scenario"), TEXT("Landing site"), TEXT("You"), TEXT("Companions")};
		for (int32 i = 0; i < 4; ++i)
		{
			if (i > 0)
			{
				ImGui::SameLine();
				ColonyHud::TextColored(NgStyle::Faint, TEXT("  /  "));
				ImGui::SameLine();
			}
			ColonyHud::TextColored(i == Current ? NgStyle::Jade : (i < Current ? NgStyle::Dim : NgStyle::Faint), FString(Steps[i]).ToUpper());
		}
	}

	/** A small flame for a passion: one for minor, two for major (drawn, so no font glyph is needed). */
	void NgFlames(colony::Passion Passion, float Size)
	{
		const int32 Count = Passion == colony::Passion::Major ? 2 : (Passion == colony::Passion::Minor ? 1 : 0);
		const ImVec2 P = ImGui::GetCursorScreenPos();
		ImDrawList* Draw = ImGui::GetWindowDrawList();
		for (int32 i = 0; i < 2; ++i)
		{
			const float X = P.x + i * Size * 0.8f + Size * 0.35f;
			const ImU32 Colour = i < Count ? ImGui::GetColorU32(NgStyle::Ember) : IM_COL32(90, 90, 90, 70);
			Draw->AddTriangleFilled(ImVec2(X, P.y + Size * 0.1f), ImVec2(X - Size * 0.3f, P.y + Size * 0.85f), ImVec2(X + Size * 0.3f, P.y + Size * 0.85f), Colour);
			Draw->AddCircleFilled(ImVec2(X, P.y + Size * 0.72f), Size * 0.3f, Colour);
		}
		ImGui::Dummy(ImVec2(Size * 1.8f, Size));
	}

	/** The races on the companions screen: the human one open, the others shown and locked until they are done (sneed). */
	struct FNgRace
	{
		const char* Lineage;
		const TCHAR* Fallback;
	};
	const FNgRace NgRaces[] = {{"Baseline", TEXT("Human")}, {"Emberkin", TEXT("Impid")}, {"Orcoid", TEXT("Orcoid")}, {"Drow", TEXT("Drow")},
		{"CatStrain", TEXT("Cat-strain")}};

	// Swatches (sRGB): natural hair, skin from fair to dark, eyes.
	const float NgHairSwatches[][3] = {{0.07f, 0.06f, 0.055f}, {0.17f, 0.11f, 0.07f}, {0.32f, 0.22f, 0.14f}, {0.48f, 0.36f, 0.24f},
		{0.78f, 0.66f, 0.45f}, {0.45f, 0.20f, 0.10f}, {0.62f, 0.27f, 0.12f}, {0.62f, 0.60f, 0.58f}};
	const float NgSkinSwatches[][3] = {{0.96f, 0.86f, 0.80f}, {0.91f, 0.80f, 0.75f}, {0.86f, 0.72f, 0.62f}, {0.78f, 0.62f, 0.50f},
		{0.64f, 0.47f, 0.36f}, {0.48f, 0.34f, 0.25f}, {0.33f, 0.23f, 0.17f}};
	const float NgEyeSwatches[][3] = {{0.30f, 0.20f, 0.12f}, {0.45f, 0.32f, 0.18f}, {0.35f, 0.50f, 0.65f}, {0.40f, 0.55f, 0.35f},
		{0.50f, 0.55f, 0.55f}, {0.15f, 0.12f, 0.10f}};

	bool NgSwatch(const char* Id, const float* C, bool bPicked, float Size)
	{
		ImGui::PushStyleColor(ImGuiCol_Border, bPicked ? NgStyle::Jade : ImVec4(0, 0, 0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, bPicked ? 2.0f : 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
		const bool bClicked = ImGui::ColorButton(Id, ImVec4(C[0], C[1], C[2], 1.0f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(Size, Size));
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();
		return bClicked;
	}

	bool NgSame(const CreatorPeople::FLook& Look, const TCHAR* Prefix, const float* C)
	{
		const float R = Look.Get(*FString::Printf(TEXT("%sR"), Prefix), -1.0f);
		const float G = Look.Get(*FString::Printf(TEXT("%sG"), Prefix), -1.0f);
		const float B = Look.Get(*FString::Printf(TEXT("%sB"), Prefix), -1.0f);
		return FMath::Abs(R - C[0]) < 0.005f && FMath::Abs(G - C[1]) < 0.005f && FMath::Abs(B - C[2]) < 0.005f;
	}

	void NgSetColour(CreatorPeople::FLook& Look, const TCHAR* Prefix, const float* C)
	{
		Look.Values.Add(FString::Printf(TEXT("%sR"), Prefix), C[0]);
		Look.Values.Add(FString::Printf(TEXT("%sG"), Prefix), C[1]);
		Look.Values.Add(FString::Printf(TEXT("%sB"), Prefix), C[2]);
	}

	/** A random name from a Names bank (the command carries it, so the sim stays deterministic). */
	std::string NgRandomName(const colony::DefDatabase& Defs, const char* Bank)
	{
		const colony::Def* Def = Defs.find("Names", Bank);
		const size_t Count = Def ? Def->count("names") : 0;
		return Count == 0 ? std::string() : Def->textAt("names", static_cast<size_t>(FMath::RandRange(0, static_cast<int32>(Count) - 1)));
	}

	/** Traits as SetColonistTraitsCommand writes them: "Def:degree;...". */
	std::string NgTraitsCommandText(const std::vector<colony::TraitRecord>& Traits)
	{
		std::string Out;
		for (const colony::TraitRecord& T : Traits)
		{
			Out += T.def + ":" + std::to_string(T.degree) + ";";
		}
		return Out;
	}

	const TCHAR* const NgCottonClothes[] = {TEXT("Apparel_TShirt:Cloth"), TEXT("Apparel_Pants:Cloth")};
}

// ---- Steps of the new game ----------------------------------------------------------------------

void UColonyHudSubsystem::CheckLanding(USimSubsystem& Bridge)
{
	if (!bAwaitingLanding)
	{
		return;
	}
	UWorldScreenSubsystem* WorldScreen = GetWorld()->GetSubsystem<UWorldScreenSubsystem>();
	if (!WorldScreen)
	{
		bAwaitingLanding = false;
		return;
	}
	if (WorldScreen->ConsumeLanding())
	{
		bAwaitingLanding = false;
		EnterPeoplePage(Bridge);
	}
	else if (WorldScreen->ConsumeCancel() || (!WorldScreen->IsOpen() && !WorldScreen->IsNewGame()))
	{
		bAwaitingLanding = false;
		bMainMenuOpen = true;
		MainMenuPage = 1;
	}
}

void UColonyHudSubsystem::EnterPeoplePage(USimSubsystem& Bridge)
{
	bMainMenuOpen = true;
	MainMenuPage = 3;
	PeopleSelected = 0;
	PeopleTab = 0;
	PeopleNamesFor = colony::kNoEntity;
	PeopleLooks.Reset();
	PeopleGeneration = Bridge.GetSimGeneration();
	PeoplePitch = -4.0f;
	PeopleDistance = 300.0f;
	PeopleSpin = 0.0f;
	PeopleSpinRate = 0.0f;
	bPeopleClothes = false;
	PeopleViewFor = colony::kNoEntity;
	Bridge.SetSpeed(0);
}

void UColonyHudSubsystem::LandColony(USimSubsystem& Bridge)
{
	// Whatever is still queued (the last edit) goes in before the clock starts.
	Bridge.ApplyPregameCommands();
	EndPeopleCamera();
	bMainMenuOpen = false;
	MainMenuPage = 0;
	if (UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>())
	{
		Overseer->EnterOverseerMode();
	}
	Bridge.SetSpeed(1);
}

void UColonyHudSubsystem::LandColonyForTest()
{
	if (USimSubsystem* Bridge = GetBridge())
	{
		LandColony(*Bridge);
	}
}

void UColonyHudSubsystem::EndPeopleCamera()
{
	if (ACameraActor* Camera = PeopleCamera.Get())
	{
		Camera->Destroy();
	}
	PeopleCamera.Reset();
	if (APointLight* Lamp = PeopleLamp.Get())
	{
		Lamp->Destroy();
	}
	PeopleLamp.Reset();
	PeopleViewFor = colony::kNoEntity;
	// The turntable, the clothes preview and the daylight are cosmetic: they end with the screen.
	if (USkySubsystem* Sky = GetWorld() ? GetWorld()->GetSubsystem<USkySubsystem>() : nullptr)
	{
		Sky->SetHourOverride(-1.0f);
	}
	if (UTerrainSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainSubsystem>() : nullptr)
	{
		Terrain->SetPlantClearings({});
	}
	if (UWorld* World = GetWorld())
	{
		if (USimPawnSubsystem* Pawns = World->GetSubsystem<USimPawnSubsystem>())
		{
			Pawns->ClearApparelPreviews();
			if (USimSubsystem* Sims = World->GetSubsystem<USimSubsystem>(); Sims && Sims->GetSim())
			{
				for (const colony::Entity Pawn : Sims->GetSim()->startingColonistSlots())
				{
					if (ASimPawnActor* Actor = Pawns->FindPawn(Pawn))
					{
						Actor->SetPreviewSpin(0.0f);
					}
				}
			}
		}
	}
}

float UColonyHudSubsystem::PickPeopleView(colony::Entity Pawn) const
{
	// Tries the camera in front of the body first, then further round each way, and takes the first direction with no tree
	// trunk or other starting person between the camera and the body (or the least blocked one).
	UWorld* World = GetWorld();
	USimPawnSubsystem* Pawns = World ? World->GetSubsystem<USimPawnSubsystem>() : nullptr;
	USimSubsystem* Bridge = World ? World->GetSubsystem<USimSubsystem>() : nullptr;
	const ASimPawnActor* Actor = Pawns ? Pawns->FindPawn(Pawn) : nullptr;
	if (!Actor || !Bridge || !Bridge->GetSim())
	{
		return 0.0f;
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	const FVector Origin = Bridge->SimToWorld(0.0f, 0.0f, 0);
	const FVector2D At((Actor->GetActorLocation().X - Origin.X) / 100.0, (Actor->GetActorLocation().Y - Origin.Y) / 100.0);
	TArray<FVector2D> Others;
	for (const colony::Entity Other : Sim.startingColonistSlots())
	{
		if (const ASimPawnActor* OtherActor = Other != Pawn ? Pawns->FindPawn(Other) : nullptr)
		{
			Others.Add(FVector2D((OtherActor->GetActorLocation().X - Origin.X) / 100.0, (OtherActor->GetActorLocation().Y - Origin.Y) / 100.0));
		}
	}
	const float Facing = Actor->GetDrawnYaw();
	float Best = 0.0f;
	float BestBlocked = TNumericLimits<float>::Max();
	for (int32 Step = 0; Step <= 18; ++Step)
	{
		const float Offset = (Step % 2 == 1 ? 1.0f : -1.0f) * 20.0f * static_cast<float>((Step + 1) / 2);
		const float Yaw = FMath::DegreesToRadians(Facing + Offset);
		const FVector2D Dir(FMath::Cos(Yaw), FMath::Sin(Yaw));
		float Blocked = 0.0f;
		for (float Along = 0.5f; Along <= NgPreviewClearMetres; Along += 0.25f)
		{
			const FVector2D P = At + Dir * Along;
			const colony::CellPos Cell{FMath::FloorToInt32(P.X), FMath::FloorToInt32(P.Y), 0};
			if (!Sim.map().inBounds(Cell))
			{
				Blocked += 1.0f;
				continue;
			}
			float TrunkX = 0.0f;
			float TrunkY = 0.0f;
			float Radius = 0.0f;
			float Height = 0.0f;
			// Plants in the clearings round the people on show are not drawn (SetPlantClearings): they don't count.
			bool bCleared = FVector2D::Distance(P, At) < NgPreviewClearingMetres;
			for (const FVector2D& O : Others)
			{
				bCleared = bCleared || FVector2D::Distance(P, O) < NgPreviewClearingMetres;
			}
			if (!bCleared && Sim.plants().trunkOf(Cell, TrunkX, TrunkY, Radius, Height) && Height > 1.0f
				&& FVector2D::Distance(P, FVector2D(TrunkX, TrunkY)) < Radius + 1.5f)
			{
				Blocked += 2.0f;  // a trunk, and the leaves around it
			}
			else if (!bCleared && Sim.plants().isTree(Cell))
			{
				Blocked += 0.5f;
			}
			for (const FVector2D& O : Others)
			{
				if (FVector2D::Distance(P, O) < 0.6f)
				{
					Blocked += 2.0f;
				}
			}
		}
		if (Blocked < BestBlocked - 0.01f)
		{
			BestBlocked = Blocked;
			Best = Offset;
		}
		if (BestBlocked <= 0.0f)
		{
			break;
		}
	}
	return Best;
}

void UColonyHudSubsystem::UpdatePeopleCamera(colony::Entity Pawn)
{
	UWorld* World = GetWorld();
	USimPawnSubsystem* Pawns = World ? World->GetSubsystem<USimPawnSubsystem>() : nullptr;
	ASimPawnActor* Actor = Pawns ? Pawns->FindPawn(Pawn) : nullptr;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!Actor || !PC)
	{
		return;
	}
	if (!PeopleCamera.IsValid())
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		PeopleCamera = World->SpawnActor<ACameraActor>(Actor->GetActorLocation(), FRotator::ZeroRotator, Params);
	}
	ACameraActor* Camera = PeopleCamera.Get();
	if (!Camera)
	{
		return;
	}
	// The screen is shown by day, whatever hour the landing is at (the sim's clock is untouched).
	USimSubsystem* Bridge = World->GetSubsystem<USimSubsystem>();
	if (USkySubsystem* Sky = World->GetSubsystem<USkySubsystem>(); Sky && Sky->GetHourOverride() < 0.0f && Bridge && Bridge->GetSim())
	{
		const float Day = static_cast<float>(FMath::Fmod(static_cast<double>(Bridge->GetGameSeconds()) / colony::kSecondsPerDay, static_cast<double>(colony::kDaysPerYear)));
		float Hour = NgPreviewHour;
		while (Hour > 6.5f && Sky->ComputeState(*Bridge->GetSim(), Hour, Day).ExposureEV + Sky->GetLook().DarkerEV > NgPreviewMaxEV)
		{
			Hour -= 0.25f;
		}
		Sky->SetHourOverride(Hour);
	}
	// A clearing round each person on show, so no tree stands over them or between them and the camera.
	if (UTerrainSubsystem* Terrain = World->GetSubsystem<UTerrainSubsystem>(); Terrain && Bridge && Bridge->GetSim())
	{
		const FVector Origin = Bridge->SimToWorld(0.0f, 0.0f, 0);
		TArray<FVector3f> Clearings;
		for (const colony::Entity Shown : Bridge->GetSim()->startingColonistSlots())
		{
			if (const ASimPawnActor* ShownActor = Pawns->FindPawn(Shown))
			{
				const FVector P = (ShownActor->GetActorLocation() - Origin) / 100.0;
				Clearings.Add(FVector3f(static_cast<float>(P.X), static_cast<float>(P.Y), NgPreviewClearingMetres));
			}
		}
		Terrain->SetPlantClearings(Clearings);
	}
	// A new pawn on the stand: the camera goes where no tree or other person is in the way, and the body turns to face it.
	if (PeopleViewFor != Pawn)
	{
		PeopleViewFor = Pawn;
		PeopleViewYaw = PickPeopleView(Pawn);
		PeopleSpin = PeopleViewYaw;
		PeopleSpinRate = 0.0f;
		Actor->SetPreviewSpin(PeopleSpin);
	}
	// The turntable: dragging over the open middle spins the body itself (so soft tissue and hair swing with it), and
	// it keeps turning a little after the mouse lets go; up and down tilt the view, the wheel zooms.
	const ImGuiIO& IO = ImGui::GetIO();
	const float Delta = FMath::Clamp(IO.DeltaTime, 0.0f, 0.1f);
	if (!IO.WantCaptureMouse && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0f)))
	{
		PeopleSpinRate = Delta > 0.0f ? IO.MouseDelta.x * 0.45f / Delta : 0.0f;
		PeoplePitch = FMath::Clamp(PeoplePitch - IO.MouseDelta.y * 0.15f, -30.0f, 15.0f);
	}
	else
	{
		PeopleSpinRate *= FMath::Exp(-2.5f * Delta);
	}
	PeopleSpinRate = FMath::Clamp(PeopleSpinRate, -900.0f, 900.0f);
	PeopleSpin = FRotator::NormalizeAxis(PeopleSpin + PeopleSpinRate * Delta);
	if (!IO.WantCaptureMouse && IO.MouseWheel != 0.0f)
	{
		PeopleDistance = FMath::Clamp(PeopleDistance * (IO.MouseWheel > 0.0f ? 0.88f : 1.0f / 0.88f), 70.0f, 520.0f);
	}
	Actor->SetPreviewSpin(PeopleSpin);
	// The camera stays put in front of the body's own facing (not the spin), at the body's middle, the face when close.
	FVector Focus = Actor->GetActorLocation() + FVector(0.0, 0.0, 20.0);
	if (const USkeletalMeshComponent* Mesh = Actor->GetMakeHumanMesh())
	{
		const FBoxSphereBounds Bounds = Mesh->Bounds;
		const float Closeness = 1.0f - FMath::Clamp((PeopleDistance - 70.0f) / 300.0f, 0.0f, 1.0f);
		Focus = FVector(Actor->GetActorLocation().X, Actor->GetActorLocation().Y, Bounds.Origin.Z + Bounds.BoxExtent.Z * (0.05f + 0.7f * Closeness));
	}
	const float Facing = Actor->GetDrawnYaw() + PeopleViewYaw;
	const FRotator Around(PeoplePitch, Facing + 180.0f, 0.0f);
	const FVector Eye = Focus - Around.Vector() * PeopleDistance;
	Camera->SetActorLocationAndRotation(Eye, Around);
	// A soft key light up and to one side of the camera, so the face and body read even in the shade of the canopy.
	if (!PeopleLamp.IsValid())
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		PeopleLamp = World->SpawnActor<APointLight>(Eye, FRotator::ZeroRotator, Params);
		if (APointLight* Lamp = PeopleLamp.Get())
		{
			Lamp->PointLightComponent->SetMobility(EComponentMobility::Movable);
			Lamp->PointLightComponent->SetIntensityUnits(ELightUnits::Candelas);
			Lamp->PointLightComponent->SetIntensity(NgPreviewLampIntensity);
			Lamp->PointLightComponent->SetAttenuationRadius(900.0f);
			Lamp->PointLightComponent->SetSourceRadius(60.0f);
			Lamp->PointLightComponent->SetSoftSourceRadius(120.0f);
			Lamp->PointLightComponent->SetLightColor(FLinearColor(1.0f, 0.95f, 0.88f));
			Lamp->PointLightComponent->SetCastShadows(false);
		}
	}
	if (APointLight* Lamp = PeopleLamp.Get())
	{
		const FVector Side = FRotationMatrix(Around).GetUnitAxis(EAxis::Y);
		const FVector Toward = (Focus - Eye).GetSafeNormal();
		Lamp->SetActorLocation(Focus - Toward * 180.0f + Side * 120.0f + FVector(0.0, 0.0, 90.0));
	}
	if (PC->GetViewTarget() != Camera)
	{
		PC->SetViewTargetWithBlend(Camera, 0.6f);
	}
}

void UColonyHudSubsystem::SubmitPeopleLook(colony::Entity Pawn, int32 Slot, const CreatorPeople::FLook& Look)
{
	const FString Text = CreatorPeople::Encode(Look);
	PeopleLooks.Add(static_cast<uint32>(entt::to_integral(Pawn)), Text);
	colony::EditColonistCommand Edit;
	Edit.slot = Slot;
	Edit.setCreator = true;
	Edit.creator = TCHAR_TO_UTF8(*Text);
	Submit(Edit);
}

// ---- Pages ----------------------------------------------------------------------------------------

void UColonyHudSubsystem::DrawTitlePage(USimSubsystem& Bridge, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float Wide = 380.0f * Scale;
	if (!NgBeginPanel("##NgTitle", ImVec2(Viewport->Pos.x + 80.0f * Scale, Viewport->Pos.y + Viewport->Size.y * 0.28f), ImVec2(Wide + 40.0f * Scale, 0.0f)))
	{
		NgEndPanel();
		return;
	}
	NgTitle(TEXT("Rim3D"));
	ColonyHud::TextColored(NgStyle::Dim, TEXT("A colony on the rim, year 6650"));
	NgRule();
	ImGui::Spacing();

	// Continue: the newest save.
	FString Newest;
	FDateTime NewestTime = FDateTime::MinValue();
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(USimSubsystem::GetSaveDirectory() / TEXT("*.save")), true, false);
		for (const FString& File : Files)
		{
			const FDateTime Time = IFileManager::Get().GetTimeStamp(*(USimSubsystem::GetSaveDirectory() / File));
			if (Time > NewestTime)
			{
				NewestTime = Time;
				Newest = FPaths::GetBaseFilename(File);
			}
		}
	}
	const bool bRunning = Bridge.GetSim() && Bridge.GetSim()->time().steps > 0;
	if (bRunning && NgButton(TEXT("Back to the game"), false, ImVec2(Wide, 0.0f), true))
	{
		bMainMenuOpen = false;
	}
	if (!bRunning && !Newest.IsEmpty())
	{
		if (NgButton(TEXT("Continue"), false, ImVec2(Wide, 0.0f), true) && Bridge.LoadGame(Newest))
		{
			bNarratorPending = false;
			bMainMenuOpen = false;
			if (UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>())
			{
				Overseer->EnterOverseerMode();
			}
		}
		ColonyHud::Tip(FString::Printf(TEXT("%s, saved %s"), *Newest, *NewestTime.ToString(TEXT("%d %b %Y %H:%M"))));
	}
	if (NgButton(TEXT("New colony"), false, ImVec2(Wide, 0.0f)))
	{
		MainMenuPage = 1;
	}
	if (NgButton(TEXT("Load a game"), false, ImVec2(Wide, 0.0f)))
	{
		MainMenuPage = 2;
		SaveSlots.Reset();
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(USimSubsystem::GetSaveDirectory() / TEXT("*.save")), true, false);
		// Newest first.
		Files.Sort([](const FString& A, const FString& B)
		{
			return IFileManager::Get().GetTimeStamp(*(USimSubsystem::GetSaveDirectory() / A)) > IFileManager::Get().GetTimeStamp(*(USimSubsystem::GetSaveDirectory() / B));
		});
		for (const FString& File : Files)
		{
			SaveSlots.Add(FPaths::GetBaseFilename(File));
		}
	}
	if (NgButton(TEXT("Settings"), bSettingsOpen, ImVec2(Wide, 0.0f)))
	{
		bSettingsOpen = !bSettingsOpen;
	}
	if (NgButton(TEXT("Debug"), false, ImVec2(Wide, 0.0f)))
	{
		MainMenuPage = 5;
	}
	ColonyHud::Tip(TEXT("Test scenes: the gun range, the character creator and more"));
	ImGui::Spacing();
	if (NgButton(TEXT("Quit to desktop"), false, ImVec2(Wide, 0.0f)))
	{
		UKismetSystemLibrary::QuitGame(GetWorld(), GetWorld()->GetFirstPlayerController(), EQuitPreference::Quit, false);
	}
	NgEndPanel();
}

void UColonyHudSubsystem::DrawScenarioPage(USimSubsystem& Bridge, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const colony::DefDatabase* Defs = Bridge.GetDefs().get();
	const ImVec2 Size(FMath::Min(1100.0f * Scale, Viewport->Size.x - 80.0f * Scale), FMath::Min(640.0f * Scale, Viewport->Size.y - 120.0f * Scale));
	const ImVec2 Pos(Viewport->Pos.x + (Viewport->Size.x - Size.x) * 0.5f, Viewport->Pos.y + (Viewport->Size.y - Size.y) * 0.5f);
	if (!NgBeginPanel("##NgScenario", Pos, Size))
	{
		NgEndPanel();
		return;
	}
	NgTitle(TEXT("New colony"));
	NgSteps(0);
	NgRule();

	// The scenarios the menu offers (`"menu": true`), in their order.
	TArray<const colony::Def*> Scenarios;
	if (Defs)
	{
		for (const colony::DefId Id : Defs->idsOfKind("Scenarios"))
		{
			const colony::Def& Def = Defs->get(Id);
			if (Def.flag("menu"))
			{
				Scenarios.Add(&Def);
			}
		}
		Scenarios.Sort([](const colony::Def& A, const colony::Def& B) { return A.integer("menuOrder", 99) < B.integer("menuOrder", 99); });
	}
	const colony::Def* Picked = nullptr;
	for (const colony::Def* Def : Scenarios)
	{
		Picked = UTF8_TO_TCHAR(Def->defName().c_str()) == NewScenario ? Def : Picked;
	}
	if (!Picked && Scenarios.Num() > 0)
	{
		Picked = Scenarios[0];
		NewScenario = UTF8_TO_TCHAR(Picked->defName().c_str());
	}

	const float ListW = 300.0f * Scale;
	const float FooterH = ImGui::GetFrameHeightWithSpacing() * 1.6f;
	ImGui::BeginChild("##NgScenarioList", ImVec2(ListW, -FooterH), ImGuiChildFlags_None);
	for (const colony::Def* Def : Scenarios)
	{
		const FString Name = UTF8_TO_TCHAR(Def->defName().c_str());
		if (NgButton(FString::Printf(TEXT("%s##scen"), *NgCapital(Def->label())), Def == Picked, ImVec2(ListW - 8.0f * Scale, ImGui::GetFrameHeight() * 1.6f)))
		{
			NewScenario = Name;
		}
	}
	ColonyHud::TextColored(NgStyle::Faint, TEXT("More scenarios later."));
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("##NgScenarioInfo", ImVec2(0.0f, -FooterH), ImGuiChildFlags_None);
	if (Picked && Defs)
	{
		NgHeading(NgCapital(Picked->label()));
		ImGui::PushTextWrapPos(0.0f);
		ColonyHud::Text(UTF8_TO_TCHAR(Picked->text("description").c_str()));
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		const int64 Strangers = Picked->integer("colonists", 0);
		NgLabel(TEXT("People"));
		ColonyHud::Text(Picked->flag("protagonist")
			? FString::Printf(TEXT("You, and %lld %s you don't remember."), Strangers, Strangers == 1 ? TEXT("stranger") : TEXT("strangers"))
			: FString::Printf(TEXT("%lld colonists."), Strangers));
		ImGui::Spacing();
		NgLabel(TEXT("In the cargo pack"));
		for (size_t i = 0; i < Picked->count("startingThings"); ++i)
		{
			const std::string Base = "startingThings." + std::to_string(i);
			const std::string Thing = Picked->text((Base + ".thing").c_str());
			const int64 Count = Picked->integer((Base + ".count").c_str(), 1);
			const std::string Stuff = Picked->text((Base + ".stuff").c_str());
			const FString Made = Stuff.empty() ? FString() : NgDefLabel(*Defs, "Things", Stuff).ToLower() + TEXT(" ");
			ColonyHud::Text(FString::Printf(TEXT("  %lld  %s%s"), Count, *Made, *NgDefLabel(*Defs, "Things", Thing).ToLower()));
		}
		ImGui::Spacing();
		NgLabel(TEXT("Narrator"));
		for (const colony::DefId Id : Defs->idsOfKind("Narrators"))
		{
			const colony::Def& Def = Defs->get(Id);
			const FString Name = UTF8_TO_TCHAR(Def.defName().c_str());
			if (NgButton(FString::Printf(TEXT("%s##narr"), *Name), NewNarrator == Name))
			{
				NewNarrator = Name;
			}
			ColonyHud::Tip(UTF8_TO_TCHAR(Def.text("description").c_str()));
			ImGui::SameLine();
		}
		ImGui::NewLine();
		// Difficulty: only the standard one for now, and it is not shown (sneed, 2026-11-21).
		if (ImGui::CollapsingHeader("World seed and map size"))
		{
			ImGui::SetNextItemWidth(160.0f * Scale);
			ImGui::InputInt("##seed", &NewSeed);
			NewSeed = FMath::Max(NewSeed, 0);
			ImGui::SameLine();
			if (NgButton(TEXT("Random seed")))
			{
				NewSeed = FMath::RandRange(1, 999999);
			}
			const int32 Sizes[] = {256, 512, 1024};
			for (const int32 MapSize : Sizes)
			{
				if (NgButton(FString::Printf(TEXT("%d m"), MapSize), NewMapSize == MapSize))
				{
					NewMapSize = MapSize;
				}
				ImGui::SameLine();
			}
			ImGui::NewLine();
		}
	}
	ImGui::EndChild();
	NgRule();
	const float FooterRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	if (NgButton(TEXT("Back"), false, ImVec2(160.0f * Scale, 0.0f)))
	{
		MainMenuPage = 0;
	}
	ImGui::SameLine(FooterRight - 520.0f * Scale);
	if (NgButton(TEXT("Land somewhere random"), false, ImVec2(250.0f * Scale, 0.0f)) && Picked)
	{
		StartNewColony(false);
	}
	ColonyHud::Tip(TEXT("Skips the world map: the shuttle comes down on a random tropical site"));
	ImGui::SameLine();
	if (NgButton(TEXT("Choose landing site"), false, ImVec2(260.0f * Scale, 0.0f), true) && Picked)
	{
		StartNewColony(true);
	}
	NgEndPanel();
}

// ---- Debug scenes -----------------------------------------------------------------------------------

void UColonyHudSubsystem::DrawDebugPage(float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const ImVec2 Size(FMath::Min(1000.0f * Scale, Viewport->Size.x - 80.0f * Scale), FMath::Min(640.0f * Scale, Viewport->Size.y - 120.0f * Scale));
	const ImVec2 Pos(Viewport->Pos.x + (Viewport->Size.x - Size.x) * 0.5f, Viewport->Pos.y + (Viewport->Size.y - Size.y) * 0.5f);
	if (!NgBeginPanel("##NgDebug", Pos, Size))
	{
		NgEndPanel();
		return;
	}
	NgTitle(TEXT("Debug scenes"));
	ColonyHud::TextColored(NgStyle::Dim, TEXT("Test scenes inside this game, so every update reaches them. F10 in a scene comes back here."));
	NgRule();

	const TConstArrayView<DebugScenes::FScene> Scenes = DebugScenes::All();
	DebugScenePicked = FMath::Clamp(DebugScenePicked, 0, Scenes.Num() - 1);
	const float ListW = 340.0f * Scale;
	const float FooterH = ImGui::GetFrameHeightWithSpacing() * 1.6f;
	ImGui::BeginChild("##NgDebugList", ImVec2(ListW, -FooterH), ImGuiChildFlags_None);
	const TCHAR* Group = nullptr;
	for (int32 i = 0; i < Scenes.Num(); ++i)
	{
		if (!Group || FCString::Strcmp(Group, Scenes[i].Group) != 0)
		{
			Group = Scenes[i].Group;
			NgLabel(FCString::Strcmp(Group, TEXT("Play")) == 0 ? TEXT("Play") : TEXT("Watch (ends by itself)"));
		}
		if (NgButton(FString::Printf(TEXT("%s##dbg%d"), Scenes[i].Label, i), i == DebugScenePicked, ImVec2(ListW - 8.0f * Scale, 0.0f)))
		{
			DebugScenePicked = i;
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("##NgDebugInfo", ImVec2(0.0f, -FooterH), ImGuiChildFlags_None);
	const DebugScenes::FScene& Picked = Scenes[DebugScenePicked];
	NgHeading(Picked.Label);
	ImGui::PushTextWrapPos(0.0f);
	ColonyHud::Text(Picked.Tip);
	ImGui::Spacing();
	ColonyHud::TextColored(NgStyle::Dim, TEXT("The game restarts into the scene, which takes as long as starting the game. F10 in the scene, or the scene ending, restarts it back to this title screen."));
	ImGui::PopTextWrapPos();
	ImGui::EndChild();
	NgRule();
	const float FooterRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	if (NgButton(TEXT("Back"), false, ImVec2(160.0f * Scale, 0.0f)))
	{
		MainMenuPage = 0;
	}
	ImGui::SameLine(FooterRight - 260.0f * Scale);
	if (NgButton(TEXT("Start scene"), false, ImVec2(260.0f * Scale, 0.0f), true))
	{
		DebugScenes::Launch(Picked);
	}
	NgEndPanel();
}

// ---- Character creation: You, then your companions ------------------------------------------------

void UColonyHudSubsystem::DrawPeopleNames(const colony::DefDatabase& Defs, const colony::Colonist& Colonist, colony::Entity Picked, int32 Slot, float Scale)
{
	if (PeopleNamesFor != Picked)
	{
		PeopleNamesFor = Picked;
		FCStringAnsi::Strncpy(PeopleFirst, Colonist.firstName.c_str(), UE_ARRAY_COUNT(PeopleFirst));
		FCStringAnsi::Strncpy(PeopleNick, Colonist.nickName.c_str(), UE_ARRAY_COUNT(PeopleNick));
		FCStringAnsi::Strncpy(PeopleLast, Colonist.lastName.c_str(), UE_ARRAY_COUNT(PeopleLast));
	}
	const float Gap = NgStyle::kGap * Scale;
	const float Third = (ImGui::GetContentRegionAvail().x - Gap * 2.0f) / 3.0f;
	bool bNames = false;
	const auto Field = [&](const TCHAR* Label, const char* Id, char* Buffer, bool bLast)
	{
		ImGui::BeginGroup();
		NgLabel(Label);
		ImGui::SetNextItemWidth(Third);
		ImGui::InputText(Id, Buffer, 25);
		bNames |= ImGui::IsItemDeactivatedAfterEdit();
		ImGui::EndGroup();
		if (!bLast)
		{
			ImGui::SameLine(0.0f, Gap);
		}
	};
	Field(TEXT("First"), "##first", PeopleFirst, false);
	Field(TEXT("Nickname"), "##nick", PeopleNick, false);
	Field(TEXT("Last"), "##last", PeopleLast, true);
	if (NgButton(TEXT("Random name"), false, ImVec2(-1.0f, 0.0f)))
	{
		const std::string First = NgRandomName(Defs, Colonist.female ? "FirstNamesFemale" : "FirstNamesMale");
		const std::string Last = NgRandomName(Defs, "LastNames");
		const std::string Nick = FMath::RandBool() ? NgRandomName(Defs, "Nicknames") : First;
		FCStringAnsi::Strncpy(PeopleFirst, First.c_str(), UE_ARRAY_COUNT(PeopleFirst));
		FCStringAnsi::Strncpy(PeopleNick, Nick.c_str(), UE_ARRAY_COUNT(PeopleNick));
		FCStringAnsi::Strncpy(PeopleLast, Last.c_str(), UE_ARRAY_COUNT(PeopleLast));
		bNames = true;
	}
	if (bNames && PeopleFirst[0] != 0)
	{
		colony::EditColonistCommand Edit;
		Edit.slot = Slot;
		Edit.firstName = PeopleFirst;
		Edit.nickName = PeopleNick[0] != 0 ? PeopleNick : PeopleFirst;
		Edit.lastName = PeopleLast;
		Submit(Edit);
	}
}

void UColonyHudSubsystem::DrawPeopleLooks(const colony::Sim& Sim, colony::Entity Picked, int32 Slot, bool bYou, float Scale)
{
	// The creator's sliders for the human body, applied live and sent when let go.
	const uint32 Key = static_cast<uint32>(entt::to_integral(Picked));
	CreatorPeople::FLook Look;
	const colony::PawnLooks* SimLooks = Sim.things().registry().try_get<colony::PawnLooks>(Picked);
	const FString* Local = PeopleLooks.Find(Key);
	const FString Text = Local ? *Local : (SimLooks ? FString(UTF8_TO_TCHAR(SimLooks->creator.c_str())) : FString());
	if (!CreatorPeople::Decode(Text, Look))
	{
		ColonyHud::TextColored(NgStyle::Faint, TEXT("Getting the body ready..."));
		return;
	}
	const CreatorPeople::FLook Full = CreatorPeople::Expanded(Look);
	const float Gap = NgStyle::kGap * Scale;
	bool bLive = false;  // changed this frame (show it now)
	bool bSend = false;  // finished a change (send it to the sim)
	const auto Slider = [&](const TCHAR* Label, const TCHAR* Name, float Min, float Max)
	{
		float Value = Full.Get(Name, 0.0f);
		NgLabel(Label);
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::SliderFloat(TCHAR_TO_UTF8(*FString::Printf(TEXT("##%s"), Name)), &Value, Min, Max, ""))
		{
			Look.Values.Add(Name, Value);
			bLive = true;
		}
		bSend |= ImGui::IsItemDeactivatedAfterEdit();
	};
	const auto SetGender = [&](bool bFemale)
	{
		colony::EditColonistCommand Edit;
		Edit.slot = Slot;
		Edit.female = bFemale ? 1 : 0;
		Submit(Edit);
		PeopleNamesFor = colony::kNoEntity;
		Look.bFemale = bFemale;
		Look.Values.Remove(TEXT("breasts"));
		Look.Values.Remove(TEXT("hairStyle"));
		bSend = true;
	};
	const float Half = (ImGui::GetContentRegionAvail().x - Gap) * 0.5f;
	if (NgButton(TEXT("Woman"), Look.bFemale, ImVec2(Half, 0.0f)) && !Look.bFemale)
	{
		SetGender(true);
	}
	ImGui::SameLine(0.0f, Gap);
	if (NgButton(TEXT("Man"), !Look.bFemale, ImVec2(Half, 0.0f)) && Look.bFemale)
	{
		SetGender(false);
	}
	Slider(TEXT("Height"), TEXT("height"), Look.bFemale ? 0.86f : 0.92f, Look.bFemale ? 1.04f : 1.12f);
	Slider(TEXT("Thin to heavy"), TEXT("weight"), -0.8f, 0.9f);
	Slider(TEXT("Soft to muscular"), TEXT("muscle"), -0.7f, 0.8f);
	if (Look.bFemale)
	{
		Slider(TEXT("Breasts"), TEXT("breasts"), -0.9f, 0.9f);
	}
	Slider(Look.bFemale ? TEXT("More masculine") : TEXT("More feminine"), TEXT("sex"), 0.0f, 0.5f);
	if (!bYou)
	{
		Slider(TEXT("Older"), TEXT("older"), 0.0f, 0.4f);  // the companions are 18: a little weathered at most
	}
	else
	{
		Slider(TEXT("Older"), TEXT("older"), 0.0f, 0.8f);
	}
	const float Swatch = 26.0f * Scale;
	const auto Swatches = [&](const TCHAR* Label, const TCHAR* Prefix, const float (*Colours)[3], int32 Count, const CreatorPeople::FLook& Against)
	{
		ImGui::Dummy(ImVec2(1.0f, Gap * 0.5f));
		NgLabel(Label);
		for (int32 i = 0; i < Count; ++i)
		{
			if (i > 0)
			{
				ImGui::SameLine(0.0f, Gap * 0.6f);
			}
			if (NgSwatch(TCHAR_TO_UTF8(*FString::Printf(TEXT("##%s%d"), Prefix, i)), Colours[i], NgSame(Against, Prefix, Colours[i]), Swatch))
			{
				NgSetColour(Look, Prefix, Colours[i]);
				bLive = bSend = true;
			}
		}
	};
	Swatches(TEXT("Skin"), TEXT("skin"), NgSkinSwatches, static_cast<int32>(UE_ARRAY_COUNT(NgSkinSwatches)), Full);
	ImGui::Dummy(ImVec2(1.0f, Gap * 0.5f));
	NgLabel(TEXT("Hair style"));
	{
		UWorld* World = GetWorld();
		USimPawnSubsystem* Pawns = World ? World->GetSubsystem<USimPawnSubsystem>() : nullptr;
		const ASimPawnActor* Actor = Pawns ? Pawns->FindPawn(Picked) : nullptr;
		const int32 Current = FMath::RoundToInt(Full.Get(TEXT("hairStyle"), 0.0f));
		const float Quarter = (ImGui::GetContentRegionAvail().x - Gap * 3.0f) / 4.0f;
		int32 Shown = 0;
		for (int32 h = 0; h < CreatorPeople::HairStyleCount(); ++h)
		{
			if (!Actor || !CreatorPeople::HasHairStyle(h, *Actor))
			{
				continue;
			}
			if (Shown++ % 4 != 0)
			{
				ImGui::SameLine(0.0f, Gap);
			}
			if (NgButton(FString::Printf(TEXT("%s##hair"), *CreatorPeople::HairStyleLabel(h)), Current == h, ImVec2(Quarter, 0.0f)))
			{
				Look.Values.Add(TEXT("hairStyle"), static_cast<float>(h));
				bLive = bSend = true;
			}
		}
	}
	Swatches(TEXT("Hair colour"), TEXT("hair"), NgHairSwatches, static_cast<int32>(UE_ARRAY_COUNT(NgHairSwatches)), Full);
	Swatches(TEXT("Eyes"), TEXT("iris"), NgEyeSwatches, static_cast<int32>(UE_ARRAY_COUNT(NgEyeSwatches)), Look);
	ImGui::Dummy(ImVec2(1.0f, Gap * 0.5f));
	NgLabel(TEXT("Body hair below"));
	static const TCHAR* const PubicNames[] = {TEXT("None"), TEXT("Natural"), TEXT("Trimmed"), TEXT("Shaved")};
	const int32 Pubic = FMath::RoundToInt(Full.Get(TEXT("pubic"), 1.0f));
	const float Quarter = (ImGui::GetContentRegionAvail().x - Gap * 3.0f) / 4.0f;
	for (int32 i = 0; i < 4; ++i)
	{
		if (i > 0)
		{
			ImGui::SameLine(0.0f, Gap);
		}
		if (NgButton(FString::Printf(TEXT("%s##pubic"), PubicNames[i]), Pubic == i, ImVec2(Quarter, 0.0f)))
		{
			Look.Values.Add(TEXT("pubic"), static_cast<float>(i));
			bLive = bSend = true;
		}
	}
	ImGui::Dummy(ImVec2(1.0f, Gap));
	if (bYou)
	{
		// Back to the creator's own woman (or man): the look that follows every hair and body update.
		if (NgButton(TEXT("Default look"), Look.Seed == CreatorPeople::kDefaultSeed && Look.Values.Num() == 0, ImVec2(Half, 0.0f)))
		{
			Look.Values.Reset();
			Look.Seed = CreatorPeople::kDefaultSeed;
			bLive = bSend = true;
		}
		ImGui::SameLine(0.0f, Gap);
	}
	if (NgButton(TEXT("Random looks"), false, ImVec2(bYou ? Half : -1.0f, 0.0f)))
	{
		Look.Values.Reset();
		Look.Seed = FMath::RandRange(1, 0x7ffffff);
		bLive = bSend = true;
	}
	if (bLive || bSend)
	{
		PeopleLooks.Add(Key, CreatorPeople::Encode(Look));
		if (UWorld* World = GetWorld())
		{
			if (USimPawnSubsystem* Pawns = World->GetSubsystem<USimPawnSubsystem>())
			{
				if (ASimPawnActor* Actor = Pawns->FindPawn(Picked))
				{
					CreatorPeople::Apply(Look, *Actor);
				}
			}
		}
	}
	if (bSend)
	{
		SubmitPeopleLook(Picked, Slot, Look);
	}
}

void UColonyHudSubsystem::DrawTraitShop(const colony::Sim& Sim, const colony::Colonist& Colonist, int32 Slot, float Scale)
{
	const colony::DefDatabase& Defs = Sim.defs();
	const int32 Pool = Sim.scenarioTraitPoints();
	const float Gap = NgStyle::kGap * Scale;
	if (Pool < 0)
	{
		ColonyHud::TextColored(NgStyle::Faint, TEXT("This scenario's people come with their own stories."));
		return;
	}
	const std::vector<colony::TraitRecord>& Have = Colonist.traits;
	int32 Spent = 0;
	int32 Earned = 0;
	for (const colony::TraitRecord& T : Have)
	{
		const int32 Cost = colony::traitPointCost(Defs, T);
		Spent += FMath::Max(0, Cost);
		Earned += FMath::Max(0, -Cost);
	}
	const int32 Left = Pool + Earned - Spent;
	const auto Send = [&](const std::vector<colony::TraitRecord>& Traits)
	{
		colony::SetColonistTraitsCommand Command;
		Command.slot = Slot;
		Command.traits = NgTraitsCommandText(Traits);
		Submit(Command);
	};

	// The points: what is left in large ember figures, what was spent and earned under it.
	ImGui::BeginGroup();
	NgSized(NgStyle::kTitle, Left > 0 ? NgStyle::Ember : NgStyle::Jade, FString::Printf(TEXT("%d"), Left));
	ImGui::EndGroup();
	ImGui::SameLine(0.0f, Gap * 1.5f);
	ImGui::BeginGroup();
	ImGui::Dummy(ImVec2(1.0f, Gap * 0.6f));
	NgLabel(TEXT("Trait points left"));
	ColonyHud::TextColored(NgStyle::Faint, FString::Printf(TEXT("%d to start, %d spent, %d from drawbacks"), Pool, Spent, Earned));
	ImGui::EndGroup();
	const float Half = (ImGui::GetContentRegionAvail().x - Gap) * 0.5f;
	if (NgButton(TEXT("Random traits"), false, ImVec2(Half, 0.0f)))
	{
		colony::SetColonistTraitsCommand Command;
		Command.slot = Slot;
		Command.randomize = true;
		Command.roll = ++PeopleTraitRoll + static_cast<uint32>(FMath::RandRange(0, 0xffff)) * 65536u;
		Submit(Command);
	}
	ColonyHud::Tip(TEXT("Spends all the points at random"));
	ImGui::SameLine(0.0f, Gap);
	if (NgButton(TEXT("Clear"), false, ImVec2(Half, 0.0f)))
	{
		Send({});
	}

	// Chosen: click one to take it back.
	ImGui::Dummy(ImVec2(1.0f, Gap * 0.5f));
	NgLabel(FString::Printf(TEXT("Traits (%d)"), static_cast<int32>(Have.size())));
	if (Have.empty())
	{
		ColonyHud::TextColored(NgStyle::Faint, TEXT("None yet."));
	}
	float LineX = 0.0f;
	const float Room = ImGui::GetContentRegionAvail().x;
	for (size_t i = 0; i < Have.size(); ++i)
	{
		FString Label;
		FString Description;
		NgTraitText(Defs, Have[i], Label, Description);
		const int32 Cost = colony::traitPointCost(Defs, Have[i]);
		const FString Shown = FString::Printf(TEXT("%s  %s%d  x"), *Label, Cost < 0 ? TEXT("+") : TEXT("-"), FMath::Abs(Cost));
		const FString Chip = FString::Printf(TEXT("%s##have%d"), *Shown, static_cast<int32>(i));
		const float W = ImGui::CalcTextSize(TCHAR_TO_UTF8(*Shown)).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		if (i > 0 && LineX + W < Room)
		{
			ImGui::SameLine(0.0f, Gap * 0.6f);
		}
		else
		{
			LineX = 0.0f;
		}
		LineX += W + Gap * 0.6f;
		if (NgButton(Chip, true))
		{
			std::vector<colony::TraitRecord> Fewer = Have;
			Fewer.erase(Fewer.begin() + static_cast<std::ptrdiff_t>(i));
			Send(Fewer);
		}
		ColonyHud::Tip(Description.IsEmpty() ? TEXT("Click to take it back") : Description + TEXT("\nClick to take it back"));
	}

	// The shop: everything one more could be, costs on the right; drawbacks give points.
	struct FOffer
	{
		colony::TraitRecord Trait;
		int32 Cost;
		FString Label;
		FString Description;
		bool bFits;
		FString Why;
	};
	std::vector<FOffer> Offers;
	for (const colony::DefId Id : Defs.idsOfKind("Traits"))
	{
		const colony::Def& Def = Defs.get(Id);
		if (Def.flag("abstract"))
		{
			continue;
		}
		for (size_t d = 0; d < Def.count("degreeDatas"); ++d)
		{
			const std::string Base = "degreeDatas." + std::to_string(d);
			if (!Def.has((Base + ".points").c_str()))
			{
				continue;
			}
			FOffer Offer;
			Offer.Trait = colony::TraitRecord{Def.defName(), static_cast<int>(Def.integer((Base + ".degree").c_str(), 0))};
			if (std::any_of(Have.begin(), Have.end(), [&](const colony::TraitRecord& T) { return T.def == Offer.Trait.def && T.degree == Offer.Trait.degree; }))
			{
				continue;
			}
			Offer.Cost = colony::traitPointCost(Defs, Offer.Trait);
			NgTraitText(Defs, Offer.Trait, Offer.Label, Offer.Description);
			// Another degree of a trait they have swaps for it.
			std::vector<colony::TraitRecord> With;
			for (const colony::TraitRecord& T : Have)
			{
				if (T.def != Offer.Trait.def)
				{
					With.push_back(T);
				}
			}
			With.push_back(Offer.Trait);
			std::string Why;
			Offer.bFits = colony::traitSetFits(Defs, With, Pool, Why);
			Offer.Why = UTF8_TO_TCHAR(Why.c_str());
			Offers.push_back(MoveTemp(Offer));
		}
	}
	std::sort(Offers.begin(), Offers.end(), [](const FOffer& A, const FOffer& B) { return A.Cost != B.Cost ? A.Cost > B.Cost : A.Label < B.Label; });
	const auto Shop = [&](const TCHAR* Heading, bool bDrawbacks)
	{
		NgHeading(Heading);
		const float Cost = 46.0f * Scale;
		for (size_t i = 0; i < Offers.size(); ++i)
		{
			const FOffer& Offer = Offers[i];
			if ((Offer.Cost < 0) != bDrawbacks)
			{
				continue;
			}
			ImGui::PushID(static_cast<int32>(i));
			ImGui::BeginDisabled(!Offer.bFits);
			const bool bTake = NgButton(Offer.Label, false, ImVec2(ImGui::GetContentRegionAvail().x - Cost, 0.0f));
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			{
				const FString Tip = Offer.bFits ? Offer.Description : Offer.Description + TEXT("\n") + Offer.Why;
				ImGui::SetTooltip("%s", TCHAR_TO_UTF8(*Tip));
			}
			ImGui::SameLine();
			ColonyHud::TextColored(!Offer.bFits ? NgStyle::Faint : (bDrawbacks ? NgStyle::Jade : NgStyle::Ember),
				FString::Printf(TEXT("%s%d"), bDrawbacks ? TEXT("+") : TEXT(""), FMath::Abs(Offer.Cost)));
			if (bTake)
			{
				std::vector<colony::TraitRecord> With;
				for (const colony::TraitRecord& T : Have)
				{
					if (T.def != Offer.Trait.def)
					{
						With.push_back(T);
					}
				}
				With.push_back(Offer.Trait);
				Send(With);
			}
			ImGui::PopID();
		}
	};
	ImGui::BeginChild("##NgShop", ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.52f), ImGuiChildFlags_None);
	Shop(TEXT("Strengths (cost points)"), false);
	Shop(TEXT("Drawbacks (give points)"), true);
	ImGui::EndChild();

	// Skills: bars to 20; a skill trait's levels show past it in jade (up to 23).
	NgHeading(TEXT("Skills"));
	ImGui::BeginChild("##NgSkills", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
	const float LabelW = 110.0f * Scale;
	const float BarW = ImGui::GetContentRegionAvail().x - LabelW - 92.0f * Scale;
	for (const colony::SkillRecord& Skill : Colonist.skills)
	{
		ColonyHud::TextColored(Skill.disabled ? NgStyle::Faint : NgStyle::Text, NgDefLabel(Defs, "Skills", Skill.def));
		ImGui::SameLine(LabelW);
		if (Skill.disabled)
		{
			ColonyHud::TextColored(NgStyle::Faint, TEXT("won't do"));
			continue;
		}
		const ImVec2 P = ImGui::GetCursorScreenPos();
		const float H = ImGui::GetTextLineHeight() * 0.45f;
		ImDrawList* Draw = ImGui::GetWindowDrawList();
		const float Y = P.y + (ImGui::GetTextLineHeight() - H) * 0.5f;
		const int32 Base = FMath::Clamp(Skill.level - FMath::Max(0, Skill.bonus), 0, 20);
		Draw->AddRectFilled(ImVec2(P.x, Y), ImVec2(P.x + BarW, Y + H), IM_COL32(30, 38, 34, 220));
		Draw->AddRectFilled(ImVec2(P.x, Y), ImVec2(P.x + BarW * Base / 20.0f, Y + H), ImGui::GetColorU32(NgStyle::Dim));
		if (Skill.level > Base)
		{
			// The trait's levels, in jade, running past the end of the bar when they pass 20.
			Draw->AddRectFilled(ImVec2(P.x + BarW * Base / 20.0f, Y), ImVec2(P.x + BarW * Skill.level / 20.0f, Y + H), ImGui::GetColorU32(NgStyle::Jade));
		}
		ImGui::Dummy(ImVec2(BarW + 18.0f * Scale, ImGui::GetTextLineHeight()));
		ImGui::SameLine();
		ColonyHud::TextColored(Skill.bonus > 0 ? NgStyle::Jade : NgStyle::Text, FString::Printf(TEXT("%2d"), Skill.level));
		ImGui::SameLine();
		NgFlames(Skill.passion, ImGui::GetTextLineHeight());
	}
	if (!Colonist.disabledWorkTags.empty())
	{
		FString Off;
		for (const std::string& Tag : Colonist.disabledWorkTags)
		{
			Off += (Off.IsEmpty() ? TEXT("") : TEXT(", ")) + FString(UTF8_TO_TCHAR(Tag.c_str())).ToLower();
		}
		ColonyHud::TextColored(NgStyle::Bad, FString::Printf(TEXT("Won't do: %s"), *Off));
	}
	ImGui::EndChild();
}

void UColonyHudSubsystem::DrawPeoplePage(USimSubsystem& Bridge, float Scale)
{
	const colony::Sim* Sim = Bridge.GetSim();
	const colony::DefDatabase* Defs = Bridge.GetDefs().get();
	if (!Sim || !Defs || Bridge.GetSimGeneration() != PeopleGeneration)
	{
		MainMenuPage = 0;
		EndPeopleCamera();
		return;
	}
	// Rerolls and edits apply at once while the game waits at its first second.
	Bridge.ApplyPregameCommands();
	UpdateMugshots(Bridge);
	const std::vector<colony::Entity> People = Sim->startingColonistSlots();
	if (People.empty())
	{
		ColonyHud::Text(TEXT("No colonists landed (see the log)."));
		return;
	}
	const entt::registry& Reg = Sim->things().registry();
	const bool bYouPage = MainMenuPage == 3;
	PeopleSelected = bYouPage ? 0 : FMath::Clamp(PeopleSelected, 1, FMath::Max(1, static_cast<int32>(People.size()) - 1));
	PeopleSelected = FMath::Min(PeopleSelected, static_cast<int32>(People.size()) - 1);
	const colony::Entity Picked = People[static_cast<size_t>(PeopleSelected)];

	// Every colonist gets a creator look the first time it is seen (the 3D body the creator draws). You start as the
	// creator's own woman, the gun range's, untouched, so every hair and body update shows on you; the strangers are
	// random people seeded by the game, their slot and their rerolls.
	for (int32 Slot = 0; Slot < static_cast<int32>(People.size()); ++Slot)
	{
		const colony::Entity Pawn = People[static_cast<size_t>(Slot)];
		const colony::PawnLooks* Looks = Reg.try_get<colony::PawnLooks>(Pawn);
		const colony::Colonist* Colonist = Sim->colonistOf(Pawn);
		const uint32 Key = static_cast<uint32>(entt::to_integral(Pawn));
		if (Looks && Colonist && Looks->creator.empty() && !PeopleLooks.Contains(Key))
		{
			CreatorPeople::FLook Look;
			if (Colonist->protagonist)
			{
				Look.Seed = CreatorPeople::kDefaultSeed;
				Look.bFemale = true;
				const FString Text = CreatorPeople::Encode(Look);
				PeopleLooks.Add(Key, Text);
				colony::EditColonistCommand Edit;
				Edit.slot = Slot;
				Edit.female = 1;
				Edit.setCreator = true;
				Edit.creator = TCHAR_TO_UTF8(*Text);
				Submit(Edit);
			}
			else
			{
				Look.Seed = static_cast<int32>((Sim->settings().seed * 7919u + static_cast<uint64>(Slot) * 104729u + static_cast<uint64>(PeopleRerolls) * 15485863u) & 0x7fffffff);
				Look.bFemale = Colonist->female;
				SubmitPeopleLook(Pawn, Slot, Look);
			}
		}
	}
	// Naked by default (they land so); "clothes on" shows the cargo pack's cotton shirt and trousers on everyone.
	if (USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
	{
		TArray<FString> Cotton;
		for (const TCHAR* Piece : NgCottonClothes)
		{
			Cotton.Add(Piece);
		}
		for (const colony::Entity Pawn : People)
		{
			if (bPeopleClothes)
			{
				Pawns->SetApparelPreview(Pawn, Cotton);
			}
			else
			{
				Pawns->ClearApparelPreview(Pawn);
			}
			if (Pawn != Picked)
			{
				if (ASimPawnActor* Other = Pawns->FindPawn(Pawn))
				{
					Other->SetPreviewSpin(0.0f);
				}
			}
		}
	}
	UpdatePeopleCamera(Picked);

	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float Pad = NgStyle::kPad * Scale;
	const float Gap = NgStyle::kGap * Scale;
	const float Left = Viewport->Pos.x + Pad;
	const float Right = Viewport->Pos.x + Viewport->Size.x - Pad;
	const float Top = Viewport->Pos.y + Pad;
	const float FooterH = 62.0f * Scale;
	const float Bottom = Viewport->Pos.y + Viewport->Size.y - Pad - FooterH;
	const float SideW = 420.0f * Scale;
	const float InfoW = 460.0f * Scale;
	const colony::Colonist* Colonist = Sim->colonistOf(Picked);

	if (bYouPage)
	{
		// Left: who you are (nobody you remember).
		if (NgBeginPanel("##NgYou", ImVec2(Left, Top), ImVec2(SideW, Bottom - Top - Gap)))
		{
			NgTitle(TEXT("Who you are"));
			ImGui::PushTextWrapPos(0.0f);
			ColonyHud::TextColored(NgStyle::Dim, TEXT("You wake in the wreck with no memory: not your name, not how you got here, not who the two "
				"beside you are. Only this body is yours."));
			ImGui::PopTextWrapPos();
			NgHeading(TEXT("Your name"));
			if (Colonist)
			{
				DrawPeopleNames(*Defs, *Colonist, Picked, 0, Scale);
			}
			NgHeading(TEXT("What you can do"));
			ImGui::PushTextWrapPos(0.0f);
			ColonyHud::TextColored(NgStyle::Faint, TEXT("Nothing to choose here. Whatever you knew before the crash, you will find out by doing."));
			ImGui::PopTextWrapPos();
		}
		NgEndPanel();
		// Right: the looks.
		if (NgBeginPanel("##NgYouLooks", ImVec2(Right - InfoW, Top), ImVec2(InfoW, Bottom - Top - Gap)))
		{
			NgHeading(TEXT("Your looks"));
			DrawPeopleLooks(*Sim, Picked, 0, true, Scale);
		}
		NgEndPanel();
	}
	else
	{
		// Header: the title and the races (only humans for now, the rest shown and locked).
		const float HeadH = 92.0f * Scale;
		if (NgBeginPanel("##NgCompanionsHead", ImVec2(Left, Top), ImVec2(Right - Left, HeadH)))
		{
			NgTitle(TEXT("Your companions"));
			ImGui::SameLine(SideW + Pad);
			ImGui::BeginGroup();
			ColonyHud::TextColored(NgStyle::Dim, TEXT("Two strangers, both 18. Shape them, and choose what they are good and bad at."));
			for (const FNgRace& Race : NgRaces)
			{
				const colony::Def* Lineage = Defs->find("Lineages", Race.Lineage);
				const bool bOpen = FCStringAnsi::Strcmp(Race.Lineage, "Baseline") == 0;
				const FString Label = bOpen ? FString(Race.Fallback) : (Lineage ? NgCapital(Lineage->label()) : FString(Race.Fallback));
				ImGui::BeginDisabled(!bOpen);
				NgButton(bOpen ? Label : FString::Printf(TEXT("%s  (locked)"), *Label), bOpen, ImVec2(150.0f * Scale, 0.0f));
				ImGui::EndDisabled();
				if (!bOpen && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				{
					ImGui::SetTooltip("Not playable yet: this race comes later.");
				}
				ImGui::SameLine(0.0f, Gap);
			}
			ImGui::NewLine();
			ImGui::EndGroup();
		}
		NgEndPanel();
		const float BodyTop = Top + HeadH + Gap;

		// Left: the two strangers as cards.
		if (NgBeginPanel("##NgCompanionsCards", ImVec2(Left, BodyTop), ImVec2(SideW, Bottom - BodyTop - Gap)))
		{
			NgHeading(TEXT("The strangers"));
			const float Face = 84.0f * Scale;
			for (int32 Slot = 1; Slot < static_cast<int32>(People.size()); ++Slot)
			{
				const colony::Entity Pawn = People[static_cast<size_t>(Slot)];
				const colony::Colonist* Stranger = Sim->colonistOf(Pawn);
				if (!Stranger)
				{
					continue;
				}
				ImGui::PushID(Slot);
				const bool bPicked = Slot == PeopleSelected;
				ImGui::PushStyleColor(ImGuiCol_ChildBg, bPicked ? NgStyle::JadeDeep : ImVec4(0.05f, 0.08f, 0.07f, 0.7f));
				ImGui::PushStyleColor(ImGuiCol_Border, bPicked ? NgStyle::Jade : NgStyle::PanelEdge);
				ImGui::BeginChild("##card", ImVec2(0.0f, Face + 22.0f * Scale), ImGuiChildFlags_Borders);
				if (UTexture* Mugshot = GetMugshot(Pawn))
				{
					ImGui::Image(ImTextureRef(Mugshot), ImVec2(Face, Face));
				}
				else
				{
					ImGui::Dummy(ImVec2(Face, Face));
				}
				if (ImGui::IsItemClicked())
				{
					PeopleSelected = Slot;
				}
				ImGui::SameLine(0.0f, Gap);
				ImGui::BeginGroup();
				NgSized(NgStyle::kHeading, NgStyle::Text, FString::Printf(TEXT("%s %s"), UTF8_TO_TCHAR(Stranger->firstName.c_str()), UTF8_TO_TCHAR(Stranger->lastName.c_str())));
				ColonyHud::TextColored(NgStyle::Dim, FString::Printf(TEXT("%s, %d, %d traits"), Stranger->female ? TEXT("Woman") : TEXT("Man"), Stranger->age,
					static_cast<int32>(Stranger->traits.size())));
				if (NgButton(TEXT("Shape"), bPicked))
				{
					PeopleSelected = Slot;
				}
				ImGui::SameLine(0.0f, Gap);
				if (NgButton(TEXT("Reroll")))
				{
					++PeopleRerolls;
					PeopleLooks.Remove(static_cast<uint32>(entt::to_integral(Pawn)));
					PeopleNamesFor = colony::kNoEntity;
					Submit(colony::RerollColonistCommand{Slot});
				}
				ColonyHud::Tip(TEXT("Someone else: new name, traits, skills and looks"));
				ImGui::EndGroup();
				ImGui::EndChild();
				ImGui::PopStyleColor(2);
				ImGui::PopID();
				ImGui::Dummy(ImVec2(1.0f, Gap * 0.5f));
			}
			if (NgButton(TEXT("Reroll both"), false, ImVec2(-1.0f, 0.0f)))
			{
				for (int32 Slot = 1; Slot < static_cast<int32>(People.size()); ++Slot)
				{
					++PeopleRerolls;
					PeopleLooks.Remove(static_cast<uint32>(entt::to_integral(People[static_cast<size_t>(Slot)])));
					Submit(colony::RerollColonistCommand{Slot});
				}
				PeopleNamesFor = colony::kNoEntity;
			}
			if (Colonist)
			{
				NgHeading(TEXT("Name"));
				DrawPeopleNames(*Defs, *Colonist, Picked, PeopleSelected, Scale);
			}
		}
		NgEndPanel();

		// Right: traits or looks of the one being shaped.
		if (Colonist && NgBeginPanel("##NgCompanionsInfo", ImVec2(Right - InfoW, BodyTop), ImVec2(InfoW, Bottom - BodyTop - Gap)))
		{
			const float Half = (ImGui::GetContentRegionAvail().x - Gap) * 0.5f;
			if (NgButton(TEXT("Traits and skills"), PeopleTab == 0, ImVec2(Half, 0.0f)))
			{
				PeopleTab = 0;
			}
			ImGui::SameLine(0.0f, Gap);
			if (NgButton(TEXT("Looks"), PeopleTab == 1, ImVec2(Half, 0.0f)))
			{
				PeopleTab = 1;
			}
			NgRule();
			if (PeopleTab == 0)
			{
				DrawTraitShop(*Sim, *Colonist, PeopleSelected, Scale);
			}
			else
			{
				DrawPeopleLooks(*Sim, Picked, PeopleSelected, false, Scale);
			}
		}
		if (Colonist)
		{
			NgEndPanel();
		}
	}

	// Under the body: how to look at it, and the clothes.
	const float StripW = 520.0f * Scale;
	if (NgBeginPanel("##NgPeopleStrip", ImVec2(Viewport->Pos.x + (Viewport->Size.x - StripW) * 0.5f, Bottom - 50.0f * Scale - Gap), ImVec2(StripW, 50.0f * Scale)))
	{
		ImGui::Checkbox("##clothes", &bPeopleClothes);
		ImGui::SameLine();
		ColonyHud::TextColored(NgStyle::Text, TEXT("CLOTHES ON"));
		ColonyHud::Tip(TEXT("Shows the cotton shirt and trousers from the cargo pack. Everyone lands naked."));
		ImGui::SameLine(StripW * 0.42f);
		ColonyHud::TextColored(NgStyle::Faint, TEXT("Drag to spin, wheel to zoom"));
	}
	NgEndPanel();

	// Footer: back, the steps, and on.
	if (NgBeginPanel("##NgPeopleFoot", ImVec2(Left, Bottom), ImVec2(Right - Left, FooterH)))
	{
		const float FootLeft = ImGui::GetCursorPosX();
		const float FootRight = FootLeft + ImGui::GetContentRegionAvail().x;
		if (NgButton(TEXT("Back"), false, ImVec2(160.0f * Scale, 0.0f)))
		{
			if (bYouPage)
			{
				EndPeopleCamera();
				MainMenuPage = 1;
			}
			else
			{
				MainMenuPage = 3;
				PeopleNamesFor = colony::kNoEntity;
			}
		}
		ImGui::SameLine((FootLeft + FootRight) * 0.5f - 230.0f * Scale);
		NgSteps(bYouPage ? 2 : 3);
		ImGui::SameLine(FootRight - 280.0f * Scale);
		if (bYouPage)
		{
			if (NgButton(TEXT("Next: your companions"), false, ImVec2(280.0f * Scale, 0.0f), true))
			{
				MainMenuPage = 4;
				PeopleSelected = 1;
				PeopleTab = 0;
				PeopleNamesFor = colony::kNoEntity;
				PeopleSpin = 0.0f;
				PeopleSpinRate = 0.0f;
			}
		}
		else if (NgButton(TEXT("Land and start"), false, ImVec2(280.0f * Scale, 0.0f), true))
		{
			NgEndPanel();
			LandColony(Bridge);
			return;
		}
	}
	NgEndPanel();
}
