// Not a check: `creator` (tools/play-creator.bat) is the live character creator (docs/design/makehuman.md, "Character
// creator"): colonists on the MakeHuman creator bases standing in a row with the graphics up, and a panel that changes
// the picked one as you drag: gender (which base), kind (baseline, imp, orcoid), body sliders, pubic hair and skin
// (morph targets on its mesh, its height and skin colour). They can walk back and forth to show the jiggle and the tail.
#include "SmokeTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
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
	enum class ECreatorKind : int32 { Baseline, Imp, Orcoid };
	const char* const CreatorKindNames[] = {"Baseline", "Imp", "Orcoid"};
	const char* const CreatorPubicNames[] = {"None", "Natural", "Trimmed", "Strip"};

	/** One character in the creator: its gender (which base), kind, slider values, and its colonist. */
	struct FCreatorPerson
	{
		colony::Entity Thing = colony::kNoEntity;
		FSimPawnLooks Looks;      // what the colonist is drawn with (its base)
		bool bFemale = true;
		ECreatorKind Kind = ECreatorKind::Baseline;
		float Height = 1.0f;      // scale of the mesh
		float Sex = 0.0f;         // 0 .. 1 towards the other sex (Masculine on the woman's base, Feminine on the man's)
		float Older = 0.0f;
		float Weight = 0.0f;      // -1 thin .. 1 heavy
		float Muscle = 0.0f;      // -1 soft .. 1 muscular
		float Breasts = 0.0f;     // -1 small .. 1 large (woman's base)
		float ImpEars = 0.0f;
		float ImpFace = 0.0f;
		float OrcFace = 0.0f;
		float OrcBody = 0.0f;
		bool bShaved = false;     // pubic stubble only
		float Horns = 0.0f;
		float Tusks = 0.0f;
		float Tail = 0.0f;
		float Genital = 0.0f;     // -1 smaller .. 1 larger (the man's genitals, the woman's labia)
		float Pigment = 0.5f;     // 0 .. 1: how much darker the areolae and genitals are than the skin
		int32 Pubic = 1;          // 0 none, else grown pubic hair with the four controls below
		float PubicCoverage = 1.0f;
		float PubicDensity = 1.0f;
		float PubicLength = 1.0f;  // 1 natural .. 0.35 trimmed (PubicShort)
		float PubicCurl = 1.0f;    // 1 curly .. 0 straighter (PubicStraight)
		bool bJiggle = true;
		float PubicLow = 0.0f;
		float Skin[3] = {0.91f, 0.80f, 0.75f};
		float Hair[3] = {0.10f, 0.08f, 0.07f};
		bool bOwnPubicColour = false;  // else a little darker than the head hair, as is usual
		float PubicHair[3] = {0.07f, 0.055f, 0.05f};

		FString Label(int32 Index) const
		{
			return FString::Printf(TEXT("%d %s %s"), Index + 1, bFemale ? TEXT("woman") : TEXT("man"), UTF8_TO_TCHAR(CreatorKindNames[static_cast<int32>(Kind)]));
		}
	};

	struct FCreatorState
	{
		TArray<FCreatorPerson> People;
		int32 Selected = 0;
		float CameraYaw = 180.0f; // the free view: looking along this yaw (180: from +X, in front of them) ...
		float CameraPitch = -8.0f; // ... and pitch, round a point by the picked one moved by FocusOffset
		FVector FocusOffset = FVector::ZeroVector;
		FVector2D LastMouse = FVector2D::ZeroVector;
		bool bHadMouse = false;
		bool bUiWantsMouse = false;
		bool bUiWantsKeys = false;
		bool bFaced = false;
		float DemoTime = -1.0f;   // -CreatorDemo: seconds into the self-running demo
		int32 Recorded = 0;
		int32 PanelFrames = 0;
		int32 BaseSwitches = 0;
		float CameraDistance = 420.0f;
		bool bWalk = false;
		float LegTime = 99.0f;
		int32 Leg = 0;
		colony::CellPos Middle = colony::CellPos{48, 48, 0};
		float LargestTailSwing = 0.0f;
		float LargestJiggle = 0.0f;
	};

	const FVector CreatorOrigin(-1695000.0, 4800.0, 0.0);

	void CreatorSetSkin(FCreatorPerson& P, float R, float G, float B)
	{
		P.Skin[0] = R;
		P.Skin[1] = G;
		P.Skin[2] = B;
	}

	/** A kind's look on top of the person's gender: the inhuman parts, a build and a skin colour. Body sliders reset. */
	void CreatorSetKind(FCreatorPerson& P, ECreatorKind Kind)
	{
		P.Kind = Kind;
		P.Sex = P.Older = P.Breasts = P.Genital = 0.0f;
		P.Pigment = 0.5f;
		P.ImpEars = P.ImpFace = P.OrcFace = P.OrcBody = P.Horns = P.Tusks = P.Tail = 0.0f;
		P.Weight = P.bFemale ? -0.2f : 0.0f;
		P.Muscle = 0.0f;
		P.Height = P.bFemale ? 0.94f : 1.02f;
		if (P.bFemale)
		{
			CreatorSetSkin(P, 0.91f, 0.80f, 0.75f);
			P.Hair[0] = 0.10f, P.Hair[1] = 0.08f, P.Hair[2] = 0.07f;  // black
		}
		else
		{
			CreatorSetSkin(P, 0.85f, 0.70f, 0.60f);
			P.Hair[0] = 0.32f, P.Hair[1] = 0.22f, P.Hair[2] = 0.14f;  // brown
		}
		if (Kind == ECreatorKind::Imp)
		{
			P.Height = P.bFemale ? 0.80f : 0.84f;
			P.Weight = -0.6f;
			P.Muscle = 0.6f;
			P.ImpEars = P.ImpFace = P.Horns = P.Tail = 1.0f;
			CreatorSetSkin(P, 0.72f, 0.17f, 0.12f);
		}
		else if (Kind == ECreatorKind::Orcoid)
		{
			P.Height = P.bFemale ? 1.0f : 1.08f;
			P.Weight = 0.25f;
			P.Muscle = 1.0f;
			P.OrcFace = P.Tusks = P.OrcBody = 1.0f;
			P.ImpEars = 0.45f;
			CreatorSetSkin(P, 0.42f, 0.55f, 0.32f);
		}
	}

	/** Puts a person's sliders on its mesh: morphs, height, skin colour. */
	void CreatorApply(const FCreatorPerson& P, ASimPawnActor& Pawn)
	{
		USkeletalMeshComponent* Mesh = Pawn.GetMakeHumanMesh();
		if (!Mesh || Pawn.GetLooks().MakeHuman != P.Looks.MakeHuman)
		{
			return;  // its new base is not on yet
		}
		const auto Set = [Mesh](const TCHAR* Name, float Value) { Mesh->SetMorphTarget(Name, FMath::Clamp(Value, 0.0f, 1.0f), false); };
		Set(P.bFemale ? TEXT("Masculine") : TEXT("Feminine"), P.Sex);
		Set(TEXT("Older"), P.Older);
		Set(TEXT("Heavy"), P.Weight);
		Set(TEXT("Thin"), -P.Weight);
		Set(TEXT("Muscular"), P.Muscle);
		Set(TEXT("Soft"), -P.Muscle);
		Set(TEXT("BreastsLarge"), P.bFemale ? P.Breasts : 0.0f);
		Set(TEXT("BreastsSmall"), P.bFemale ? -P.Breasts : 0.0f);
		Set(TEXT("ImpEars"), P.ImpEars);
		Set(TEXT("ImpFace"), P.ImpFace);
		Set(TEXT("OrcFace"), P.OrcFace);
		Set(TEXT("OrcBody"), P.OrcBody);
		Set(TEXT("NoHorns"), 1.0f - P.Horns);
		Set(TEXT("NoTusks"), 1.0f - P.Tusks);
		Set(TEXT("NoTail"), 1.0f - P.Tail);
		Set(TEXT("PubicShort"), P.bShaved ? 0.0f : (1.0f - P.PubicLength) / 0.65f);
		Set(TEXT("PubicStraight"), P.bShaved ? 0.0f : 1.0f - P.PubicCurl);
		Set(TEXT("PubicStubble"), P.bShaved ? 1.0f : 0.0f);
		Mesh->SetCustomPrimitiveDataFloat(4, P.Pubic == 0 ? 0.0f : P.PubicDensity);
		Mesh->SetCustomPrimitiveDataFloat(5, P.PubicCoverage);
		if (UMakeHumanAnimInstance* Anim = Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()))
		{
			Anim->SetSoftTissueEnabled(P.bJiggle);
		}
		Set(TEXT("GenitalLarge"), P.Genital);
		Set(TEXT("GenitalSmall"), -P.Genital);
		Mesh->SetCustomPrimitiveDataFloat(3, P.Pigment);
		Mesh->SetRelativeScale3D(FVector(P.Height));
		// Hair colours: head hair and brows the picked colour (brows a touch darker), pubic hair a little darker than the head
		// hair unless it has its own. The hair materials' Tint multiplies their texture.
		const auto Linear = [](const float* C) { return FLinearColor::FromSRGBColor(FLinearColor(C[0], C[1], C[2]).ToFColor(false)); };
		const float PubicDefault[3] = {P.Hair[0] * 0.72f, P.Hair[1] * 0.70f, P.Hair[2] * 0.70f};
		const FLinearColor HairColour = Linear(P.Hair);
		const FLinearColor PubicColour = Linear(P.bOwnPubicColour ? P.PubicHair : PubicDefault);
		const TArray<FName> Slots = Mesh->GetMaterialSlotNames();
		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			const FString Slot = Slots[i].ToString();
			const bool bPubic = Slot.Contains(TEXT("Pubic"));
			const bool bBrow = Slot.Contains(TEXT("eyebrow"));
			const bool bHead = !bPubic && !Slot.Contains(TEXT("lash")) && (Slot.Contains(TEXT("hair")) || Slot.Contains(TEXT("long0")) ||
				Slot.Contains(TEXT("short0")) || Slot.Contains(TEXT("ponytail")) || Slot.Contains(TEXT("bob0")) || Slot.Contains(TEXT("braid")));
			if (!bPubic && !bBrow && !bHead)
			{
				continue;
			}
			UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(i));
			if (!Dynamic)
			{
				Dynamic = Mesh->CreateDynamicMaterialInstance(i);
			}
			if (Dynamic)
			{
				Dynamic->SetVectorParameterValue(TEXT("Tint"), bPubic ? PubicColour : (bBrow ? HairColour * 0.8f : HairColour));
			}
		}
		// The panel's colour is as seen (sRGB); the skin material takes it linear, as FSimPawnLooks::Skin is.
		const FLinearColor Skin = FLinearColor::FromSRGBColor(FLinearColor(P.Skin[0], P.Skin[1], P.Skin[2]).ToFColor(false));
		Mesh->SetCustomPrimitiveDataVector3(PawnLooksSkinDataIndex, FVector(Skin.R, Skin.G, Skin.B));
	}

	/** Gender: the person's base (the woman's or the man's); its kind and sliders stay. */
	void CreatorSetGender(FCreatorPerson& P, bool bFemale, USimPawnSubsystem& Pawns)
	{
		P.bFemale = bFemale;
		P.Looks.bFemale = bFemale;
		P.Looks.MakeHuman = bFemale ? TEXT("CreatorFemale") : TEXT("CreatorMale");
		Pawns.SetLooksOverride(P.Thing, P.Looks);
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
		// A baseline woman and man, a woman imp and a man orcoid (gender and kind are chosen apart).
		const int32 Count = FMath::Min(4, Colonists.Num());
		const colony::DefDatabase* Defs = B->GetDefs().get();
		for (int32 i = 0; i < Colonists.Num(); ++i)
		{
			if (i >= Count)
			{
				B->Submit(colony::DestroyThingCommand{Colonists[i]->id});
				continue;
			}
			FCreatorPerson Person;
			Person.Thing = Colonists[i]->id;
			Person.bFemale = i == 0 || i == 2;
			CreatorSetKind(Person, i < 2 ? ECreatorKind::Baseline : (i == 2 ? ECreatorKind::Imp : ECreatorKind::Orcoid));
			Person.Looks = Colonists[i]->looks >= 0 ? USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(Colonists[i]->looks)], Defs) : FSimPawnLooks();
			Person.Looks.Height = 1.0f;
			Person.Looks.Skin = FLinearColor(Person.Skin[0], Person.Skin[1], Person.Skin[2]);
			CreatorSetGender(Person, Person.bFemale, *Pawns);
			State->People.Add(Person);
			for (const colony::Entity Item : Sim->apparel().worn(Person.Thing))
			{
				std::string Error;
				Sim->apparel().takeOff(Person.Thing, Item, Error);
				B->Submit(colony::DestroyThingCommand{Item});
			}
			B->Submit(colony::DraftCommand{Person.Thing, true});
			B->Submit(colony::MoveOrderCommand{Person.Thing, colony::CellPos{Middle.x - 1, Middle.y + 6 - 3 + i * 2, 0}});
		}
		B->StepOnce();
		B->SetSpeed(1);
		// For pictures: -CreatorSelect=<0-2>, -CreatorWalk, -CreatorYaw=<degrees>, -CreatorDistance=<cm>.
		FParse::Value(FCommandLine::Get(), TEXT("CreatorSelect="), State->Selected);
		State->bWalk = FParse::Param(FCommandLine::Get(), TEXT("CreatorWalk"));
		float Yaw = 0.0f;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorYaw="), Yaw))
		{
			State->CameraYaw += Yaw;
		}
		FParse::Value(FCommandLine::Get(), TEXT("CreatorFocusZ="), State->FocusOffset.Z);
		FParse::Value(FCommandLine::Get(), TEXT("CreatorPitch="), State->CameraPitch);
		float Genital = 0.0f;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorGenital="), Genital))
		{
			for (FCreatorPerson& Person : State->People)
			{
				Person.Genital = Genital;
			}
		}
		int32 Pubic = -1;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorPubic="), Pubic))
		{
			for (FCreatorPerson& Person : State->People)
			{
				Person.Pubic = Pubic;
			}
		}
		FParse::Value(FCommandLine::Get(), TEXT("CreatorDistance="), State->CameraDistance);
		float Pigment = -1.0f;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorPigment="), Pigment))
		{
			for (FCreatorPerson& Person : State->People)
			{
				Person.Pigment = Pigment;
			}
		}
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
				B->Submit(colony::MoveOrderCommand{State->People[i].Thing, colony::CellPos{State->Middle.x, State->Middle.y + 6 - 3 + i * 2, 0}});
			}
		}
		// Walking: back and forth along X, side by side, so the jiggle and the tail show.
		if (State->bWalk && State->LegTime > 4.0f)
		{
			State->LegTime = 0.0f;
			const int32 X = State->Leg % 2 == 0 ? -8 : 2;
			for (int32 i = 0; i < State->People.Num(); ++i)
			{
				B->Submit(colony::MoveOrderCommand{State->People[i].Thing, colony::CellPos{State->Middle.x + X, State->Middle.y + 6 - 3 + i * 2, 0}});
			}
			++State->Leg;
		}
		// -CreatorDemo: the sliders move by themselves on the woman (body, then into an imp), then they all walk; with
		// -CreatorRecord=<seconds> every frame is saved at a fixed 30 a second (creator/rec_0000.png ...).
		if (FParse::Param(FCommandLine::Get(), TEXT("CreatorDemo")) && PhaseTime > 4.5f && State->People.Num() >= 2)
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
			// Body sliders on the woman, then her gender and kind changed: man, imp, orcoid, back to a woman orcoid; then all walk.
			State->Selected = T < 21.0f ? 0 : (T < 25.0f ? FMath::Min(2, State->People.Num() - 1) : FMath::Min(3, State->People.Num() - 1));
			State->CameraDistance = T < 21.0f ? 260.0f : 380.0f;
			State->CameraYaw = T < 21.0f ? 180.0f : 255.0f;
			if (T < 9.0f)
			{
				W.Weight = -0.2f + Wave(0.0f, 3.0f);
				W.Muscle = Wave(3.0f, 6.0f);
				W.Breasts = Wave(6.0f, 9.0f);
			}
			const int32 Step = T < 11.0f ? 0 : (T < 13.0f ? 1 : (T < 15.0f ? 2 : (T < 17.0f ? 3 : (T < 19.0f ? 4 : 5))));
			static const bool StepFemale[] = {true, false, false, false, true, true};
			static const ECreatorKind StepKind[] = {ECreatorKind::Baseline, ECreatorKind::Baseline, ECreatorKind::Imp, ECreatorKind::Orcoid,
				ECreatorKind::Orcoid, ECreatorKind::Orcoid};
			if (W.bFemale != StepFemale[Step])
			{
				CreatorSetGender(W, StepFemale[Step], *Pawns);
				CreatorSetKind(W, StepKind[Step]);
			}
			if (W.Kind != StepKind[Step])
			{
				CreatorSetKind(W, StepKind[Step]);
			}
			if (T > 20.0f && !State->bWalk)
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
		// The free view round the picked one: middle mouse turns it, the wheel moves closer or further, right mouse drags the
		// point it looks at (as do WASD, Q and E), not while the mouse or keys are on the panel.
		APlayerController* ViewPC = GetWorld()->GetFirstPlayerController();
		const float Dt = GetWorld()->GetDeltaSeconds();
		if (ViewPC)
		{
			float MouseX = 0.0f;
			float MouseY = 0.0f;
			const bool bMouse = ViewPC->GetMousePosition(MouseX, MouseY);
			const FVector2D Mouse(MouseX, MouseY);
			const FVector2D Delta = bMouse && State->bHadMouse ? Mouse - State->LastMouse : FVector2D::ZeroVector;
			State->LastMouse = Mouse;
			State->bHadMouse = bMouse;
			const FRotator Look(0.0, State->CameraYaw, 0.0);
			if (!State->bUiWantsMouse)
			{
				if (ViewPC->IsInputKeyDown(EKeys::MiddleMouseButton))
				{
					State->CameraYaw += Delta.X * 0.3f;
					State->CameraPitch = FMath::Clamp(State->CameraPitch - Delta.Y * 0.3f, -85.0f, 85.0f);
				}
				if (ViewPC->IsInputKeyDown(EKeys::RightMouseButton))
				{
					const FVector Right = FRotationMatrix(Look).GetUnitAxis(EAxis::Y);
					State->FocusOffset += (-Right * Delta.X + FVector::UpVector * Delta.Y) * State->CameraDistance * 0.0015;
				}
				if (ViewPC->WasInputKeyJustPressed(EKeys::MouseScrollUp))
				{
					State->CameraDistance *= 0.87f;
				}
				if (ViewPC->WasInputKeyJustPressed(EKeys::MouseScrollDown))
				{
					State->CameraDistance /= 0.87f;
				}
				State->CameraDistance = FMath::Clamp(State->CameraDistance, 40.0f, 2500.0f);
			}
			if (!State->bUiWantsKeys)
			{
				const FVector Forward = Look.Vector();
				const FVector Right = FRotationMatrix(Look).GetUnitAxis(EAxis::Y);
				FVector Move = FVector::ZeroVector;
				Move += ViewPC->IsInputKeyDown(EKeys::W) ? Forward : FVector::ZeroVector;
				Move -= ViewPC->IsInputKeyDown(EKeys::S) ? Forward : FVector::ZeroVector;
				Move += ViewPC->IsInputKeyDown(EKeys::D) ? Right : FVector::ZeroVector;
				Move -= ViewPC->IsInputKeyDown(EKeys::A) ? Right : FVector::ZeroVector;
				Move += ViewPC->IsInputKeyDown(EKeys::E) ? FVector::UpVector : FVector::ZeroVector;
				Move -= ViewPC->IsInputKeyDown(EKeys::Q) ? FVector::UpVector : FVector::ZeroVector;
				State->FocusOffset += Move * FMath::Max(150.0f, State->CameraDistance) * Dt;
			}
		}
		FCreatorPerson& Sel = State->People[FMath::Clamp(State->Selected, 0, State->People.Num() - 1)];
		if (const ASimPawnActor* Pawn = Pawns->FindPawn(Sel.Thing))
		{
			const FVector Focus = Pawn->GetActorLocation() + FVector(0.0, 0.0, 100.0 * Sel.Height) + State->FocusOffset;
			const FRotator View(State->CameraPitch, State->CameraYaw, 0.0);
			ViewFrom(Focus - View.Vector() * State->CameraDistance, View);
		}
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			PC->bShowMouseCursor = true;
		}

		// The panel (-CreatorNoPanel: none, for pictures).
		if (FParse::Param(FCommandLine::Get(), TEXT("CreatorNoPanel")))
		{
			return;
		}
		const ImGui::FScopedContext ScopedContext;
		if (!ScopedContext)
		{
			return;
		}
		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
		ImGui::GetStyle().FontScaleMain = 1.8f * Scale;  // half as big again as the other panels (sneed)
		State->bUiWantsMouse = ImGui::GetIO().WantCaptureMouse;
		State->bUiWantsKeys = ImGui::GetIO().WantCaptureKeyboard;
		ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + 16.0f * Scale, Viewport->Pos.y + 16.0f * Scale), ImGuiCond_Once);
		ImGui::SetNextWindowBgAlpha(0.88f);
		// A look of its own: warm dark panel, rounded corners, copper accents (popped after End).
		const ImVec4 Copper(0.86f, 0.52f, 0.30f, 1.0f);
		const ImVec4 CopperDim(0.55f, 0.32f, 0.18f, 1.0f);
		const ImVec4 Panel(0.09f, 0.08f, 0.08f, 1.0f);
		const ImVec4 Field(0.20f, 0.16f, 0.14f, 1.0f);
		const ImVec4 FieldHover(0.30f, 0.23f, 0.19f, 1.0f);
		const ImGuiStyleVar Vars[] = {ImGuiStyleVar_WindowRounding, ImGuiStyleVar_FrameRounding, ImGuiStyleVar_GrabRounding, ImGuiStyleVar_TabRounding};
		for (const ImGuiStyleVar Var : Vars)
		{
			ImGui::PushStyleVar(Var, 7.0f * Scale);
		}
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * Scale, 12.0f * Scale));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * Scale, 5.0f * Scale));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(9.0f * Scale, 7.0f * Scale));
		ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 14.0f * Scale);
		const TPair<ImGuiCol, ImVec4> Colours[] = {
			{ImGuiCol_WindowBg, Panel}, {ImGuiCol_TitleBg, CopperDim}, {ImGuiCol_TitleBgActive, CopperDim}, {ImGuiCol_TitleBgCollapsed, CopperDim},
			{ImGuiCol_FrameBg, Field}, {ImGuiCol_FrameBgHovered, FieldHover}, {ImGuiCol_FrameBgActive, FieldHover},
			{ImGuiCol_SliderGrab, Copper}, {ImGuiCol_SliderGrabActive, ImVec4(1.0f, 0.68f, 0.45f, 1.0f)}, {ImGuiCol_CheckMark, Copper},
			{ImGuiCol_Button, Field}, {ImGuiCol_ButtonHovered, CopperDim}, {ImGuiCol_ButtonActive, Copper},
			{ImGuiCol_Tab, Field}, {ImGuiCol_TabHovered, CopperDim}, {ImGuiCol_TabSelected, CopperDim}, {ImGuiCol_TabSelectedOverline, Copper},
			{ImGuiCol_Header, Field}, {ImGuiCol_HeaderHovered, FieldHover}, {ImGuiCol_Separator, CopperDim}, {ImGuiCol_Border, CopperDim},
			{ImGuiCol_Text, ImVec4(0.95f, 0.91f, 0.86f, 1.0f)}, {ImGuiCol_TextDisabled, ImVec4(0.62f, 0.56f, 0.50f, 1.0f)}};
		for (const TPair<ImGuiCol, ImVec4>& Colour : Colours)
		{
			ImGui::PushStyleColor(Colour.Key, Colour.Value);
		}
		if (ImGui::Begin("CHARACTER CREATOR", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			for (int32 i = 0; i < State->People.Num(); ++i)
			{
				if (i > 0)
				{
					ImGui::SameLine();
				}
				if (ImGui::RadioButton(TCHAR_TO_UTF8(*FString::Printf(TEXT("%d##person"), i + 1)), State->Selected == i))
				{
					State->Selected = i;
				}
			}
			FCreatorPerson& P = State->People[FMath::Clamp(State->Selected, 0, State->People.Num() - 1)];
			ImGui::SameLine();
			ImGui::TextUnformatted(TCHAR_TO_UTF8(*P.Label(State->Selected)));
			if (ImGui::BeginTabBar("##creator"))
			{
				if (ImGui::BeginTabItem("Gender"))
				{
					if (ImGui::RadioButton("Woman", P.bFemale) && !P.bFemale)
					{
						CreatorSetGender(P, true, *Pawns);
						++State->BaseSwitches;
					}
					ImGui::SameLine();
					if (ImGui::RadioButton("Man", !P.bFemale) && P.bFemale)
					{
						CreatorSetGender(P, false, *Pawns);
						++State->BaseSwitches;
					}
					ImGui::SliderFloat(P.bFemale ? "More masculine" : "More feminine", &P.Sex, 0.0f, 1.0f, "%.2f");
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Kind"))
				{
					for (int32 k = 0; k < 3; ++k)
					{
						if (k > 0)
						{
							ImGui::SameLine();
						}
						if (ImGui::RadioButton(CreatorKindNames[k], static_cast<int32>(P.Kind) == k))
						{
							CreatorSetKind(P, static_cast<ECreatorKind>(k));
						}
					}
					ImGui::SeparatorText("Features");
					ImGui::SliderFloat("Pointed ears", &P.ImpEars, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Imp face", &P.ImpFace, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Orc face", &P.OrcFace, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Orc build", &P.OrcBody, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Horns", &P.Horns, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Tusks", &P.Tusks, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Tail", &P.Tail, 0.0f, 1.0f, "%.2f");
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Body"))
				{
					ImGui::SliderFloat("Height", &P.Height, 0.75f, 1.15f, "%.2f");
					ImGui::SliderFloat("Older", &P.Older, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Thin / heavy", &P.Weight, -1.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Soft / muscular", &P.Muscle, -1.0f, 1.0f, "%.2f");
					if (P.bFemale)
					{
						ImGui::SliderFloat("Breasts", &P.Breasts, -1.0f, 1.0f, "%.2f");
					}
					ImGui::SliderFloat(P.bFemale ? "Labia" : "Genitals", &P.Genital, -1.0f, 1.0f, "%.2f");
					ImGui::Checkbox("Jiggle (off: keep still)", &P.bJiggle);
					if (ImGui::Button("Random body"))
					{
						FRandomStream R(FPlatformTime::Cycles());
						P.Height = R.FRandRange(0.85f, 1.1f);
						P.Sex = R.FRandRange(0.0f, 0.3f);
						P.Older = R.FRand() < 0.3f ? R.FRandRange(0.2f, 1.0f) : 0.0f;
						P.Weight = R.FRandRange(-0.8f, 0.8f);
						P.Muscle = R.FRandRange(-0.6f, 0.8f);
						P.Breasts = R.FRandRange(-0.8f, 0.8f);
					}
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Hair and skin"))
				{
					ImGui::SeparatorText("Hair");
					ImGui::ColorEdit3("Hair colour", P.Hair);
					ImGui::SeparatorText("Pubic hair");
					bool bPubic = P.Pubic != 0;
					if (ImGui::Checkbox("Pubic hair", &bPubic))
					{
						P.Pubic = bPubic ? 1 : 0;
					}
					ImGui::SameLine();
					ImGui::Checkbox("Shaved (stubble)", &P.bShaved);
					ImGui::SliderFloat("Coverage", &P.PubicCoverage, 0.3f, 1.0f, "%.2f");
					ImGui::SliderFloat("Density", &P.PubicDensity, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Length (trimmed .. natural)", &P.PubicLength, 0.35f, 1.0f, "%.2f");
					ImGui::SliderFloat("Straight .. curly", &P.PubicCurl, 0.0f, 1.0f, "%.2f");
					ImGui::Checkbox("Own pubic hair colour", &P.bOwnPubicColour);
					if (P.bOwnPubicColour)
					{
						ImGui::ColorEdit3("Pubic hair colour", P.PubicHair);
					}
					else
					{
						ImGui::TextUnformatted("(a little darker than the hair)");
					}
					ImGui::SeparatorText("Skin");
					ImGui::ColorEdit3("Skin", P.Skin);
					ImGui::SliderFloat("Areola and genital pigment", &P.Pigment, 0.0f, 1.0f, "%.2f");
					if (ImGui::Button("Random skin tone"))
					{
						const float Tone = FRandomStream(FPlatformTime::Cycles()).FRand();
						CreatorSetSkin(P, FMath::Lerp(0.95f, 0.36f, Tone), FMath::Lerp(0.84f, 0.22f, Tone), FMath::Lerp(0.78f, 0.15f, Tone));
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			ImGui::Separator();
			ImGui::Checkbox("Walk back and forth", &State->bWalk);
			ImGui::SeparatorText("View");
			ImGui::TextDisabled("middle mouse turn, wheel zoom, right mouse or WASD/QE move");
			if (ImGui::Button("Reset view"))
			{
				State->CameraYaw = 180.0f;
				State->CameraPitch = -8.0f;
				State->CameraDistance = 420.0f;
				State->FocusOffset = FVector::ZeroVector;
			}
		}
		ImGui::End();
		ImGui::PopStyleColor(UE_ARRAY_COUNT(Colours));
		ImGui::PopStyleVar(UE_ARRAY_COUNT(Vars) + 4);
		++State->PanelFrames;
	};
	Play.Verify = [=, this]()
	{
		Notes.Add(FString::Printf(TEXT("creator: %d people, panel drawn %d frames, tail swung up to %.0f degrees, soft tissue up to %.1f cm"), State->People.Num(),
			State->PanelFrames, State->LargestTailSwing, State->LargestJiggle));
		for (int32 i = 0; i < State->People.Num(); ++i)
		{
			Notes.Add(FString::Printf(TEXT("creator: %s"), *State->People[i].Label(i)));
		}
	};
	Phases.Add(MoveTemp(Play));
}
