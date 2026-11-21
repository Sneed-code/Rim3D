// The title screen and the new game (docs/design/new-game.md): title, scenario, landing site (the world screen in its
// new-game mode), and the people screen where the player meets, rerolls, renames and shapes the starting colonists on
// their real 3D bodies before the shuttle's crew lands. The look is the "Frontier Glass" concept until sneed picks one.

#include "ColonyHudSubsystem.h"

#include "Camera/CameraActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"

#include <imgui.h>

#include "ColonyHudStyle.h"
#include "CreatorPeople.h"
#include "OverseerSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"
#include "WorldScreenSubsystem.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <string>
#include <vector>

namespace
{
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

	FString NgTraitLabel(const colony::DefDatabase& Defs, const colony::TraitRecord& Trait)
	{
		const colony::Def* Def = Defs.find("Traits", Trait.def);
		std::string Label = Def ? Def->label() : Trait.def;
		for (size_t Index = 0; Def && Index < Def->count("degreeDatas"); ++Index)
		{
			const std::string Base = "degreeDatas." + std::to_string(Index);
			if (static_cast<int>(Def->integer((Base + ".degree").c_str(), 0)) == Trait.degree)
			{
				Label = Def->text((Base + ".label").c_str(), Label);
			}
		}
		return NgCapital(Label.empty() ? Trait.def : Label);
	}

	FString NgName(const colony::Colonist& C)
	{
		const FString First = UTF8_TO_TCHAR(C.firstName.c_str());
		const FString Nick = UTF8_TO_TCHAR(C.nickName.c_str());
		const FString Last = UTF8_TO_TCHAR(C.lastName.c_str());
		return Nick.IsEmpty() || Nick == First ? FString::Printf(TEXT("%s %s"), *First, *Last)
			: FString::Printf(TEXT("%s '%s' %s"), *First, *Nick, *Last);
	}

	/** A glass panel (the Frontier Glass look): dark see-through fill, a thin amber edge. Pair with NgEndPanel. */
	bool NgBeginPanel(const char* Id, const ImVec2& Pos, const ImVec2& Size)
	{
		ImGui::SetNextWindowPos(Pos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(Size, ImGuiCond_Always);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.08f, 0.78f));
		ImGui::PushStyleColor(ImGuiCol_Border, ColonyHud::Rgb(0xe0b04a, 0.35f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
		const bool bOpen = ImGui::Begin(Id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(2);
		return bOpen;
	}

	void NgEndPanel()
	{
		ImGui::End();
	}

	/** A thin amber rule under a heading. */
	void NgRule()
	{
		const ImVec2 P = ImGui::GetCursorScreenPos();
		const float W = ImGui::GetContentRegionAvail().x;
		ImGui::GetWindowDrawList()->AddLine(P, ImVec2(P.x + W, P.y), ImGui::GetColorU32(ColonyHud::Rgb(0xe0b04a, 0.45f)), 1.0f);
		ImGui::Dummy(ImVec2(W, 4.0f));
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
			const ImU32 Colour = i < Count ? IM_COL32(240, 140, 40, 255) : IM_COL32(90, 90, 90, 90);
			Draw->AddTriangleFilled(ImVec2(X, P.y + Size * 0.1f), ImVec2(X - Size * 0.3f, P.y + Size * 0.85f), ImVec2(X + Size * 0.3f, P.y + Size * 0.85f), Colour);
			Draw->AddCircleFilled(ImVec2(X, P.y + Size * 0.72f), Size * 0.3f, Colour);
		}
		ImGui::Dummy(ImVec2(Size * 1.8f, Size));
	}

	/** The races on the people screen: the human one open, the others shown and locked until they are done (sneed). */
	struct FNgRace
	{
		const char* Lineage;
		const TCHAR* Fallback;
	};
	const FNgRace NgRaces[] = {{"Baseline", TEXT("Human")}, {"Emberkin", TEXT("Impid")}, {"Orcoid", TEXT("Orcoid")}, {"Drow", TEXT("Drow")},
		{"CatStrain", TEXT("Cat-strain")}};

	// Swatches for the people screen (sRGB): natural hair, skin from fair to dark, eyes.
	const float NgHairSwatches[][3] = {{0.07f, 0.06f, 0.055f}, {0.17f, 0.11f, 0.07f}, {0.32f, 0.22f, 0.14f}, {0.48f, 0.36f, 0.24f},
		{0.78f, 0.66f, 0.45f}, {0.45f, 0.20f, 0.10f}, {0.62f, 0.27f, 0.12f}, {0.62f, 0.60f, 0.58f}};
	const float NgSkinSwatches[][3] = {{0.96f, 0.86f, 0.80f}, {0.91f, 0.80f, 0.75f}, {0.86f, 0.72f, 0.62f}, {0.78f, 0.62f, 0.50f},
		{0.64f, 0.47f, 0.36f}, {0.48f, 0.34f, 0.25f}, {0.33f, 0.23f, 0.17f}};
	const float NgEyeSwatches[][3] = {{0.30f, 0.20f, 0.12f}, {0.45f, 0.32f, 0.18f}, {0.35f, 0.50f, 0.65f}, {0.40f, 0.55f, 0.35f},
		{0.50f, 0.55f, 0.55f}, {0.15f, 0.12f, 0.10f}};

	bool NgSwatch(const char* Id, const float* C, bool bPicked, float Size)
	{
		ImGui::PushStyleColor(ImGuiCol_Border, bPicked ? ColonyHud::Accent() : ImVec4(0, 0, 0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, bPicked ? 2.0f : 0.0f);
		const bool bClicked = ImGui::ColorButton(Id, ImVec4(C[0], C[1], C[2], 1.0f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(Size, Size));
		ImGui::PopStyleVar();
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
	PeopleYaw = 200.0f;
	PeoplePitch = -4.0f;
	PeopleDistance = 280.0f;
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
}

void UColonyHudSubsystem::UpdatePeopleCamera(colony::Entity Pawn)
{
	UWorld* World = GetWorld();
	USimPawnSubsystem* Pawns = World ? World->GetSubsystem<USimPawnSubsystem>() : nullptr;
	const ASimPawnActor* Actor = Pawns ? Pawns->FindPawn(Pawn) : nullptr;
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
	// Turn and zoom with the mouse over the open middle of the screen (not over a panel).
	const ImGuiIO& IO = ImGui::GetIO();
	if (!IO.WantCaptureMouse)
	{
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0f))
		{
			PeopleYaw += IO.MouseDelta.x * 0.35f;
			PeoplePitch = FMath::Clamp(PeoplePitch - IO.MouseDelta.y * 0.2f, -35.0f, 20.0f);
		}
		if (IO.MouseWheel != 0.0f)
		{
			PeopleDistance = FMath::Clamp(PeopleDistance * (IO.MouseWheel > 0.0f ? 0.88f : 1.0f / 0.88f), 70.0f, 520.0f);
		}
	}
	// Look at the body's middle, a little higher when close (the face), from the yaw and pitch round it.
	FVector Focus = Actor->GetActorLocation() + FVector(0.0, 0.0, 20.0);
	if (const USkeletalMeshComponent* Mesh = Actor->GetMakeHumanMesh())
	{
		const FBoxSphereBounds Bounds = Mesh->Bounds;
		const float Closeness = 1.0f - FMath::Clamp((PeopleDistance - 70.0f) / 300.0f, 0.0f, 1.0f);
		Focus = Bounds.Origin + FVector(0.0, 0.0, Bounds.BoxExtent.Z * (0.05f + 0.7f * Closeness));
	}
	const FRotator Around(PeoplePitch, Actor->GetActorRotation().Yaw + PeopleYaw, 0.0f);
	const FVector Eye = Focus - Around.Vector() * PeopleDistance;
	Camera->SetActorLocationAndRotation(Eye, Around);
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
	ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.4f);
	ColonyHud::TextColored(ColonyHud::TextMain(), TEXT("RIM3D"));
	ImGui::PopFont();
	ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("A colony on the rim, year 6650"));
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
	if (bRunning && ColonyHud::Button(TEXT("Back to the game"), true, ImVec2(Wide, 0.0f)))
	{
		bMainMenuOpen = false;
	}
	if (!bRunning && !Newest.IsEmpty())
	{
		if (ColonyHud::Button(TEXT("Continue"), true, ImVec2(Wide, 0.0f)) && Bridge.LoadGame(Newest))
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
	if (ColonyHud::Button(TEXT("New colony"), false, ImVec2(Wide, 0.0f)))
	{
		MainMenuPage = 1;
	}
	if (ColonyHud::Button(TEXT("Load a game"), false, ImVec2(Wide, 0.0f)))
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
	if (ColonyHud::Button(TEXT("Settings"), bSettingsOpen, ImVec2(Wide, 0.0f)))
	{
		bSettingsOpen = !bSettingsOpen;
	}
	ImGui::Spacing();
	if (ColonyHud::Button(TEXT("Quit to desktop"), false, ImVec2(Wide, 0.0f)))
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
	ColonyHud::Heading(TEXT("New colony"));
	ImGui::SameLine();
	ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("   Scenario  >  Landing site  >  People"));
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
		if (ColonyHud::Button(FString::Printf(TEXT("%s##scen"), *NgCapital(Def->label())), Def == Picked, ImVec2(ListW - 8.0f * Scale, ImGui::GetFrameHeight() * 1.6f)))
		{
			NewScenario = Name;
		}
	}
	ColonyHud::TextColored(ColonyHud::TextFaint(), TEXT("More scenarios later."));
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("##NgScenarioInfo", ImVec2(0.0f, -FooterH), ImGuiChildFlags_None);
	if (Picked && Defs)
	{
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
		ColonyHud::TextColored(ColonyHud::Accent(), NgCapital(Picked->label()));
		ImGui::PopFont();
		ImGui::PushTextWrapPos(0.0f);
		ColonyHud::Text(UTF8_TO_TCHAR(Picked->text("description").c_str()));
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		const int64 Strangers = Picked->integer("colonists", 0);
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("People"));
		ColonyHud::Text(Picked->flag("protagonist")
			? FString::Printf(TEXT("You, and %lld %s you have never met."), Strangers, Strangers == 1 ? TEXT("stranger") : TEXT("strangers"))
			: FString::Printf(TEXT("%lld colonists."), Strangers));
		ImGui::Spacing();
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("You start with"));
		for (size_t i = 0; i < Picked->count("startingThings"); ++i)
		{
			const std::string Base = "startingThings." + std::to_string(i);
			const std::string Thing = Picked->text((Base + ".thing").c_str());
			const int64 Count = Picked->integer((Base + ".count").c_str(), 1);
			ColonyHud::Text(FString::Printf(TEXT("  %lld  %s"), Count, *NgDefLabel(*Defs, "Things", Thing)));
		}
		ImGui::Spacing();
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Narrator"));
		for (const colony::DefId Id : Defs->idsOfKind("Narrators"))
		{
			const colony::Def& Def = Defs->get(Id);
			const FString Name = UTF8_TO_TCHAR(Def.defName().c_str());
			if (ColonyHud::Button(FString::Printf(TEXT("%s##narr"), *Name), NewNarrator == Name))
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
			if (ColonyHud::Button(TEXT("Random seed")))
			{
				NewSeed = FMath::RandRange(1, 999999);
			}
			const int32 Sizes[] = {256, 512, 1024};
			for (const int32 MapSize : Sizes)
			{
				if (ColonyHud::Button(FString::Printf(TEXT("%d m"), MapSize), NewMapSize == MapSize))
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
	if (ColonyHud::Button(TEXT("Back"), false, ImVec2(160.0f * Scale, 0.0f)))
	{
		MainMenuPage = 0;
	}
	ImGui::SameLine(FooterRight - 520.0f * Scale);
	if (ColonyHud::Button(TEXT("Land somewhere random"), false, ImVec2(250.0f * Scale, 0.0f)) && Picked)
	{
		StartNewColony(false);
	}
	ColonyHud::Tip(TEXT("Skips the world map: the shuttle comes down on a random tropical site"));
	ImGui::SameLine();
	if (ColonyHud::Button(TEXT("Choose landing site"), true, ImVec2(260.0f * Scale, 0.0f)) && Picked)
	{
		StartNewColony(true);
	}
	NgEndPanel();
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
	PeopleSelected = FMath::Clamp(PeopleSelected, 0, static_cast<int32>(People.size()) - 1);
	const colony::Entity Picked = People[static_cast<size_t>(PeopleSelected)];
	const entt::registry& Reg = Sim->things().registry();

	// Every colonist gets a creator look the first time it is seen (the 3D body the creator draws), seeded by the game,
	// its slot and its rerolls, so a reroll is a new face too.
	for (int32 Slot = 0; Slot < static_cast<int32>(People.size()); ++Slot)
	{
		const colony::Entity Pawn = People[static_cast<size_t>(Slot)];
		const colony::PawnLooks* Looks = Reg.try_get<colony::PawnLooks>(Pawn);
		const colony::Colonist* Colonist = Sim->colonistOf(Pawn);
		const uint32 Key = static_cast<uint32>(entt::to_integral(Pawn));
		if (Looks && Colonist && Looks->creator.empty() && !PeopleLooks.Contains(Key))
		{
			CreatorPeople::FLook Look;
			Look.Seed = static_cast<int32>((Sim->settings().seed * 7919u + static_cast<uint64>(Slot) * 104729u + static_cast<uint64>(PeopleRerolls) * 15485863u) & 0x7fffffff);
			Look.bFemale = Colonist->female;
			SubmitPeopleLook(Pawn, Slot, Look);
		}
	}
	UpdatePeopleCamera(Picked);

	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float Pad = 24.0f * Scale;
	const float Top = Viewport->Pos.y + Pad;
	const float FooterH = 64.0f * Scale;
	const float Bottom = Viewport->Pos.y + Viewport->Size.y - Pad - FooterH;
	const float LeftW = 330.0f * Scale;
	const float RightW = 430.0f * Scale;

	// Header: title and the races (only humans for now, the rest locked).
	if (NgBeginPanel("##NgPeopleHead", ImVec2(Viewport->Pos.x + Pad, Top), ImVec2(Viewport->Size.x - Pad * 2.0f, 54.0f * Scale)))
	{
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
		ColonyHud::TextColored(ColonyHud::TextMain(), TEXT("Your crew"));
		ImGui::PopFont();
		ImGui::SameLine(LeftW + Pad);
		for (const FNgRace& Race : NgRaces)
		{
			const colony::Def* Lineage = Defs->find("Lineages", Race.Lineage);
			const bool bOpen = FCStringAnsi::Strcmp(Race.Lineage, "Baseline") == 0;
			const FString Label = bOpen ? FString(Race.Fallback) : (Lineage ? NgCapital(Lineage->label()) : FString(Race.Fallback));
			ImGui::BeginDisabled(!bOpen);
			ColonyHud::Button(bOpen ? Label : FString::Printf(TEXT("%s (locked)"), *Label), bOpen, ImVec2(150.0f * Scale, 0.0f));
			ImGui::EndDisabled();
			if (!bOpen && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			{
				ImGui::SetTooltip("Not playable yet: this race comes later.");
			}
			ImGui::SameLine();
		}
	}
	NgEndPanel();

	// Left: the people as cards, the player first.
	const float BodyTop = Top + 54.0f * Scale + Pad * 0.5f;
	if (NgBeginPanel("##NgPeopleCards", ImVec2(Viewport->Pos.x + Pad, BodyTop), ImVec2(LeftW, Bottom - BodyTop)))
	{
		ColonyHud::TextColored(ColonyHud::Accent(), FString::Printf(TEXT("Starting people (%d)"), static_cast<int32>(People.size())));
		NgRule();
		const float Face = 72.0f * Scale;
		for (int32 Slot = 0; Slot < static_cast<int32>(People.size()); ++Slot)
		{
			const colony::Entity Pawn = People[static_cast<size_t>(Slot)];
			const colony::Colonist* Colonist = Sim->colonistOf(Pawn);
			if (!Colonist)
			{
				continue;
			}
			ImGui::PushID(Slot);
			const bool bPicked = Slot == PeopleSelected;
			ImGui::PushStyleColor(ImGuiCol_ChildBg, bPicked ? ColonyHud::Rgb(0x3a2c17, 0.75f) : ColonyHud::Rgb(0x1c2126, 0.6f));
			ImGui::PushStyleColor(ImGuiCol_Border, bPicked ? ColonyHud::Accent() : ColonyHud::Border());
			ImGui::BeginChild("##card", ImVec2(0.0f, Face + 20.0f * Scale), ImGuiChildFlags_Borders);
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
			ImGui::SameLine();
			ImGui::BeginGroup();
			ColonyHud::TextColored(Colonist->protagonist ? ColonyHud::Accent() : ColonyHud::TextDim(), Colonist->protagonist ? TEXT("YOU") : TEXT("STRANGER"));
			ColonyHud::Text(FString::Printf(TEXT("%s %s"), UTF8_TO_TCHAR(Colonist->firstName.c_str()), UTF8_TO_TCHAR(Colonist->lastName.c_str())));
			ColonyHud::TextColored(ColonyHud::TextFaint(), FString::Printf(TEXT("%s, %d"), *NgDefLabel(*Defs, "Backstories", Colonist->adulthood, "titleShort"), Colonist->age));
			if (ColonyHud::Button(TEXT("Select"), bPicked))
			{
				PeopleSelected = Slot;
			}
			ImGui::SameLine();
			if (ColonyHud::Button(TEXT("Reroll")))
			{
				++PeopleRerolls;
				PeopleLooks.Remove(static_cast<uint32>(entt::to_integral(Pawn)));
				PeopleNamesFor = colony::kNoEntity;
				Submit(colony::RerollColonistCommand{Slot});
			}
			ColonyHud::Tip(TEXT("A different person: new name, story, traits, skills and looks"));
			ImGui::EndGroup();
			ImGui::EndChild();
			ImGui::PopStyleColor(2);
			ImGui::PopID();
		}
		ImGui::Spacing();
		if (ColonyHud::Button(TEXT("Reroll the strangers"), false, ImVec2(-1.0f, 0.0f)))
		{
			for (int32 Slot = 1; Slot < static_cast<int32>(People.size()); ++Slot)
			{
				++PeopleRerolls;
				PeopleLooks.Remove(static_cast<uint32>(entt::to_integral(People[static_cast<size_t>(Slot)])));
				Submit(colony::RerollColonistCommand{Slot});
			}
			PeopleNamesFor = colony::kNoEntity;
		}
		ImGui::Spacing();
		ColonyHud::TextColored(ColonyHud::TextFaint(), TEXT("Drag in the middle to turn, wheel to zoom."));
	}
	NgEndPanel();

	// Right: the picked one's story or looks.
	const colony::Colonist* Colonist = Sim->colonistOf(Picked);
	if (Colonist && NgBeginPanel("##NgPeopleInfo", ImVec2(Viewport->Pos.x + Viewport->Size.x - Pad - RightW, BodyTop), ImVec2(RightW, Bottom - BodyTop)))
	{
		if (ColonyHud::Button(TEXT("Story and skills"), PeopleTab == 0, ImVec2(RightW * 0.46f, 0.0f)))
		{
			PeopleTab = 0;
		}
		ImGui::SameLine();
		if (ColonyHud::Button(TEXT("Looks"), PeopleTab == 1, ImVec2(RightW * 0.46f, 0.0f)))
		{
			PeopleTab = 1;
		}
		NgRule();
		const int32 Slot = PeopleSelected;
		if (PeopleTab == 0)
		{
			// Names (typed, sent when the field is left).
			if (PeopleNamesFor != Picked)
			{
				PeopleNamesFor = Picked;
				FCStringAnsi::Strncpy(PeopleFirst, Colonist->firstName.c_str(), UE_ARRAY_COUNT(PeopleFirst));
				FCStringAnsi::Strncpy(PeopleNick, Colonist->nickName.c_str(), UE_ARRAY_COUNT(PeopleNick));
				FCStringAnsi::Strncpy(PeopleLast, Colonist->lastName.c_str(), UE_ARRAY_COUNT(PeopleLast));
			}
			const float Third = (ImGui::GetContentRegionAvail().x - 12.0f * Scale) / 3.0f;
			bool bNames = false;
			ImGui::BeginGroup();
			ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("First"));
			ImGui::SetNextItemWidth(Third);
			ImGui::InputText("##first", PeopleFirst, 25);
			bNames |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::EndGroup();
			ImGui::SameLine();
			ImGui::BeginGroup();
			ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Nickname"));
			ImGui::SetNextItemWidth(Third);
			ImGui::InputText("##nick", PeopleNick, 25);
			bNames |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::EndGroup();
			ImGui::SameLine();
			ImGui::BeginGroup();
			ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Last"));
			ImGui::SetNextItemWidth(Third);
			ImGui::InputText("##last", PeopleLast, 25);
			bNames |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::EndGroup();
			if (bNames && PeopleFirst[0] != 0)
			{
				colony::EditColonistCommand Edit;
				Edit.slot = Slot;
				Edit.firstName = PeopleFirst;
				Edit.nickName = PeopleNick[0] != 0 ? PeopleNick : PeopleFirst;
				Edit.lastName = PeopleLast;
				Submit(Edit);
			}
			ColonyHud::TextColored(ColonyHud::TextDim(), FString::Printf(TEXT("%s, age %d"), Colonist->female ? TEXT("Woman") : TEXT("Man"), Colonist->age));
			ImGui::Spacing();
			const auto Story = [&](const TCHAR* Heading, const std::string& Name)
			{
				ColonyHud::TextColored(ColonyHud::Accent(), Heading);
				ColonyHud::Text(NgDefLabel(*Defs, "Backstories", Name, "title"));
				if (const colony::Def* Def = Defs->find("Backstories", Name))
				{
					ImGui::PushTextWrapPos(0.0f);
					ColonyHud::TextColored(ColonyHud::TextFaint(), UTF8_TO_TCHAR(Def->text("description").c_str()));
					ImGui::PopTextWrapPos();
				}
				ImGui::Spacing();
			};
			Story(TEXT("Childhood"), Colonist->childhood);
			Story(TEXT("Adulthood"), Colonist->adulthood);
			ColonyHud::TextColored(ColonyHud::Accent(), FString::Printf(TEXT("Traits (%d)"), static_cast<int32>(Colonist->traits.size())));
			for (const colony::TraitRecord& Trait : Colonist->traits)
			{
				ColonyHud::Button(FString::Printf(TEXT("%s##trait%s"), *NgTraitLabel(*Defs, Trait), UTF8_TO_TCHAR(Trait.def.c_str())), true);
				ImGui::SameLine();
			}
			ImGui::NewLine();
			if (!Colonist->disabledWorkTags.empty())
			{
				FString Off;
				for (const std::string& Tag : Colonist->disabledWorkTags)
				{
					Off += (Off.IsEmpty() ? TEXT("") : TEXT(", ")) + FString(UTF8_TO_TCHAR(Tag.c_str())).ToLower();
				}
				ColonyHud::TextColored(ColonyHud::Bad(), FString::Printf(TEXT("Won't do: %s"), *Off));
			}
			ImGui::Spacing();
			ColonyHud::TextColored(ColonyHud::Accent(), TEXT("Skills"));
			const float BarW = ImGui::GetContentRegionAvail().x - 190.0f * Scale;
			for (const colony::SkillRecord& Skill : Colonist->skills)
			{
				ColonyHud::Text(NgDefLabel(*Defs, "Skills", Skill.def));
				ImGui::SameLine(120.0f * Scale);
				if (Skill.disabled)
				{
					ColonyHud::TextColored(ColonyHud::TextFaint(), TEXT("-"));
					continue;
				}
				const ImVec2 P = ImGui::GetCursorScreenPos();
				const float H = ImGui::GetTextLineHeight() * 0.55f;
				ImDrawList* Draw = ImGui::GetWindowDrawList();
				const float Y = P.y + (ImGui::GetTextLineHeight() - H) * 0.5f;
				Draw->AddRectFilled(ImVec2(P.x, Y), ImVec2(P.x + BarW, Y + H), IM_COL32(40, 44, 48, 200), 2.0f);
				Draw->AddRectFilled(ImVec2(P.x, Y), ImVec2(P.x + BarW * FMath::Clamp(Skill.level / 20.0f, 0.0f, 1.0f), Y + H), IM_COL32(224, 176, 74, 230), 2.0f);
				ImGui::Dummy(ImVec2(BarW, ImGui::GetTextLineHeight()));
				ImGui::SameLine();
				ColonyHud::Text(FString::Printf(TEXT("%2d"), Skill.level));
				ImGui::SameLine();
				NgFlames(Skill.passion, ImGui::GetTextLineHeight());
			}
		}
		else
		{
			// Looks: the creator's sliders for the human body, applied live and sent when let go.
			const uint32 Key = static_cast<uint32>(entt::to_integral(Picked));
			CreatorPeople::FLook Look;
			const colony::PawnLooks* SimLooks = Reg.try_get<colony::PawnLooks>(Picked);
			const FString* Local = PeopleLooks.Find(Key);
			const FString Text = Local ? *Local : (SimLooks ? FString(UTF8_TO_TCHAR(SimLooks->creator.c_str())) : FString());
			if (!CreatorPeople::Decode(Text, Look))
			{
				ColonyHud::TextColored(ColonyHud::TextFaint(), TEXT("Getting the body ready..."));
			}
			else
			{
				CreatorPeople::FLook Full = CreatorPeople::Expanded(Look);
				bool bLive = false;  // changed this frame (show it now)
				bool bSend = false;  // finished a change (send it to the sim)
				const auto Slider = [&](const char* Label, const TCHAR* Name, float Min, float Max)
				{
					float Value = Full.Get(Name, 0.0f);
					ColonyHud::TextColored(ColonyHud::TextDim(), UTF8_TO_TCHAR(Label));
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::SliderFloat(TCHAR_TO_UTF8(*FString::Printf(TEXT("##%s"), Name)), &Value, Min, Max, ""))
					{
						Look.Values.Add(Name, Value);
						bLive = true;
					}
					bSend |= ImGui::IsItemDeactivatedAfterEdit();
				};
				// Gender: the other base body, the same random person otherwise.
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Body"));
				if (ColonyHud::Button(TEXT("Woman"), Look.bFemale, ImVec2(120.0f * Scale, 0.0f)) && !Look.bFemale)
				{
					colony::EditColonistCommand Edit;
					Edit.slot = Slot;
					Edit.female = 1;
					Submit(Edit);
					Look.bFemale = true;
					Look.Values.Remove(TEXT("breasts"));
					Look.Values.Remove(TEXT("hairStyle"));
					bSend = true;
				}
				ImGui::SameLine();
				if (ColonyHud::Button(TEXT("Man"), !Look.bFemale, ImVec2(120.0f * Scale, 0.0f)) && Look.bFemale)
				{
					colony::EditColonistCommand Edit;
					Edit.slot = Slot;
					Edit.female = 0;
					Submit(Edit);
					Look.bFemale = false;
					Look.Values.Remove(TEXT("breasts"));
					Look.Values.Remove(TEXT("hairStyle"));
					bSend = true;
				}
				Slider("Height", TEXT("height"), Look.bFemale ? 0.86f : 0.92f, Look.bFemale ? 1.04f : 1.12f);
				Slider("Thin to heavy", TEXT("weight"), -0.8f, 0.9f);
				Slider("Soft to muscular", TEXT("muscle"), -0.7f, 0.8f);
				if (Look.bFemale)
				{
					Slider("Breasts", TEXT("breasts"), -0.9f, 0.9f);
				}
				Slider(Look.bFemale ? "More masculine" : "More feminine", TEXT("sex"), 0.0f, 0.5f);
				Slider("Older", TEXT("older"), 0.0f, 0.8f);
				ImGui::Spacing();
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Skin"));
				const float Swatch = 26.0f * Scale;
				for (int32 i = 0; i < static_cast<int32>(UE_ARRAY_COUNT(NgSkinSwatches)); ++i)
				{
					if (i > 0)
					{
						ImGui::SameLine();
					}
					if (NgSwatch(TCHAR_TO_UTF8(*FString::Printf(TEXT("##skin%d"), i)), NgSkinSwatches[i], NgSame(Full, TEXT("skin"), NgSkinSwatches[i]), Swatch))
					{
						NgSetColour(Look, TEXT("skin"), NgSkinSwatches[i]);
						bLive = bSend = true;
					}
				}
				ImGui::Spacing();
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Hair"));
				{
					UWorld* World = GetWorld();
					USimPawnSubsystem* Pawns = World ? World->GetSubsystem<USimPawnSubsystem>() : nullptr;
					const ASimPawnActor* Actor = Pawns ? Pawns->FindPawn(Picked) : nullptr;
					const int32 Current = FMath::RoundToInt(Full.Get(TEXT("hairStyle"), 0.0f));
					int32 Shown = 0;
					for (int32 h = 0; h < CreatorPeople::HairStyleCount(); ++h)
					{
						if (!Actor || !CreatorPeople::HasHairStyle(h, *Actor))
						{
							continue;
						}
						if (Shown++ % 4 != 0)
						{
							ImGui::SameLine();
						}
						if (ColonyHud::Button(FString::Printf(TEXT("%s##hair"), *CreatorPeople::HairStyleLabel(h)), Current == h, ImVec2(96.0f * Scale, 0.0f)))
						{
							Look.Values.Add(TEXT("hairStyle"), static_cast<float>(h));
							bLive = bSend = true;
						}
					}
				}
				for (int32 i = 0; i < static_cast<int32>(UE_ARRAY_COUNT(NgHairSwatches)); ++i)
				{
					if (i > 0)
					{
						ImGui::SameLine();
					}
					if (NgSwatch(TCHAR_TO_UTF8(*FString::Printf(TEXT("##hairc%d"), i)), NgHairSwatches[i], NgSame(Full, TEXT("hair"), NgHairSwatches[i]), Swatch))
					{
						NgSetColour(Look, TEXT("hair"), NgHairSwatches[i]);
						bLive = bSend = true;
					}
				}
				ImGui::Spacing();
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Eyes"));
				for (int32 i = 0; i < static_cast<int32>(UE_ARRAY_COUNT(NgEyeSwatches)); ++i)
				{
					if (i > 0)
					{
						ImGui::SameLine();
					}
					if (NgSwatch(TCHAR_TO_UTF8(*FString::Printf(TEXT("##eye%d"), i)), NgEyeSwatches[i], NgSame(Look, TEXT("iris"), NgEyeSwatches[i]), Swatch))
					{
						NgSetColour(Look, TEXT("iris"), NgEyeSwatches[i]);
						bLive = bSend = true;
					}
				}
				ImGui::Spacing();
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Body hair below"));
				static const TCHAR* const PubicNames[] = {TEXT("None"), TEXT("Natural"), TEXT("Trimmed"), TEXT("Shaved")};
				const int32 Pubic = FMath::RoundToInt(Full.Get(TEXT("pubic"), 1.0f));
				for (int32 i = 0; i < 4; ++i)
				{
					if (i > 0)
					{
						ImGui::SameLine();
					}
					if (ColonyHud::Button(FString::Printf(TEXT("%s##pubic"), PubicNames[i]), Pubic == i, ImVec2(90.0f * Scale, 0.0f)))
					{
						Look.Values.Add(TEXT("pubic"), static_cast<float>(i));
						bLive = bSend = true;
					}
				}
				ImGui::Spacing();
				if (ColonyHud::Button(TEXT("Random looks"), false, ImVec2(-1.0f, 0.0f)))
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
		}
	}
	if (Colonist)
	{
		NgEndPanel();
	}

	// Footer: back, the steps, and the landing.
	if (NgBeginPanel("##NgPeopleFoot", ImVec2(Viewport->Pos.x + Pad, Bottom + Pad * 0.5f), ImVec2(Viewport->Size.x - Pad * 2.0f, FooterH - Pad * 0.5f)))
	{
		const float FootLeft = ImGui::GetCursorPosX();
		const float FootRight = FootLeft + ImGui::GetContentRegionAvail().x;
		if (ColonyHud::Button(TEXT("Back"), false, ImVec2(160.0f * Scale, 0.0f)))
		{
			EndPeopleCamera();
			MainMenuPage = 1;
		}
		ImGui::SameLine((FootLeft + FootRight) * 0.5f - 200.0f * Scale);
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Scenario  >  Landing site  >  "));
		ImGui::SameLine(0.0f, 0.0f);
		ColonyHud::TextColored(ColonyHud::Accent(), TEXT("People"));
		ImGui::SameLine(FootRight - 220.0f * Scale);
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
		const bool bStart = ColonyHud::Button(TEXT("Start"), true, ImVec2(220.0f * Scale, 0.0f));
		ImGui::PopFont();
		if (bStart)
		{
			NgEndPanel();
			LandColony(Bridge);
			return;
		}
	}
	NgEndPanel();
}
