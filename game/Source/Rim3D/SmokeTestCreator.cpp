// Not a check: `creator` (tools/play-creator.bat) is the first live character creator (docs/design/makehuman.md,
// "Character creator"): three colonists on the MakeHuman creator bases (a woman, a man and an imp made from the man's
// base), standing in a row with the graphics up, and a panel of sliders that change the selected one as you drag (morph
// targets on its mesh, its height and skin colour). They can walk back and forth to show the jiggle and the tail.
#include "SmokeTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "Misc/Parse.h"

#include "MakeHumanAnimInstance.h"
#include "PawnActor.h"
#include "PawnLooks.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <imgui.h>
#include <vector>

namespace
{
	/** One character in the creator: which base, its slider values, and its colonist. */
	struct FCreatorPerson
	{
		FString Label;
		FString Base;             // CreatorFemale or CreatorMale
		bool bFemale = true;
		colony::Entity Thing = colony::kNoEntity;
		float Height = 1.0f;      // scale of the mesh
		float Sex = 0.0f;         // 0 .. 1 towards the other sex (Masculine on the woman's base, Feminine on the man's)
		float Older = 0.0f;
		float Weight = 0.0f;      // -1 thin .. 1 heavy
		float Muscle = 0.0f;      // -1 soft .. 1 muscular
		float Breasts = 0.0f;     // -1 small .. 1 large (woman's base)
		float ImpEars = 0.0f;
		float ImpFace = 0.0f;
		float Horns = 0.0f;
		float Tail = 0.0f;
		float Skin[3] = {0.91f, 0.80f, 0.75f};
	};

	struct FCreatorState
	{
		TArray<FCreatorPerson> People;
		int32 Selected = 0;
		float CameraYaw = 0.0f;   // degrees round the selected one, 0 in front
		bool bFaced = false;
		float DemoTime = -1.0f;   // -CreatorDemo: seconds into the self-running demo
		int32 Recorded = 0;
		int32 PanelFrames = 0;
		float CameraDistance = 420.0f;
		float CameraHeight = 120.0f;
		bool bWalk = false;
		float LegTime = 99.0f;
		int32 Leg = 0;
		colony::CellPos Middle = colony::CellPos{48, 48, 0};
		float LargestTailSwing = 0.0f;
		float LargestJiggle = 0.0f;
	};

	const FVector CreatorOrigin(-1695000.0, 4800.0, 0.0);

	void CreatorRegular(FCreatorPerson& P)
	{
		const bool bFemale = P.bFemale;
		P = FCreatorPerson{P.Label, P.Base, bFemale, P.Thing};
		if (!bFemale)
		{
			P.Skin[0] = 0.85f;
			P.Skin[1] = 0.70f;
			P.Skin[2] = 0.60f;
			P.Height = 1.02f;
		}
		else
		{
			P.Height = 0.94f;
			P.Weight = -0.2f;
		}
	}

	void CreatorImp(FCreatorPerson& P)
	{
		CreatorRegular(P);
		P.Height = 0.82f;
		P.Weight = -0.6f;
		P.Muscle = 0.6f;
		P.ImpEars = 1.0f;
		P.ImpFace = 1.0f;
		P.Horns = 1.0f;
		P.Tail = 1.0f;
		P.Skin[0] = 0.72f;
		P.Skin[1] = 0.17f;
		P.Skin[2] = 0.12f;
	}

	/** Puts a person's sliders on its mesh: morphs, height, skin colour. */
	void CreatorApply(const FCreatorPerson& P, ASimPawnActor& Pawn)
	{
		USkeletalMeshComponent* Mesh = Pawn.GetMakeHumanMesh();
		if (!Mesh)
		{
			return;
		}
		const auto Set = [Mesh](const TCHAR* Name, float Value) { Mesh->SetMorphTarget(Name, FMath::Clamp(Value, 0.0f, 1.0f), false); };
		Set(P.bFemale ? TEXT("Masculine") : TEXT("Feminine"), P.Sex);
		Set(TEXT("Older"), P.Older);
		Set(TEXT("Heavy"), P.Weight);
		Set(TEXT("Thin"), -P.Weight);
		Set(TEXT("Muscular"), P.Muscle);
		Set(TEXT("Soft"), -P.Muscle);
		if (P.bFemale)
		{
			Set(TEXT("BreastsLarge"), P.Breasts);
			Set(TEXT("BreastsSmall"), -P.Breasts);
		}
		Set(TEXT("ImpEars"), P.ImpEars);
		Set(TEXT("ImpFace"), P.ImpFace);
		Set(TEXT("NoHorns"), 1.0f - P.Horns);
		Set(TEXT("NoTail"), 1.0f - P.Tail);
		Mesh->SetRelativeScale3D(FVector(P.Height));
		// The panel's colour is as seen (sRGB); the skin material takes it linear, as FSimPawnLooks::Skin is.
		const FLinearColor Skin = FLinearColor::FromSRGBColor(FLinearColor(P.Skin[0], P.Skin[1], P.Skin[2]).ToFColor(false));
		Mesh->SetCustomPrimitiveDataVector3(PawnLooksSkinDataIndex, FVector(Skin.R, Skin.G, Skin.B));
	}
}

void USmokeTestSubsystem::BuildCreatorPhases()
{
	const TSharedRef<FCreatorState> State = MakeShared<FCreatorState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto PawnsOf = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };

	FPhase Play;
	Play.Name = TEXT("creator");
	Play.WarmupSeconds = 6.0f;
	float Minutes = 240.0f;
	FParse::Value(FCommandLine::Get(), TEXT("CreatorMinutes="), Minutes);
	Play.MeasureSeconds = Minutes * 60.0f;
	Play.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!B || !Pawns)
		{
			Fail(TEXT("USimSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			for (const TCHAR* Command : {TEXT("sg.ShadowQuality 4"), TEXT("sg.GlobalIlluminationQuality 4"), TEXT("sg.ReflectionQuality 4"),
				TEXT("sg.PostProcessQuality 4"), TEXT("sg.TextureQuality 4"), TEXT("sg.EffectsQuality 4"), TEXT("sg.ShadingQuality 4"),
				TEXT("sg.AntiAliasingQuality 4"), TEXT("r.ScreenPercentage 100"), TEXT("r.AntiAliasingMethod 4"), TEXT("r.DynamicGlobalIlluminationMethod 1"),
				TEXT("r.ReflectionMethod 1"), TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor 8"), TEXT("r.Shadow.Virtual.Enable 1"),
				TEXT("r.SSS.Quality 1"), TEXT("r.SSS.HalfRes 0"), TEXT("r.SSS.Checkerboard 0"), TEXT("r.ContactShadows 1"), TEXT("Pawns.FullDetail 20"),
				TEXT("Overseer.UI 0"), TEXT("DisableAllScreenMessages")})
			{
				PC->ConsoleCommand(Command);
			}
		}
		for (const TPair<const TCHAR*, float>& Var : {TPair<const TCHAR*, float>(TEXT("Pawns.MetaHumans"), 1.0f), TPair<const TCHAR*, float>(TEXT("Sky.Hour"), 9.0f),
			TPair<const TCHAR*, float>(TEXT("Sky.DayOfYear"), 22.5f)})
		{
			if (IConsoleVariable* Found = IConsoleManager::Get().FindConsoleVariable(Var.Key))
			{
				Found->Set(Var.Value, ECVF_SetByCode);
			}
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 96;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(CreatorOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		B->StepOnce();
		State->Middle = B->GetSim()->stockpileCell().value_or(colony::CellPos{48, 48, 0});
		const colony::CellPos Middle = State->Middle;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Pawn && FMath::Abs(View.x - Middle.x) < 24.0f && FMath::Abs(View.y - Middle.y) < 24.0f)
			{
				B->Submit(colony::DestroyThingCommand{View.id});
			}
		}
		colony::Sim* Sim = const_cast<colony::Sim*>(B->GetSim());
		for (int32 x = -24; x <= 24; ++x)
		{
			for (int32 y = -24; y <= 24; ++y)
			{
				Sim->plants().remove(colony::CellPos{Middle.x + x, Middle.y + y, 0});
			}
		}
		B->StepOnce();
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		TArray<const colony::ThingView*> Colonists;
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.lifeStage < 0)
			{
				Colonists.Add(&View);
			}
		}
		Colonists.Sort([](const colony::ThingView& A, const colony::ThingView& Bv) { return A.id < Bv.id; });
		if (Colonists.Num() < 1)
		{
			Fail(TEXT("the Crashlanded start has no colonists"));
			return;
		}
		// A regular woman, a regular man and an imp made from the man's base.
		const int32 Count = FMath::Min(3, Colonists.Num());
		const colony::DefDatabase* Defs = B->GetDefs().get();
		for (int32 i = 0; i < Colonists.Num(); ++i)
		{
			if (i >= Count)
			{
				B->Submit(colony::DestroyThingCommand{Colonists[i]->id});
				continue;
			}
			FCreatorPerson Person;
			Person.Label = i == 0 ? TEXT("Woman") : (i == 1 ? TEXT("Man") : TEXT("Imp"));
			Person.bFemale = i == 0;
			Person.Base = Person.bFemale ? TEXT("CreatorFemale") : TEXT("CreatorMale");
			Person.Thing = Colonists[i]->id;
			if (i == 2)
			{
				CreatorImp(Person);
			}
			else
			{
				CreatorRegular(Person);
			}
			State->People.Add(Person);
			FSimPawnLooks Looks = Colonists[i]->looks >= 0 ? USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(Colonists[i]->looks)], Defs) : FSimPawnLooks();
			Looks.bFemale = Person.bFemale;
			Looks.Height = 1.0f;
			Looks.Skin = FLinearColor(Person.Skin[0], Person.Skin[1], Person.Skin[2]);
			Looks.MakeHuman = Person.Base;
			Pawns->SetLooksOverride(Person.Thing, Looks);
			for (const colony::Entity Item : Sim->apparel().worn(Person.Thing))
			{
				std::string Error;
				Sim->apparel().takeOff(Person.Thing, Item, Error);
				B->Submit(colony::DestroyThingCommand{Item});
			}
			B->Submit(colony::DraftCommand{Person.Thing, true});
			B->Submit(colony::MoveOrderCommand{Person.Thing, colony::CellPos{Middle.x - 1, Middle.y + 6 - 2 + i * 2, 0}});
		}
		B->StepOnce();
		B->SetSpeed(1);
		// For pictures: -CreatorSelect=<0-2>, -CreatorWalk, -CreatorYaw=<degrees>, -CreatorDistance=<cm>.
		FParse::Value(FCommandLine::Get(), TEXT("CreatorSelect="), State->Selected);
		State->bWalk = FParse::Param(FCommandLine::Get(), TEXT("CreatorWalk"));
		FParse::Value(FCommandLine::Get(), TEXT("CreatorYaw="), State->CameraYaw);
		FParse::Value(FCommandLine::Get(), TEXT("CreatorDistance="), State->CameraDistance);
	};
	Play.OnFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!B || !Pawns || State->People.IsEmpty())
		{
			return;
		}
		// Once they stand in their row, a step towards +X: they face the view (which looks from +X).
		State->LegTime += GetWorld()->GetDeltaSeconds();
		if (!State->bFaced && PhaseTime > 3.0f)
		{
			State->bFaced = true;
			for (int32 i = 0; i < State->People.Num(); ++i)
			{
				B->Submit(colony::MoveOrderCommand{State->People[i].Thing, colony::CellPos{State->Middle.x, State->Middle.y + 6 - 2 + i * 2, 0}});
			}
		}
		// Walking: back and forth along X, side by side, so the jiggle and the tail show.
		if (State->bWalk && State->LegTime > 4.0f)
		{
			State->LegTime = 0.0f;
			const int32 X = State->Leg % 2 == 0 ? -8 : 2;
			for (int32 i = 0; i < State->People.Num(); ++i)
			{
				B->Submit(colony::MoveOrderCommand{State->People[i].Thing, colony::CellPos{State->Middle.x + X, State->Middle.y + 6 - 2 + i * 2, 0}});
			}
			++State->Leg;
		}
		// -CreatorDemo: the sliders move by themselves on the woman (body, then into an imp), then they all walk; with
		// -CreatorRecord=<seconds> every frame is saved at a fixed 30 a second (creator/rec_0000.png ...).
		if (FParse::Param(FCommandLine::Get(), TEXT("CreatorDemo")) && PhaseTime > 4.5f && State->People.Num() >= 3)
		{
			float RecordSeconds = 0.0f;
			FParse::Value(FCommandLine::Get(), TEXT("CreatorRecord="), RecordSeconds);
			if (State->DemoTime < 0.0f)
			{
				State->DemoTime = 0.0f;
				if (RecordSeconds > 0.0f)
				{
					FApp::SetUseFixedTimeStep(true);
					FApp::SetFixedDeltaTime(1.0 / 30.0);
				}
			}
			State->DemoTime += GetWorld()->GetDeltaSeconds();
			const float T = State->DemoTime;
			FCreatorPerson& W = State->People[0];
			const auto Wave = [T](float From, float To) { return T < From || T > To ? 0.0f : FMath::Sin(PI * 2.0f * (T - From) / (To - From)); };
			const auto Ramp = [T](float From, float To) { return FMath::Clamp((T - From) / (To - From), 0.0f, 1.0f); };
			State->Selected = T < 17.0f ? 0 : (T < 22.0f ? 2 : 1);
			State->CameraDistance = T < 17.0f ? 260.0f : 380.0f;
			State->CameraYaw = T < 17.0f ? 0.0f : 75.0f;
			W.Weight = -0.2f + Wave(0.0f, 3.0f);
			W.Muscle = Wave(3.0f, 6.0f);
			W.Breasts = Wave(6.0f, 9.0f);
			W.Older = FMath::Max(0.0f, Wave(9.0f, 12.0f));
			const float Imp = Ramp(12.0f, 15.0f);
			W.ImpEars = W.ImpFace = W.Horns = W.Tail = Imp;
			W.Skin[0] = FMath::Lerp(0.91f, 0.72f, Imp);
			W.Skin[1] = FMath::Lerp(0.80f, 0.17f, Imp);
			W.Skin[2] = FMath::Lerp(0.75f, 0.12f, Imp);
			if (T > 16.0f && !State->bWalk)
			{
				State->bWalk = true;
				State->LegTime = 99.0f;
			}
			if (RecordSeconds > 0.0f && State->Recorded < FMath::RoundToInt32(RecordSeconds * 30.0f))
			{
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke/creator") / FString::Printf(TEXT("rec_%04d.png"), State->Recorded++), false, false);
			}
		}
		for (FCreatorPerson& P : State->People)
		{
			if (ASimPawnActor* Pawn = Pawns->FindPawn(P.Thing))
			{
				CreatorApply(P, *Pawn);
				if (const USkeletalMeshComponent* Mesh = Pawn->GetMakeHumanMesh())
				{
					if (UMakeHumanAnimInstance* Anim = Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()))
					{
						State->LargestTailSwing = FMath::Max(State->LargestTailSwing, Anim->TakeLargestTailSwing());
						State->LargestJiggle = FMath::Max(State->LargestJiggle, Anim->TakeLargestJiggle());
					}
				}
			}
		}
		// The camera round the selected one.
		FCreatorPerson& Sel = State->People[FMath::Clamp(State->Selected, 0, State->People.Num() - 1)];
		if (const ASimPawnActor* Pawn = Pawns->FindPawn(Sel.Thing))
		{
			const FVector Target = Pawn->GetActorLocation() + FVector(0.0, 0.0, 100.0 * Sel.Height);
			// From +X (in front of them as they stand), turned round by the slider.
			const FRotator Around(0.0, State->CameraYaw, 0.0);
			const FVector From = Pawn->GetActorLocation() + Around.Vector() * State->CameraDistance + FVector(0.0, 0.0, State->CameraHeight);
			ViewFrom(From, (Target - From).Rotation());
		}
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			PC->bShowMouseCursor = true;
		}

		// The panel.
		const ImGui::FScopedContext ScopedContext;
		if (!ScopedContext)
		{
			return;
		}
		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
		ImGui::GetStyle().FontScaleMain = 1.2f * Scale;
		ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + 16.0f * Scale, Viewport->Pos.y + 16.0f * Scale), ImGuiCond_Once);
		ImGui::SetNextWindowBgAlpha(0.8f);
		if (ImGui::Begin("Character creator", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			for (int32 i = 0; i < State->People.Num(); ++i)
			{
				if (i > 0)
				{
					ImGui::SameLine();
				}
				if (ImGui::RadioButton(TCHAR_TO_UTF8(*State->People[i].Label), State->Selected == i))
				{
					State->Selected = i;
				}
			}
			ImGui::Separator();
			FCreatorPerson& P = State->People[FMath::Clamp(State->Selected, 0, State->People.Num() - 1)];
			ImGui::Text("Base: %s", P.bFemale ? "woman" : "man");
			ImGui::SliderFloat("Height", &P.Height, 0.75f, 1.15f, "%.2f");
			ImGui::SliderFloat(P.bFemale ? "Masculine" : "Feminine", &P.Sex, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat("Older", &P.Older, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat("Thin / heavy", &P.Weight, -1.0f, 1.0f, "%.2f");
			ImGui::SliderFloat("Soft / muscular", &P.Muscle, -1.0f, 1.0f, "%.2f");
			if (P.bFemale)
			{
				ImGui::SliderFloat("Breasts", &P.Breasts, -1.0f, 1.0f, "%.2f");
			}
			ImGui::Separator();
			ImGui::TextUnformatted("Inhuman");
			ImGui::SliderFloat("Pointed ears", &P.ImpEars, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat("Imp face", &P.ImpFace, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat("Horns", &P.Horns, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat("Tail", &P.Tail, 0.0f, 1.0f, "%.2f");
			ImGui::ColorEdit3("Skin", P.Skin);
			ImGui::Separator();
			if (ImGui::Button("Regular"))
			{
				CreatorRegular(P);
			}
			ImGui::SameLine();
			if (ImGui::Button("Imp"))
			{
				CreatorImp(P);
			}
			ImGui::SameLine();
			if (ImGui::Button("Random"))
			{
				FRandomStream R(FPlatformTime::Cycles());
				CreatorRegular(P);
				P.Height = R.FRandRange(0.85f, 1.1f);
				P.Sex = R.FRandRange(0.0f, 0.35f);
				P.Older = R.FRand() < 0.3f ? R.FRandRange(0.2f, 1.0f) : 0.0f;
				P.Weight = R.FRandRange(-0.8f, 0.8f);
				P.Muscle = R.FRandRange(-0.6f, 0.8f);
				P.Breasts = R.FRandRange(-0.8f, 0.8f);
				const float Tone = R.FRand();
				P.Skin[0] = FMath::Lerp(0.95f, 0.36f, Tone);
				P.Skin[1] = FMath::Lerp(0.84f, 0.22f, Tone);
				P.Skin[2] = FMath::Lerp(0.78f, 0.15f, Tone);
			}
			ImGui::Separator();
			ImGui::Checkbox("Walk back and forth", &State->bWalk);
			ImGui::SliderFloat("View round", &State->CameraYaw, -180.0f, 180.0f, "%.0f");
			ImGui::SliderFloat("View distance", &State->CameraDistance, 120.0f, 700.0f, "%.0f");
			ImGui::SliderFloat("View height", &State->CameraHeight, 0.0f, 250.0f, "%.0f");
		}
		ImGui::End();
		++State->PanelFrames;
	};
	Play.Verify = [=, this]()
	{
		Notes.Add(FString::Printf(TEXT("creator: %d people, panel drawn %d frames, tail swung up to %.0f degrees, soft tissue up to %.1f cm"), State->People.Num(),
			State->PanelFrames, State->LargestTailSwing, State->LargestJiggle));
	};
	Phases.Add(MoveTemp(Play));
}
