// The title screen and the new game (docs/design/new-game.md): title, scenario, landing site (the world screen in its
// new-game mode), then character creation in two parts on the colonists' real 3D bodies: "You" (who the amnesiac
// player is: looks and a name only) and "Your companions" (the two 18-year-old strangers: looks, names and traits bought
// from the scenario's point pool). Our own look, "Canopy" (sneed 2026-11-21: no reference UIs), described at NgStyle.

#include "ColonyHudSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/PointLight.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
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
#include "TitleStage.h"
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
	// 24000 cd: brighter (60000 until 2026-11-27) pushed lit dark skin past what the subsurface scattering keeps and it came
	// out white (the brightest of 0.3x/0.4x/0.5x with no white on the Melanesian preset; 0.5x clipped on the chest).
	constexpr float NgPreviewLampIntensity = 24000.0f;
	constexpr float NgPreviewClearMetres = 5.5f;
	// Plants within this many metres of a person on show are not drawn while the screen is open (a cosmetic clearing).
	constexpr float NgPreviewClearingMetres = 3.5f;
	// The character screen's camera tilt (degrees, down is negative), for the left-drag tilt and the right-button orbit,
	// and how far over the feet the eye must stay (cm).
	constexpr float NgPreviewPitchLow = -50.0f;
	constexpr float NgPreviewPitchHigh = 15.0f;
	// Holding the right button goes further: nearly straight down from above and straight up from under the feet.
	constexpr float NgPreviewOrbitPitchLow = -85.0f;
	constexpr float NgPreviewOrbitPitchHigh = 85.0f;
	constexpr double NgPreviewLowestEyeCm = 25.0;
	// The engine warns on screen when its exposure passes 12 (cached lighting would clip): the screen's hour is the one
	// nearest NgPreviewHour, earlier in the morning, whose exposure (ours, which reads a little above the engine's) stays
	// under this. At 6:30, 12.1 of ours showed no warning; 10:30 did.
	constexpr float NgPreviewMaxEV = 12.25f;

	// The title screen's camera (cosmetic): the glade it circles is the open spot with the fewest trees within
	// NgTitleGladeMetres; it circles at NgTitleOrbitMetres, NgTitleEyeHeightCm over the ground, with the glade
	// NgTitleSubjectOffsetDegrees right of the middle, in the morning (NgTitleHour, or earlier if too bright).
	constexpr float NgTitleGladeMetres = 16.0f;
	constexpr float NgTitleOrbitMetres = 14.0f;
	constexpr float NgTitleEyeHeightCm = 230.0f;
	constexpr float NgTitleFieldOfView = 55.0f;
	constexpr float NgTitleDegreesPerSecond = 1.6f;
	constexpr float NgTitleSubjectOffsetDegrees = 14.0f;
	constexpr float NgTitleHour = 7.25f;
	constexpr float NgTitleLetterSpacing = 0.16f;  // of the name's size, between its letters
	// The title's starship stage (ATitleStage) stands this high over the map's corner, far above anything drawn there.
	constexpr double NgTitleStageHeightCm = 3000000.0;

	// For finding and tuning the screen's light (sneed 2026-11-26: lit dark skin washed out to white on "You").
	TAutoConsoleVariable<float> GNgPreviewMaxEV(TEXT("NewGame.PreviewMaxEV"), NgPreviewMaxEV,
		TEXT("The character screen's sky is held at the hour nearest 10:30, earlier, whose exposure (EV100) stays under this."), ECVF_Default);
	TAutoConsoleVariable<float> GNgPreviewLamp(TEXT("NewGame.PreviewLamp"), 1.0f,
		TEXT("The character screen's key lamp, times its own brightness (0: off)."), ECVF_Default);
	TAutoConsoleVariable<float> GNgPreviewLampRadius(TEXT("NewGame.PreviewLampRadius"), 60.0f,
		TEXT("The character screen's key lamp: its source radius (cm)."), ECVF_Default);
	TAutoConsoleVariable<int32> GNgPreviewLampShadows(TEXT("NewGame.PreviewLampShadows"), 0,
		TEXT("The character screen's key lamp: 1 casts shadows."), ECVF_Default);
	TAutoConsoleVariable<int32> GNgPreviewLampTransmission(TEXT("NewGame.PreviewLampTransmission"), 1,
		TEXT("The character screen's key lamp: 1 shines through skin (subsurface transmission)."), ECVF_Default);

	TAutoConsoleVariable<int32> GNgTitleStage(TEXT("Title.Stage"), 1,
		TEXT("1: the title screen shows the starship stage (the character at a window in orbit); 0: the old camera round a rainforest glade."));

	/** The hour nearest `Preferred`, earlier in the morning, whose exposure stays under NgPreviewMaxEV (see there). */
	float NgCalmHour(const USkySubsystem& Sky, const USimSubsystem& Bridge, float Preferred)
	{
		if (!Bridge.GetSim())
		{
			return Preferred;
		}
		const float Day = static_cast<float>(FMath::Fmod(static_cast<double>(Bridge.GetGameSeconds()) / colony::kSecondsPerDay, static_cast<double>(colony::kDaysPerYear)));
		float Hour = Preferred;
		while (Hour > 6.5f && Sky.ComputeState(*Bridge.GetSim(), Hour, Day).ExposureEV + Sky.GetLook().DarkerEV > GNgPreviewMaxEV.GetValueOnGameThread())
		{
			Hour -= 0.25f;
		}
		return Hour;
	}

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
		{0.64f, 0.47f, 0.36f}, {0.48f, 0.34f, 0.25f}, {0.33f, 0.23f, 0.17f}, {0.27f, 0.185f, 0.135f}};  // as CreatorSkinRamp
	const float NgEyeSwatches[][3] = {{0.30f, 0.20f, 0.12f}, {0.45f, 0.32f, 0.18f}, {0.35f, 0.50f, 0.65f}, {0.40f, 0.55f, 0.35f},
		{0.50f, 0.55f, 0.55f}, {0.15f, 0.12f, 0.10f}};

	/**
	 * Skin as one slider from fair (0) to dark (1) (sneed 2026-11-25: "have skin as a slider"): the colour at any point
	 * between the swatches, blended. The skin material takes any colour (it shifts the skin texture's own average to it),
	 * so a point between two swatches is as good as a swatch.
	 */
	void NgSkinAt(float Tone, float* Out)
	{
		constexpr int32 Last = static_cast<int32>(UE_ARRAY_COUNT(NgSkinSwatches)) - 1;
		const float T = FMath::Clamp(Tone, 0.0f, 1.0f) * static_cast<float>(Last);
		const int32 K = FMath::Clamp(FMath::FloorToInt32(T), 0, Last - 1);
		const float F = T - static_cast<float>(K);
		for (int32 c = 0; c < 3; ++c)
		{
			Out[c] = FMath::Lerp(NgSkinSwatches[K][c], NgSkinSwatches[K + 1][c], F);
		}
	}

	/** Where a skin colour lies on the slider: the nearest point of the blend (a random person's skin has no tone yet). */
	float NgSkinToneOf(float R, float G, float B)
	{
		float BestTone = 0.0f;
		float BestDistance = TNumericLimits<float>::Max();
		constexpr int32 Steps = 240;
		for (int32 i = 0; i <= Steps; ++i)
		{
			const float Tone = static_cast<float>(i) / static_cast<float>(Steps);
			float C[3];
			NgSkinAt(Tone, C);
			const float Distance = FMath::Square(C[0] - R) + FMath::Square(C[1] - G) + FMath::Square(C[2] - B);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				BestTone = Tone;
			}
		}
		return BestTone;
	}

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
	PeopleOrbit = 0.0f;
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
		// The camera starts on you (the protagonist, else the first colonist), selected, not over the map's middle where
		// nobody is (sneed 2026-11-24: "i cannot see my character").
		Overseer->FocusOnColonists(true);
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
	// Called every frame on the other menu pages: only a screen that was shown has anything to end (the title keeps its
	// own clearings and light).
	if (!PeopleCamera.IsValid() && !PeopleLamp.IsValid())
	{
		return;
	}
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
						Actor->SetPreviewLive(false);
						Actor->SetBodyHidden(false);  // the screen showed only the one being shaped
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
		// Only the one being shaped stands on show (the others are hidden): the hidden ones are not in the way.
		const ASimPawnActor* OtherActor = Other != Pawn ? Pawns->FindPawn(Other) : nullptr;
		if (OtherActor && !OtherActor->IsBodyHidden())
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
		Sky->SetHourOverride(NgCalmHour(*Sky, *Bridge, NgPreviewHour));
	}
	// A clearing round each person on show, so no tree stands over them or between them and the camera.
	if (UTerrainSubsystem* Terrain = World->GetSubsystem<UTerrainSubsystem>(); Terrain && Bridge && Bridge->GetSim())
	{
		const FVector Origin = Bridge->SimToWorld(0.0f, 0.0f, 0);
		TArray<FVector4f> Clearings;
		for (const colony::Entity Shown : Bridge->GetSim()->startingColonistSlots())
		{
			if (const ASimPawnActor* ShownActor = Pawns->FindPawn(Shown))
			{
				const FVector P = (ShownActor->GetActorLocation() - Origin) / 100.0;
				Clearings.Add(FVector4f(static_cast<float>(P.X), static_cast<float>(P.Y), NgPreviewClearingMetres, 0.0f));
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
		PeopleOrbit = 0.0f;
		Actor->SetPreviewSpin(PeopleSpin);
	}
	// The turntable: dragging with the left button over the open middle spins the body itself (so soft tissue and hair
	// swing with it), and it keeps turning a little after the mouse lets go; up and down tilt the view. Holding the right
	// button carries the camera round the still body instead (sneed 2026-11-25: "holding right click should allow me to
	// orbit the character"), side to side all the way round, up and down within the tilt's limits. The wheel zooms.
	const ImGuiIO& IO = ImGui::GetIO();
	const float Delta = FMath::Clamp(IO.DeltaTime, 0.0f, 0.1f);
	// The right button is read from the player controller too, and the mouse's move from the cursor or, while the engine
	// holds the cursor still for a right-button drag, from the raw mouse: ImGui alone never heard it (sneed 2026-11-25:
	// "when i right click i cannot do a 360 orbit (or any orbit)").
	const bool bRightDown = ImGui::IsMouseDown(ImGuiMouseButton_Right) || PC->IsInputKeyDown(EKeys::RightMouseButton);
	float CursorX = 0.0f;
	float CursorY = 0.0f;
	const bool bHasCursor = PC->GetMousePosition(CursorX, CursorY);
	const FVector2D Cursor(CursorX, CursorY);
	FVector2D OrbitMove = bHasCursor && bPeopleHadCursor ? Cursor - PeopleLastCursor : FVector2D::ZeroVector;
	PeopleLastCursor = Cursor;
	bPeopleHadCursor = bHasCursor;
	if (OrbitMove.IsNearlyZero())
	{
		float RawX = 0.0f;
		float RawY = 0.0f;
		PC->GetInputMouseDelta(RawX, RawY);
		OrbitMove = FVector2D(RawX, -RawY) * 4.0;  // raw counts, up positive: about the same feel as cursor pixels
	}
	// Started over the open middle (not a panel), the orbit keeps going wherever the mouse goes until the button is let go.
	if (!bRightDown)
	{
		bPeopleOrbiting = false;
	}
	else if (!bPeopleOrbiting && !IO.WantCaptureMouse && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		bPeopleOrbiting = true;
	}
	const bool bOrbiting = bPeopleOrbiting;
	if (bOrbiting)
	{
		// All the way round and right over and under the body (sneed: "rotate around to see underside etc all angles").
		PeopleOrbit = FRotator::NormalizeAxis(PeopleOrbit + static_cast<float>(OrbitMove.X) * 0.35f);
		PeoplePitch = FMath::Clamp(PeoplePitch - static_cast<float>(OrbitMove.Y) * 0.25f, NgPreviewOrbitPitchLow, NgPreviewOrbitPitchHigh);
	}
	if (!bOrbiting && !IO.WantCaptureMouse && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
	{
		PeopleSpinRate = Delta > 0.0f ? IO.MouseDelta.x * 0.45f / Delta : 0.0f;
		PeoplePitch = FMath::Clamp(PeoplePitch - IO.MouseDelta.y * 0.15f, FMath::Min(NgPreviewPitchLow, PeoplePitch), FMath::Max(NgPreviewPitchHigh, PeoplePitch));
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
	// The camera stands in front of the body's own facing (not the spin), carried round by the orbit, at the body's middle,
	// the face when close.
	FVector Focus = Actor->GetActorLocation() + FVector(0.0, 0.0, 20.0);
	if (const USkeletalMeshComponent* Mesh = Actor->GetMakeHumanMesh())
	{
		const FBoxSphereBounds Bounds = Mesh->Bounds;
		const float Closeness = 1.0f - FMath::Clamp((PeopleDistance - 70.0f) / 300.0f, 0.0f, 1.0f);
		Focus = FVector(Actor->GetActorLocation().X, Actor->GetActorLocation().Y, Bounds.Origin.Z + Bounds.BoxExtent.Z * (0.05f + 0.7f * Closeness));
	}
	const float Facing = Actor->GetDrawnYaw() + PeopleViewYaw + PeopleOrbit;
	const FRotator Around(PeoplePitch, Facing + 180.0f, 0.0f);
	FVector Eye = Focus - Around.Vector() * PeopleDistance;
	// Never under the ground when looking up from far off: the eye stays a little over the feet (looking at the same spot),
	// unless the right-button orbit has been taken under the body on purpose to look up at it (the ground is one-sided, so
	// from below it is not drawn and the body shows from underneath).
	const double LowestEye = Actor->GetActorLocation().Z + NgPreviewLowestEyeCm;
	FRotator Looking = Around;
	if (Eye.Z < LowestEye && PeoplePitch <= NgPreviewPitchHigh)
	{
		Eye.Z = LowestEye;
		Looking = (Focus - Eye).Rotation();
	}
	Camera->SetActorLocationAndRotation(Eye, Looking);
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
			Lamp->PointLightComponent->SetIntensity(NgPreviewLampIntensity * FMath::Max(GNgPreviewLamp.GetValueOnGameThread(), 0.0f));
			Lamp->PointLightComponent->SetAttenuationRadius(900.0f);
			const float Radius = FMath::Max(GNgPreviewLampRadius.GetValueOnGameThread(), 0.0f);
			Lamp->PointLightComponent->SetSourceRadius(Radius);
			Lamp->PointLightComponent->SetSoftSourceRadius(Radius * 2.0f);
			Lamp->PointLightComponent->SetLightColor(FLinearColor(1.0f, 0.95f, 0.88f));
			Lamp->PointLightComponent->SetCastShadows(GNgPreviewLampShadows.GetValueOnGameThread() != 0);
			Lamp->PointLightComponent->bTransmission = GNgPreviewLampTransmission.GetValueOnGameThread() != 0;
			Lamp->PointLightComponent->MarkRenderStateDirty();
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
	if (Slot == 0)
	{
		TitleStage::RememberCharacterText(Text);  // You: the character the title screen shows next time
	}
	colony::EditColonistCommand Edit;
	Edit.slot = Slot;
	Edit.setCreator = true;
	Edit.creator = TCHAR_TO_UTF8(*Text);
	Submit(Edit);
}

// ---- Pages ----------------------------------------------------------------------------------------

// ---- The title screen -----------------------------------------------------------------------------

void UColonyHudSubsystem::UpdateTitleCamera(USimSubsystem& Bridge)
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const colony::Sim* Sim = Bridge.GetSim();
	if (!PC || !Sim)
	{
		return;
	}
	if (GNgTitleStage.GetValueOnGameThread() != 0 && UpdateTitleStage(Bridge))
	{
		return;
	}
	const colony::Map& Map = Sim->map();
	const FVector Origin = Bridge.SimToWorld(0.0f, 0.0f, 0);
	if (!TitleCamera.IsValid())
	{
		// The glade: of a grid of walkable spots in the map's middle, the one with the fewest trees round it (a little
		// pull towards the middle), so the camera sees the forest's edge across open ground.
		const colony::Plants& Plants = Sim->plants();
		const float Size = static_cast<float>(FMath::Min(Map.width(), Map.height()));
		float BestScore = TNumericLimits<float>::Max();
		TitleFocus = FVector2D(Map.width() * 0.5, Map.height() * 0.5);
		for (float Y = Size * 0.2f; Y <= Size * 0.8f; Y += 8.0f)
		{
			for (float X = Size * 0.2f; X <= Size * 0.8f; X += 8.0f)
			{
				if (!Map.walkable(colony::CellPos{FMath::FloorToInt32(X), FMath::FloorToInt32(Y), 0}))
				{
					continue;
				}
				float Trees = 0.0f;
				for (float DY = -NgTitleGladeMetres; DY <= NgTitleGladeMetres; DY += 2.0f)
				{
					for (float DX = -NgTitleGladeMetres; DX <= NgTitleGladeMetres; DX += 2.0f)
					{
						const colony::CellPos Cell{FMath::FloorToInt32(X + DX), FMath::FloorToInt32(Y + DY), 0};
						if (DX * DX + DY * DY <= NgTitleGladeMetres * NgTitleGladeMetres && Map.inBounds(Cell) && (Plants.isTree(Cell) || !Map.walkable(Cell)))
						{
							Trees += 1.0f;
						}
					}
				}
				const float Score = Trees + 0.03f * FVector2D::Distance(FVector2D(X, Y), FVector2D(Size * 0.5f, Size * 0.5f));
				if (Score < BestScore)
				{
					BestScore = Score;
					TitleFocus = FVector2D(X, Y);
				}
			}
		}
		TitleRadius = NgTitleOrbitMetres;
		TitleYaw = 0.0f;
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		TitleCamera = World->SpawnActor<ACameraActor>(Origin + FVector(TitleFocus.X * 100.0, TitleFocus.Y * 100.0, 300.0), FRotator::ZeroRotator, Params);
		if (ACameraActor* Made = TitleCamera.Get())
		{
			Made->GetCameraComponent()->SetFieldOfView(NgTitleFieldOfView);
		}
		// No trunk across the camera's circle or in the glade (undergrowth stays): cosmetic, gone when the title goes.
		if (UTerrainSubsystem* Terrain = World->GetSubsystem<UTerrainSubsystem>())
		{
			TArray<FVector4f> Clearings;
			Clearings.Add(FVector4f(static_cast<float>(TitleFocus.X), static_cast<float>(TitleFocus.Y), 5.0f, 1.0f));
			for (int32 i = 0; i < 20; ++i)
			{
				const float A = static_cast<float>(i) / 20.0f * 2.0f * PI;
				Clearings.Add(FVector4f(static_cast<float>(TitleFocus.X) + FMath::Cos(A) * TitleRadius, static_cast<float>(TitleFocus.Y) + FMath::Sin(A) * TitleRadius, 4.5f, 1.0f));
			}
			Terrain->SetPlantClearings(Clearings);
		}
		// Calm morning light, held while the title shows.
		if (USkySubsystem* Sky = World->GetSubsystem<USkySubsystem>())
		{
			Sky->SetHourOverride(NgCalmHour(*Sky, Bridge, NgTitleHour));
		}
	}
	ACameraActor* Camera = TitleCamera.Get();
	if (!Camera)
	{
		return;
	}
	// A slow drift round the glade, low, the glade a little right of the middle (the menu is on the left).
	const float Delta = FMath::Clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
	TitleYaw = FRotator::NormalizeAxis(TitleYaw + NgTitleDegreesPerSecond * Delta);
	const float Rad = FMath::DegreesToRadians(TitleYaw);
	const float EyeX = static_cast<float>(TitleFocus.X) + FMath::Cos(Rad) * TitleRadius;
	const float EyeY = static_cast<float>(TitleFocus.Y) + FMath::Sin(Rad) * TitleRadius;
	const FVector Eye = Bridge.SimToWorld(EyeX, EyeY, 0) + FVector(0.0, 0.0, NgTitleEyeHeightCm);
	const FVector Look = Bridge.SimToWorld(static_cast<float>(TitleFocus.X), static_cast<float>(TitleFocus.Y), 0) + FVector(0.0, 0.0, 160.0);
	FRotator View = (Look - Eye).Rotation();
	View.Yaw -= NgTitleSubjectOffsetDegrees;
	View.Pitch = FMath::Clamp(View.Pitch + 2.0, -15.0, 10.0);
	Camera->SetActorLocationAndRotation(Eye, View);
	if (PC->GetViewTarget() != Camera)
	{
		PC->SetViewTarget(Camera);
	}
}

const ATitleStage* UColonyHudSubsystem::GetTitleStage() const
{
	return TitleStageActor.Get();
}

bool UColonyHudSubsystem::UpdateTitleStage(USimSubsystem& Bridge)
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return false;
	}
	if (!TitleStageActor.IsValid())
	{
		// High over the map, out of sight of everything else: the stage brings its own sun, sky and exposure, and the
		// sky's are put away while it stands.
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;
		const FVector At = Bridge.SimToWorld(0.0f, 0.0f, 0) + FVector(0.0, 0.0, NgTitleStageHeightCm);
		ATitleStage* Made = World->SpawnActor<ATitleStage>(At, FRotator::ZeroRotator, Params);
		if (!Made)
		{
			return false;
		}
		Made->Build();
		TitleStageActor = Made;
		if (USkySubsystem* Sky = World->GetSubsystem<USkySubsystem>())
		{
			Sky->SetStageMode(true);
			bTitleStageSky = true;
		}
		if (!TitleCamera.IsValid())
		{
			FActorSpawnParameters CameraParams;
			CameraParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			TitleCamera = World->SpawnActor<ACameraActor>(Made->GetCameraTransform().GetLocation(), Made->GetCameraTransform().Rotator(), CameraParams);
		}
		if (APlayerCameraManager* CameraManager = PC->PlayerCameraManager)
		{
			CameraManager->SetGameCameraCutThisFrame();
		}
	}
	ATitleStage* Stage = TitleStageActor.Get();
	ACameraActor* Camera = TitleCamera.Get();
	if (!Stage || !Camera)
	{
		return false;
	}
	Camera->GetCameraComponent()->SetFieldOfView(Stage->GetFieldOfView());
	Camera->SetActorTransform(Stage->GetCameraTransform());
	if (PC->GetViewTarget() != Camera)
	{
		PC->SetViewTarget(Camera);
	}
	return true;
}

void UColonyHudSubsystem::EndTitleCamera()
{
	if (ATitleStage* Stage = TitleStageActor.Get())
	{
		Stage->Destroy();
	}
	TitleStageActor.Reset();
	if (bTitleStageSky)
	{
		bTitleStageSky = false;
		if (USkySubsystem* Sky = GetWorld() ? GetWorld()->GetSubsystem<USkySubsystem>() : nullptr)
		{
			Sky->SetStageMode(false);
		}
	}
	ACameraActor* Camera = TitleCamera.Get();
	if (!Camera)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr; PC && PC->GetViewTarget() == Camera)
	{
		PC->SetViewTarget(PC->GetPawn() ? static_cast<AActor*>(PC->GetPawn()) : static_cast<AActor*>(PC));
	}
	Camera->Destroy();
	TitleCamera.Reset();
	TitleShownAt = -1.0;
	if (World)
	{
		if (UTerrainSubsystem* Terrain = World->GetSubsystem<UTerrainSubsystem>())
		{
			Terrain->SetPlantClearings({});
		}
		if (USkySubsystem* Sky = World->GetSubsystem<USkySubsystem>())
		{
			Sky->SetHourOverride(-1.0f);
		}
	}
}

void UColonyHudSubsystem::DrawMenuBackdrop(USimSubsystem& Bridge, float Scale)
{
	(void)Scale;
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	ImDrawList* Back = ImGui::GetBackgroundDrawList();
	const ImVec2 A = Viewport->Pos;
	const ImVec2 B(Viewport->Pos.x + Viewport->Size.x, Viewport->Pos.y + Viewport->Size.y);
	if (MainMenuPage != 0)
	{
		// The other pages: a dark veil, heavier on the left.
		Back->AddRectFilledMultiColor(A, B, IM_COL32(6, 8, 10, 215), IM_COL32(6, 8, 10, 120), IM_COL32(6, 8, 10, 120), IM_COL32(6, 8, 10, 215));
		return;
	}
	// The title: the world shows; a deep shade behind the menu on the left fades out by the middle, the bottom and the
	// top edge darken a little (a film's framing).
	const float W = Viewport->Size.x;
	const float H = Viewport->Size.y;
	const ImU32 Deep = IM_COL32(4, 7, 6, Bridge.IsTitleBackdrop() ? 200 : 225);
	const ImU32 Clear = IM_COL32(4, 7, 6, 0);
	Back->AddRectFilledMultiColor(A, ImVec2(A.x + W * 0.62f, B.y), Deep, Clear, Clear, Deep);
	Back->AddRectFilledMultiColor(ImVec2(A.x, A.y + H * 0.70f), B, Clear, Clear, IM_COL32(4, 7, 6, 170), IM_COL32(4, 7, 6, 170));
	Back->AddRectFilledMultiColor(A, ImVec2(B.x, A.y + H * 0.14f), IM_COL32(4, 7, 6, 110), IM_COL32(4, 7, 6, 110), Clear, Clear);
}

void UColonyHudSubsystem::LoadFromMenu(USimSubsystem& Bridge, const FString& Slot)
{
	if (!Bridge.LoadGame(Slot))
	{
		return;
	}
	EndTitleCamera();
	bNarratorPending = false;
	bMainMenuOpen = false;
	MainMenuPage = 0;
	if (UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>())
	{
		Overseer->EnterOverseerMode();
	}
}

void UColonyHudSubsystem::DrawTitlePage(USimSubsystem& Bridge, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const double Now = ImGui::GetTime();
	if (TitleShownAt < 0.0)
	{
		TitleShownAt = Now;
	}
	const float Age = static_cast<float>(Now - TitleShownAt);
	const auto Fade = [Age](float Start, float Length) { const float T = FMath::Clamp((Age - Start) / Length, 0.0f, 1.0f); return 1.0f - (1.0f - T) * (1.0f - T) * (1.0f - T); };
	const auto Colour = [](const ImVec4& C, float Alpha) { return ImGui::GetColorU32(ImVec4(C.x, C.y, C.z, C.w * Alpha)); };

	// The newest save, for Continue.
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
	const bool bInGame = Bridge.GetSim() && !Bridge.IsTitleBackdrop();

	ImGui::SetNextWindowPos(Viewport->Pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(Viewport->Size, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	const bool bOpen = ImGui::Begin("##NgTitleScreen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleVar();
	if (!bOpen)
	{
		ImGui::End();
		return;
	}
	ImDrawList* Draw = ImGui::GetWindowDrawList();
	const float W = Viewport->Size.x;
	const float H = Viewport->Size.y;
	const float Left = Viewport->Pos.x + FMath::Max(W * 0.075f, 48.0f * Scale);
	// Sizes below are in pixels; PushFont takes them before the frame's text scale, hence ToFont.
	const float Base = ImGui::GetFontSize();
	const float ToFont = ImGui::GetStyle().FontSizeBase / FMath::Max(Base, 1.0f);

	// The name: big, widely spaced capitals, a short jade bar under it, then the line about the setting.
	const float LogoSize = FMath::Clamp(H * 0.11f, 64.0f * Scale, 168.0f * Scale);
	const float LogoTop = Viewport->Pos.y + H * 0.20f;
	const float LogoAlpha = Fade(0.1f, 1.1f);
	{
		ImGui::PushFont(nullptr, LogoSize * ToFont);
		float X = Left + (1.0f - LogoAlpha) * -24.0f * Scale;
		for (const char* Letter = "RIM3D"; *Letter; ++Letter)
		{
			const char Glyph[2] = {*Letter, 0};
			Draw->AddText(ImVec2(X, LogoTop), Colour(NgStyle::Text, LogoAlpha), Glyph);
			X += ImGui::CalcTextSize(Glyph).x + LogoSize * NgTitleLetterSpacing;
		}
		ImGui::PopFont();
	}
	const float BarTop = LogoTop + LogoSize * 1.08f;
	const float BarAlpha = Fade(0.5f, 0.8f);
	Draw->AddRectFilled(ImVec2(Left, BarTop), ImVec2(Left + 84.0f * Scale * BarAlpha, BarTop + 4.0f * Scale), Colour(NgStyle::Jade, BarAlpha));
	ImGui::PushFont(nullptr, Base * 1.2f * ToFont);
	Draw->AddText(ImVec2(Left, BarTop + 18.0f * Scale), Colour(NgStyle::Dim, Fade(0.6f, 0.9f)), "A colony on the rim, year 6650");
	ImGui::PopFont();

	// The menu: big plain words, no boxes; the one under the mouse brightens, moves in a little and gets the jade bar.
	float ItemTop = BarTop + 18.0f * Scale + Base * 1.2f + H * 0.07f;
	const float ItemWidth = FMath::Min(W * 0.32f, 520.0f * Scale);
	const float ItemHeight = Base * 2.35f;
	int32 ItemIndex = 0;
	const auto Item = [&](const TCHAR* Label, const FString& Note, bool bPrimary, float GapBefore) -> bool
	{
		ItemTop += GapBefore;
		const float Alpha = Fade(0.75f + ItemIndex * 0.07f, 0.55f);
		const float Slide = (1.0f - Alpha) * -18.0f * Scale;
		ImGui::SetCursorScreenPos(ImVec2(Left + Slide, ItemTop));
		ImGui::PushID(ItemIndex++);
		const bool bClicked = ImGui::InvisibleButton("##item", ImVec2(ItemWidth, ItemHeight)) && Alpha > 0.5f;
		const bool bHovered = ImGui::IsItemHovered();
		ImGui::PopID();
		const float Size = Base * 1.6f;
		const float TextY = ItemTop + (ItemHeight - Size) * 0.5f;
		const float Indent = bHovered ? 22.0f * Scale : 0.0f;
		if (bHovered)
		{
			Draw->AddRectFilled(ImVec2(Left + Slide, TextY + Size * 0.12f), ImVec2(Left + Slide + 4.0f * Scale, TextY + Size * 0.92f), Colour(NgStyle::Jade, Alpha));
		}
		const ImVec4& Tone = bPrimary ? NgStyle::Jade : NgStyle::Text;
		const FTCHARToUTF8 Text(Label);
		ImGui::PushFont(nullptr, Size * ToFont);
		Draw->AddText(ImVec2(Left + Slide + Indent, TextY), Colour(Tone, Alpha * (bHovered || bPrimary ? 1.0f : 0.80f)), Text.Get());
		const float TextWide = ImGui::CalcTextSize(Text.Get()).x;
		ImGui::PopFont();
		if (!Note.IsEmpty())
		{
			const FTCHARToUTF8 Small(*Note);
			ImGui::PushFont(nullptr, Base * 0.95f * ToFont);
			Draw->AddText(ImVec2(Left + Slide + Indent + TextWide + 16.0f * Scale, TextY + Size - Base * 1.05f), Colour(NgStyle::Faint, Alpha * (bHovered ? 1.4f : 1.0f)), Small.Get());
			ImGui::PopFont();
		}
		ItemTop += ItemHeight;
		return bClicked;
	};

	if (bInGame && Item(TEXT("Back to the game"), FString(), true, 0.0f))
	{
		bMainMenuOpen = false;
	}
	if (!bInGame && !Newest.IsEmpty() && Item(TEXT("Continue"), FString::Printf(TEXT("%s, %s"), *Newest, *NewestTime.ToString(TEXT("%d %b %H:%M"))), true, 0.0f))
	{
		LoadFromMenu(Bridge, Newest);
	}
	if (Item(TEXT("New colony"), FString(), !bInGame && Newest.IsEmpty(), 0.0f))
	{
		MainMenuPage = 1;
	}
	if (Item(TEXT("Load a game"), FString(), false, 0.0f))
	{
		MainMenuPage = 2;
		SaveSlots.Reset();
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(USimSubsystem::GetSaveDirectory() / TEXT("*.save")), true, false);
		Files.Sort([](const FString& A, const FString& B)
		{
			return IFileManager::Get().GetTimeStamp(*(USimSubsystem::GetSaveDirectory() / A)) > IFileManager::Get().GetTimeStamp(*(USimSubsystem::GetSaveDirectory() / B));
		});
		for (const FString& File : Files)
		{
			SaveSlots.Add(FPaths::GetBaseFilename(File));
		}
	}
	if (Item(TEXT("Settings"), FString(), false, 0.0f))
	{
		bSettingsOpen = !bSettingsOpen;
	}
	if (Item(TEXT("Debug"), TEXT("test scenes"), false, 0.0f))
	{
		MainMenuPage = 5;
	}
	if (Item(TEXT("Quit"), FString(), false, ItemHeight * 0.35f))
	{
		UKismetSystemLibrary::QuitGame(GetWorld(), GetWorld()->GetFirstPlayerController(), EQuitPreference::Quit, false);
	}

	// The foot of the screen: where the land behind is, and the build.
	const float FootAlpha = Fade(1.4f, 1.0f);
	ImGui::PushFont(nullptr, Base * 0.95f * ToFont);
	const float FootY = Viewport->Pos.y + H - FMath::Max(H * 0.06f, 32.0f * Scale);
	FString Where;
	if (Bridge.IsTitleBackdrop() && TitleStageActor.IsValid())
	{
		Where = TEXT("High orbit, over a world on the rim");
	}
	else if (Bridge.IsTitleBackdrop() && Bridge.GetSim() && Bridge.GetDefs())
	{
		const colony::Def* Biome = Bridge.GetDefs()->find("Biomes", Bridge.GetSim()->biome());
		Where = Biome ? FString::Printf(TEXT("The wilds: %s, morning"), *NgCapital(Biome->label())) : FString();
	}
	if (!Where.IsEmpty())
	{
		const FTCHARToUTF8 WhereText(*Where);
		const float WhereWide = ImGui::CalcTextSize(WhereText.Get()).x;
		Draw->AddText(ImVec2(Viewport->Pos.x + W - FMath::Max(W * 0.075f, 48.0f * Scale) - WhereWide, FootY), Colour(NgStyle::Dim, FootAlpha * 0.8f), WhereText.Get());
	}
	Draw->AddText(ImVec2(Left, FootY), Colour(NgStyle::Faint, FootAlpha), "Early build. Made for sneed.");
	ImGui::PopFont();
	ImGui::End();
}

void UColonyHudSubsystem::DrawLoadPage(USimSubsystem& Bridge, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const ImVec2 Size(FMath::Min(760.0f * Scale, Viewport->Size.x - 80.0f * Scale), FMath::Min(620.0f * Scale, Viewport->Size.y - 120.0f * Scale));
	const ImVec2 Pos(Viewport->Pos.x + (Viewport->Size.x - Size.x) * 0.5f, Viewport->Pos.y + (Viewport->Size.y - Size.y) * 0.5f);
	if (!NgBeginPanel("##NgLoad", Pos, Size))
	{
		NgEndPanel();
		return;
	}
	NgTitle(TEXT("Load a game"));
	NgRule();
	const float FooterH = ImGui::GetFrameHeightWithSpacing() * 1.6f;
	ImGui::BeginChild("##NgLoadList", ImVec2(0.0f, -FooterH), ImGuiChildFlags_None);
	if (SaveSlots.IsEmpty())
	{
		ColonyHud::TextColored(NgStyle::Dim, TEXT("No saved games yet. F5 saves while you play."));
	}
	FString Picked;
	const float DateW = 220.0f * Scale;
	for (const FString& Slot : SaveSlots)
	{
		const FDateTime Time = IFileManager::Get().GetTimeStamp(*(USimSubsystem::GetSaveDirectory() / (Slot + TEXT(".save"))));
		if (NgButton(FString::Printf(TEXT("%s##slot"), *Slot), false, ImVec2(ImGui::GetContentRegionAvail().x - DateW, ImGui::GetFrameHeight() * 1.5f)))
		{
			Picked = Slot;
		}
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ColonyHud::TextColored(NgStyle::Faint, Time.ToString(TEXT("%d %b %Y  %H:%M")));
	}
	ImGui::EndChild();
	NgRule();
	if (NgButton(TEXT("Back"), false, ImVec2(160.0f * Scale, 0.0f)))
	{
		MainMenuPage = 0;
	}
	NgEndPanel();
	if (!Picked.IsEmpty())
	{
		LoadFromMenu(Bridge, Picked);
	}
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
	ImGui::SameLine(FooterRight - 260.0f * Scale);
	if ((NgButton(TEXT("Choose landing site"), false, ImVec2(260.0f * Scale, 0.0f), true) || ColonyHud::EnterPressed()) && Picked)
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
	ColonyHud::TextColored(NgStyle::Dim, TEXT("The scene loads inside this game. F10 in the scene, or the scene ending, comes back to this title screen."));
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
		DebugScenes::Launch(GetWorld(), Picked);
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

void UColonyHudSubsystem::DrawPeopleMarks(CreatorPeople::FLook& Look, const CreatorPeople::FLook& Full, bool& bLive, bool& bSend, float Scale)
{
	// Up to four tattoos, scars, brands or paint marks, each on a body area, sized, turned and as strong as wanted.
	static const float InkColours[][3] = {{0.08f, 0.10f, 0.14f}, {0.02f, 0.02f, 0.02f}, {0.10f, 0.25f, 0.14f}, {0.55f, 0.08f, 0.07f},
		{0.10f, 0.20f, 0.55f}, {0.85f, 0.85f, 0.80f}, {0.75f, 0.45f, 0.10f}};
	const float Gap = NgStyle::kGap * Scale;
	NgHeading(TEXT("Tattoos and marks"));
	const TArray<FString> Areas = CreatorPeople::MarkAreas(Look);
	if (Areas.IsEmpty())
	{
		ColonyHud::TextColored(NgStyle::Faint, TEXT("This body has no areas for marks yet."));
		return;
	}
	// Area names as words: "UpperArmL" -> "upper arm (left)".
	const auto AreaWords = [](const FString& Name)
	{
		FString Body = Name;
		FString SideWord;
		if (Body.Len() > 1 && (Body.EndsWith(TEXT("L")) || Body.EndsWith(TEXT("R"))) && FChar::IsLower(Body[Body.Len() - 2]))
		{
			SideWord = Body.EndsWith(TEXT("L")) ? TEXT(" (left)") : TEXT(" (right)");
			Body.LeftChopInline(1);
		}
		FString Out;
		for (int32 i = 0; i < Body.Len(); ++i)
		{
			if (i > 0 && FChar::IsUpper(Body[i]))
			{
				Out.AppendChar(TEXT(' '));
			}
			Out.AppendChar(i > 0 ? FChar::ToLower(Body[i]) : Body[i]);
		}
		return Out + SideWord;
	};
	const auto Key = [](int32 Mark, const TCHAR* Field) { return FString::Printf(TEXT("mark%d%s"), Mark, Field); };
	bool bOffered = false;
	for (int32 m = 0; m < 4; ++m)
	{
		ImGui::PushID(m);
		const int32 Design = FMath::RoundToInt(Full.Get(*Key(m, TEXT("Design")), -1.0f));
		if (Design < 0)
		{
			if (!bOffered && NgButton(TEXT("Add a tattoo or mark"), false, ImVec2(-1.0f, 0.0f)))
			{
				Look.Values.Add(Key(m, TEXT("Design")), 0.0f);
				Look.Values.Add(Key(m, TEXT("Area")), static_cast<float>(FMath::Max(0, Areas.IndexOfByKey(TEXT("UpperArmL")))));
				bLive = bSend = true;
			}
			bOffered = true;
			ImGui::PopID();
			continue;
		}
		const float Avail = ImGui::GetContentRegionAvail().x;
		const float RemoveW = ImGui::CalcTextSize("Remove").x + 2.0f * ImGui::GetStyle().FramePadding.x;
		const float ComboW = (Avail - RemoveW - 2.0f * Gap) * 0.5f;
		// What it is.
		ImGui::SetNextItemWidth(ComboW);
		if (ImGui::BeginCombo("##design", TCHAR_TO_UTF8(*CreatorPeople::MarkDesignLabel(Design))))
		{
			for (int32 d = 0; d < CreatorPeople::MarkDesignCount(); ++d)
			{
				if (ImGui::Selectable(TCHAR_TO_UTF8(*CreatorPeople::MarkDesignLabel(d)), d == Design))
				{
					Look.Values.Add(Key(m, TEXT("Design")), static_cast<float>(d));
					bLive = bSend = true;
				}
			}
			ImGui::EndCombo();
		}
		// Where.
		ImGui::SameLine(0.0f, Gap);
		const int32 AreaIndex = FMath::Clamp(FMath::RoundToInt(Full.Get(*Key(m, TEXT("Area")), 0.0f)), 0, Areas.Num() - 1);
		ImGui::SetNextItemWidth(ComboW);
		if (ImGui::BeginCombo("##area", TCHAR_TO_UTF8(*AreaWords(Areas[AreaIndex]))))
		{
			for (int32 a = 0; a < Areas.Num(); ++a)
			{
				if (ImGui::Selectable(TCHAR_TO_UTF8(*AreaWords(Areas[a])), a == AreaIndex))
				{
					Look.Values.Add(Key(m, TEXT("Area")), static_cast<float>(a));
					bLive = bSend = true;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine(0.0f, Gap);
		if (NgButton(TEXT("Remove")))
		{
			for (const TCHAR* Field : {TEXT("Design"), TEXT("Area"), TEXT("Size"), TEXT("Angle"), TEXT("Opacity"), TEXT("R"), TEXT("G"), TEXT("B")})
			{
				Look.Values.Remove(Key(m, Field));
			}
			bLive = bSend = true;
			ImGui::PopID();
			continue;
		}
		// How big, which way and how strong.
		const auto Small = [&](const TCHAR* Field, const char* Format, float Default, float Min, float Max, float Width)
		{
			float Value = Full.Get(*Key(m, Field), Default);
			ImGui::SetNextItemWidth(Width);
			if (ImGui::SliderFloat(TCHAR_TO_UTF8(*FString::Printf(TEXT("##%s"), Field)), &Value, Min, Max, Format))
			{
				Look.Values.Add(Key(m, Field), Value);
				bLive = true;
			}
			bSend |= ImGui::IsItemDeactivatedAfterEdit();
		};
		const float Third = (ImGui::GetContentRegionAvail().x - 2.0f * Gap) / 3.0f;
		Small(TEXT("Size"), "%.0f cm", 12.0f, 3.0f, 40.0f, Third);
		ImGui::SameLine(0.0f, Gap);
		Small(TEXT("Angle"), "turn %.0f", 0.0f, -180.0f, 180.0f, Third);
		ImGui::SameLine(0.0f, Gap);
		Small(TEXT("Opacity"), "strength %.2f", 0.9f, 0.1f, 1.0f, Third);
		if (CreatorPeople::MarkDesignTakesColour(Design))
		{
			const float Swatch = 22.0f * Scale;
			for (int32 c = 0; c < static_cast<int32>(UE_ARRAY_COUNT(InkColours)); ++c)
			{
				if (c > 0)
				{
					ImGui::SameLine(0.0f, Gap * 0.6f);
				}
				const bool bSame = FMath::IsNearlyEqual(Full.Get(*Key(m, TEXT("R")), InkColours[0][0]), InkColours[c][0], 0.01f) &&
					FMath::IsNearlyEqual(Full.Get(*Key(m, TEXT("G")), InkColours[0][1]), InkColours[c][1], 0.01f) &&
					FMath::IsNearlyEqual(Full.Get(*Key(m, TEXT("B")), InkColours[0][2]), InkColours[c][2], 0.01f);
				if (NgSwatch(TCHAR_TO_UTF8(*FString::Printf(TEXT("##ink%d"), c)), InkColours[c], bSame, Swatch))
				{
					Look.Values.Add(Key(m, TEXT("R")), InkColours[c][0]);
					Look.Values.Add(Key(m, TEXT("G")), InkColours[c][1]);
					Look.Values.Add(Key(m, TEXT("B")), InkColours[c][2]);
					bLive = bSend = true;
				}
			}
		}
		NgRule();
		ImGui::PopID();
	}
}

void UColonyHudSubsystem::DrawPeopleJewellery(CreatorPeople::FLook& Look, const CreatorPeople::FLook& Full, bool& bLive, bool& bSend, float Scale)
{
	// Each piece on or off, in its metal.
	const float Gap = NgStyle::kGap * Scale;
	NgHeading(TEXT("Jewellery"));
	const float Avail = ImGui::GetContentRegionAvail().x;
	const float MetalW = FMath::Min(130.0f * Scale, Avail * 0.35f);
	for (int32 j = 0; j < CreatorPeople::JewelCount(); ++j)
	{
		ImGui::PushID(j);
		const FString Name = FString::Printf(TEXT("jewel%d"), j);
		const int32 Worn = FMath::RoundToInt(Full.Get(*Name, 0.0f));
		if (NgButton(CreatorPeople::JewelLabel(j), Worn > 0, ImVec2(Avail - MetalW - Gap, 0.0f)))
		{
			if (Worn > 0)
			{
				Look.Values.Remove(Name);
			}
			else
			{
				Look.Values.Add(Name, 1.0f);
			}
			bLive = bSend = true;
		}
		if (Worn > 0)
		{
			ImGui::SameLine(0.0f, Gap);
			ImGui::SetNextItemWidth(MetalW);
			if (ImGui::BeginCombo("##metal", TCHAR_TO_UTF8(*CreatorPeople::MetalLabel(Worn - 1))))
			{
				for (int32 Metal = 0; Metal < CreatorPeople::MetalCount(); ++Metal)
				{
					if (ImGui::Selectable(TCHAR_TO_UTF8(*CreatorPeople::MetalLabel(Metal)), Metal == Worn - 1))
					{
						Look.Values.Add(Name, static_cast<float>(Metal + 1));
						bLive = bSend = true;
					}
				}
				ImGui::EndCombo();
			}
		}
		ImGui::PopID();
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
	// Ethnicity (sneed 2026-11-26: "a picked ethnicity ... and or slide"): a preset sets the ethnic base shape, colouring and
	// a face and build round its average; the two feature sliders and every shape slider below fine-tune it.
	ImGui::Dummy(ImVec2(1.0f, Gap * 0.5f));
	NgLabel(TEXT("Ethnicity"));
	{
		const int32 Picked = CreatorPeople::EthnicityOf(Full);
		const FString Now = Picked < 0 ? FString(TEXT("Choose...")) : CreatorPeople::EthnicityLabel(Picked);
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::BeginCombo("##ethnicity", TCHAR_TO_UTF8(*Now)))
		{
			for (int32 Ethnicity = 0; Ethnicity <= CreatorPeople::EthnicityCount(); ++Ethnicity)
			{
				if (ImGui::Selectable(TCHAR_TO_UTF8(*CreatorPeople::EthnicityLabel(Ethnicity)), Ethnicity == Picked))
				{
					CreatorPeople::ApplyEthnicity(Look, Ethnicity);
					bLive = bSend = true;
				}
			}
			ImGui::EndCombo();
		}
		if (Picked >= 0 && NgButton(TEXT("Another face of these people"), false, ImVec2(-1.0f, 0.0f)))
		{
			CreatorPeople::ApplyEthnicity(Look, Picked);
			bLive = bSend = true;
		}
		for (int32 Feature = 0; Feature < 2; ++Feature)
		{
			Slider(*CreatorPeople::EthnicFeatureLabel(Feature), *CreatorPeople::EthnicFeatureKey(Feature), 0.0f, 1.0f);
		}
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
	// The face's own shape (face variety, sneed 2026-11-25): a random one to start, then each part by hand. The same
	// sliders the Debug creator has, drawn in game with the same CreatorApply.
	ImGui::Dummy(ImVec2(1.0f, Gap * 0.5f));
	{
		const float HalfW = (ImGui::GetContentRegionAvail().x - Gap) * 0.5f;
		if (NgButton(TEXT("Random face"), false, ImVec2(HalfW, 0.0f)))
		{
			CreatorPeople::RandomFace(Look);
			bLive = bSend = true;
		}
		ImGui::SameLine(0.0f, Gap);
		if (NgButton(TEXT("Random build"), false, ImVec2(HalfW, 0.0f)))
		{
			CreatorPeople::RandomBuild(Look);
			bLive = bSend = true;
		}
	}
	// The fine sliders, folded (35 of them): the face's parts, then the build's.
	for (int32 Group = 0; Group < 2; ++Group)
	{
		const bool bBodyGroup = Group == 1;
		if (!ImGui::CollapsingHeader(bBodyGroup ? "Build in detail" : "Face in detail"))
		{
			continue;
		}
		for (int32 FaceIndex = 0; FaceIndex < CreatorPeople::FaceShapeCount(); ++FaceIndex)
		{
			if (CreatorPeople::FaceShapeIsBody(FaceIndex) != bBodyGroup)
			{
				continue;
			}
			const FString FaceKey = CreatorPeople::FaceShapeKey(FaceIndex);
			Slider(*CreatorPeople::FaceShapeLabel(FaceIndex), *FaceKey, -1.0f, 1.0f);
		}
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
	// Skin: a slider from fair to dark over the colours it blends (NgSkinAt), saved as the colour itself (skinR/G/B, what
	// the body is drawn with) and the slider's place (skinTone). Beside it the clothes tick (sneed 2026-11-25: "put the
	// clothes on box next to skin").
	ImGui::Dummy(ImVec2(1.0f, Gap * 0.5f));
	NgLabel(TEXT("Skin"));
	{
		float Tone = Look.Values.Contains(TEXT("skinTone")) ? FMath::Clamp(Look.Get(TEXT("skinTone"), 0.0f), 0.0f, 1.0f)
			: NgSkinToneOf(Full.Get(TEXT("skinR"), NgSkinSwatches[1][0]), Full.Get(TEXT("skinG"), NgSkinSwatches[1][1]), Full.Get(TEXT("skinB"), NgSkinSwatches[1][2]));
		const float ClothesW = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize("CLOTHES ON").x;
		const float SkinW = FMath::Max(ImGui::GetContentRegionAvail().x - ClothesW - 2.0f * Gap, 80.0f * Scale);
		// The track shows the skin colours it runs through.
		{
			constexpr int32 Last = static_cast<int32>(UE_ARRAY_COUNT(NgSkinSwatches)) - 1;
			const ImVec2 TrackAt = ImGui::GetCursorScreenPos();
			const float TrackH = ImGui::GetFrameHeight();
			ImDrawList* Draw = ImGui::GetWindowDrawList();
			for (int32 i = 0; i < Last; ++i)
			{
				const float X0 = TrackAt.x + SkinW * static_cast<float>(i) / static_cast<float>(Last);
				const float X1 = TrackAt.x + SkinW * static_cast<float>(i + 1) / static_cast<float>(Last);
				const ImU32 From = ImGui::GetColorU32(ImVec4(NgSkinSwatches[i][0], NgSkinSwatches[i][1], NgSkinSwatches[i][2], 1.0f));
				const ImU32 To = ImGui::GetColorU32(ImVec4(NgSkinSwatches[i + 1][0], NgSkinSwatches[i + 1][1], NgSkinSwatches[i + 1][2], 1.0f));
				Draw->AddRectFilledMultiColor(ImVec2(X0, TrackAt.y), ImVec2(X1, TrackAt.y + TrackH), From, To, To, From);
			}
		}
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
		ImGui::PushStyleColor(ImGuiCol_SliderGrab, NgStyle::Field);
		ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, NgStyle::Jade);
		ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 8.0f * Scale);
		ImGui::SetNextItemWidth(SkinW);
		if (ImGui::SliderFloat("##skinTone", &Tone, 0.0f, 1.0f, ""))
		{
			float C[3];
			NgSkinAt(Tone, C);
			NgSetColour(Look, TEXT("skin"), C);
			Look.Values.Add(TEXT("skinTone"), Tone);
			bLive = true;
		}
		bSend |= ImGui::IsItemDeactivatedAfterEdit();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(5);
		ColonyHud::Tip(TEXT("Fair to dark. Any point along it works, not only the marks."));
		ImGui::SameLine(0.0f, 2.0f * Gap);
		ImGui::Checkbox("##clothes", &bPeopleClothes);
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ColonyHud::TextColored(NgStyle::Text, TEXT("CLOTHES ON"));
		ColonyHud::Tip(TEXT("Shows the cotton shirt and trousers from the cargo pack. Everyone lands naked."));
	}
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
	const int32 Pubic = FMath::RoundToInt(Full.Get(TEXT("pubic"), 0.0f));
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
	// The rest of the old creator's choices (sneed 2026-11-25): genital size, how dark the nipples and genitals are,
	// tattoos and other marks, and jewellery. Only what the player picks; nobody is given any by chance.
	ImGui::Dummy(ImVec2(1.0f, Gap * 0.5f));
	Slider(TEXT("Genital size"), TEXT("genital"), -1.0f, 1.0f);
	Slider(TEXT("Darker nipples and genitals"), TEXT("pigment"), 0.0f, 1.0f);
	DrawPeopleMarks(Look, Full, bLive, bSend, Scale);
	DrawPeopleJewellery(Look, Full, bLive, bSend, Scale);
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
	// The label column fits the longest skill name at the text size in use (text grew half again, sneed 2026-11-24).
	float LabelW = 110.0f * Scale;
	for (const colony::SkillRecord& Skill : Colonist.skills)
	{
		const FTCHARToUTF8 Name(*NgDefLabel(Defs, "Skills", Skill.def));
		LabelW = FMath::Max(LabelW, ImGui::CalcTextSize(Name.Get()).x + 12.0f * Scale);
	}
	const float BarW = FMath::Max(40.0f * Scale, ImGui::GetContentRegionAvail().x - LabelW - 3.2f * ImGui::GetFontSize());
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
	// The game waits at its first second while the people are made (sneed 2026-11-24: it had started running, so every
	// edit was refused and the people walked off as bare mannequins): held paused whatever pressed play.
	if (Bridge.GetSpeed() != 0)
	{
		Bridge.SetSpeed(0);
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
				// You start as the creator's woman, so a man's name drawn for you goes (sneed's screen showed "Theo").
				if (!Colonist->female)
				{
					const std::string Name = colony::randomFirstName(*Defs, Sim->settings().seed ^ 0x594F55ULL, true);
					if (!Name.empty())
					{
						Edit.firstName = Name;
						PeopleNamesFor = colony::kNoEntity;  // the name boxes read the new name
					}
				}
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
	Bridge.ApplyPregameCommands();  // the first looks and your name show this frame
	// Naked by default (they land so); "clothes on" shows the cargo pack's cotton shirt and trousers on everyone.
	// Only the one being shaped stands on the spot (sneed 2026-11-25: "it only shows selected on the spot, not both"):
	// the others are hidden while the screen is open (cosmetic; EndPeopleCamera shows them again).
	if (USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
	{
		const bool bPickedDrawn = Pawns->FindPawn(Picked) != nullptr;
		TArray<FString> Cotton;
		for (const TCHAR* Piece : NgCottonClothes)
		{
			Cotton.Add(Piece);
		}
		for (const colony::Entity Pawn : People)
		{
			if (ASimPawnActor* Shown = Pawns->FindPawn(Pawn))
			{
				Shown->SetPreviewLive(true);  // a calm standing idle, breathing, while the game is held
				// Only once the picked one is drawn (then the camera stands too, and EndPeopleCamera shows them again).
				const bool bHide = Pawn != Picked && bPickedDrawn;
				if (Shown->IsBodyHidden() != bHide)
				{
					Shown->SetBodyHidden(bHide);
				}
			}
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

	// The panels sit flush on the screen's edges and on each other, with no gaps (sneed 2026-11-25: "the bottom ui bar is
	// right at the bottom of the screen ... the two side panels ... stretched to sizes so the open viewing box has no
	// gaps"): the bar along the whole bottom, the side panels from the top down onto it.
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float Pad = NgStyle::kPad * Scale;
	const float Gap = NgStyle::kGap * Scale;
	const float Left = Viewport->Pos.x;
	const float Right = Viewport->Pos.x + Viewport->Size.x;
	const float Top = Viewport->Pos.y;
	const float FooterH = FMath::Max(70.0f * Scale, ImGui::GetFrameHeight() + 2.0f * ImGui::GetStyle().WindowPadding.y + 8.0f * Scale);
	const float Bottom = Viewport->Pos.y + Viewport->Size.y - FooterH;
	const float SideW = FMath::Min(460.0f * Scale, Viewport->Size.x * 0.27f);
	const float InfoW = FMath::Min(500.0f * Scale, Viewport->Size.x * 0.29f);
	const colony::Colonist* Colonist = Sim->colonistOf(Picked);

	if (bYouPage)
	{
		// Left: who you are (nobody you remember).
		if (NgBeginPanel("##NgYou", ImVec2(Left, Top), ImVec2(SideW, Bottom - Top)))
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
		if (NgBeginPanel("##NgYouLooks", ImVec2(Right - InfoW, Top), ImVec2(InfoW, Bottom - Top)))
		{
			NgHeading(TEXT("Your looks"));
			DrawPeopleLooks(*Sim, Picked, 0, true, Scale);
		}
		NgEndPanel();
	}
	else
	{
		// Header: the title and the races (only humans for now, the rest shown and locked).
		// Sized from the text (it grew half again, sneed 2026-11-24): the title, and beside it the line and the races.
		const float HeadH = FMath::Max(92.0f * Scale, ImGui::GetFontSize() * NgStyle::kTitle + 2.0f * ImGui::GetStyle().WindowPadding.y + 10.0f * Scale);
		if (NgBeginPanel("##NgCompanionsHead", ImVec2(Left, Top), ImVec2(Right - Left, HeadH)))
		{
			NgTitle(TEXT("Your companions"));
			ImGui::SameLine(0.0f, 2.0f * Pad);
			ImGui::BeginGroup();
			ImGui::PushTextWrapPos(-1.0f);  // one line, never folded under the title
			ImGui::TextColored(NgStyle::Dim, "%s", "Two strangers, both 18. Shape them, and choose what they are good and bad at.");
			ImGui::PopTextWrapPos();
			for (const FNgRace& Race : NgRaces)
			{
				const colony::Def* Lineage = Defs->find("Lineages", Race.Lineage);
				const bool bOpen = FCStringAnsi::Strcmp(Race.Lineage, "Baseline") == 0;
				const FString Label = bOpen ? FString(Race.Fallback) : (Lineage ? NgCapital(Lineage->label()) : FString(Race.Fallback));
				ImGui::BeginDisabled(!bOpen);
				NgButton(bOpen ? Label : FString::Printf(TEXT("%s  (locked)"), *Label), bOpen);
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
		const float BodyTop = Top + HeadH;

		// Left: the two strangers as cards.
		if (NgBeginPanel("##NgCompanionsCards", ImVec2(Left, BodyTop), ImVec2(SideW, Bottom - BodyTop)))
		{
			NgHeading(TEXT("The strangers"));
			ImGui::PushTextWrapPos(0.0f);
			ColonyHud::TextColored(NgStyle::Faint, TEXT("Click one to shape them. Only that one stands on the spot."));
			ImGui::PopTextWrapPos();
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
				// Tall enough for the name, the line under it and the buttons at the text size in use.
				const float CardH = FMath::Max(Face, ImGui::GetFontSize() * NgStyle::kHeading + ImGui::GetTextLineHeightWithSpacing() +
					ImGui::GetFrameHeightWithSpacing() + 2.0f * ImGui::GetStyle().ItemSpacing.y) + 2.0f * ImGui::GetStyle().WindowPadding.y;
				ImGui::BeginChild("##card", ImVec2(0.0f, CardH), ImGuiChildFlags_Borders);
				// The whole card picks the stranger (sneed 2026-11-25: "make the names clickable"), but for its buttons.
				if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
				{
					if (!bPicked)
					{
						ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
					}
					if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
					{
						PeopleSelected = Slot;
					}
				}
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
		if (Colonist && NgBeginPanel("##NgCompanionsInfo", ImVec2(Right - InfoW, BodyTop), ImVec2(InfoW, Bottom - BodyTop)))
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

	// Under the body: how to look at it (the clothes tick is in the looks, next to the skin).
	// It fills the open middle's width and sits on the bar, between the side panels.
	const float StripW = Right - InfoW - (Left + SideW);
	const float StripH = ImGui::GetFrameHeight() + 2.0f * ImGui::GetStyle().WindowPadding.y + 4.0f * Scale;
	if (NgBeginPanel("##NgPeopleStrip", ImVec2(Left + SideW, Bottom - StripH), ImVec2(StripW, StripH)))
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(NgStyle::Faint, "%s", "Drag: spin    Hold right click: orbit    Wheel: zoom");
		// Zoom buttons as well as the wheel (sneed 2026-11-24: "i need to be able to zoom them"): in to the face, out to the feet.
		const float ZoomW = 2.2f * ImGui::GetFontSize();
		const float StripRight = ImGui::GetCursorStartPos().x + ImGui::GetWindowWidth() - 2.0f * ImGui::GetStyle().WindowPadding.x;
		ImGui::SameLine(StripRight - 2.0f * ZoomW - ImGui::GetStyle().ItemSpacing.x);
		if (NgButton(TEXT("-##zoomout"), false, ImVec2(ZoomW, 0.0f)))
		{
			PeopleDistance = FMath::Clamp(PeopleDistance / 0.8f, 70.0f, 520.0f);
		}
		ColonyHud::Tip(TEXT("Zoom out"));
		ImGui::SameLine();
		if (NgButton(TEXT("+##zoomin"), false, ImVec2(ZoomW, 0.0f)))
		{
			PeopleDistance = FMath::Clamp(PeopleDistance * 0.8f, 70.0f, 520.0f);
		}
		ColonyHud::Tip(TEXT("Zoom in"));
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
			if (NgButton(TEXT("Next: your companions"), false, ImVec2(280.0f * Scale, 0.0f), true) || ColonyHud::EnterPressed())
			{
				MainMenuPage = 4;
				PeopleSelected = 1;
				PeopleTab = 0;
				PeopleNamesFor = colony::kNoEntity;
				PeopleSpin = 0.0f;
				PeopleSpinRate = 0.0f;
			}
		}
		else if (NgButton(TEXT("Land and start"), false, ImVec2(280.0f * Scale, 0.0f), true) || ColonyHud::EnterPressed())
		{
			NgEndPanel();
			LandColony(Bridge);
			return;
		}
	}
	NgEndPanel();
}
