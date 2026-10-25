// Smoke test `looks` (task A3): seeded pawn looks on MetaHumans, in the game, offscreen.
#include "SmokeTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"

#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "PawnActor.h"
#include "PawnLooks.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

#include <string>

namespace
{
	const FVector LooksSmokeOrigin(-300000.0, 0.0, 0.0);
	// Pawns spawned beside the Crashlanded colonists, and every this-many-th pawn made Emberkin.
	constexpr int32 LooksSmokeExtraPawns = 14;
	constexpr int32 LooksSmokeEmberEvery = 4;

	struct FLooksSmokeState
	{
		TArray<colony::Entity> Ember;
		int64 Frames = 0;
		int32 Errors = 0;
		FString FirstError;
		double HornPixelsMin = 1.0e9;
		double HornPixelsSum = 0.0;
		int64 HornSamples = 0;
		double HeadPixelsSum = 0.0;
		int64 HeadSamples = 0;

		void Reset()
		{
			Frames = 0;
			Errors = 0;
			FirstError.Reset();
			HornPixelsMin = 1.0e9;
			HornPixelsSum = HeadPixelsSum = 0.0;
			HornSamples = HeadSamples = 0;
		}

		void Error(const FString& Message)
		{
			if (Errors++ == 0)
			{
				FirstError = Message;
			}
		}
	};

	void LooksSmokeMetaHumans(bool bOn)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Pawns.MetaHumans")))
		{
			Var->Set(bOn ? 1 : 0, ECVF_SetByCode);
		}
	}

	/** How much of a colour is red, whatever its brightness. */
	float LooksSmokeRedness(const FLinearColor& C)
	{
		return C.R / FMath::Max(C.R + C.G + C.B, 1.0e-4f);
	}

	/** Pixels between two world points on screen, or -1 if either is off screen. */
	double LooksSmokePixels(const APlayerController* PC, const FVector& A, const FVector& B)
	{
		FVector2D SA, SB;
		if (!PC || !PC->ProjectWorldLocationToScreen(A, SA) || !PC->ProjectWorldLocationToScreen(B, SB))
		{
			return -1.0;
		}
		return FVector2D::Distance(SA, SB);
	}
}

void USmokeTestSubsystem::BuildLooksPhases()
{
	const TSharedRef<FLooksSmokeState> State = MakeShared<FLooksSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto PawnsOf = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };

	// Every frame: each pawn shows the looks the sim gives it: height, body shape morphs on body
	// and clothes, our skin material with its colour, and horns exactly on the Emberkin.
	const auto CheckFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!B || !B->GetSim() || !Pawns || PhaseTime < 2.0f)
		{
			return;
		}
		++State->Frames;
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		const APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category != colony::ThingCategory::Pawn)
			{
				continue;
			}
			ASimPawnActor* Pawn = Pawns->FindPawn(View.id);
			if (!Pawn || View.looks < 0 || View.looks >= static_cast<int32>(Snapshot.looks.size()))
			{
				State->Error(TEXT("a pawn has no actor or no looks in the snapshot"));
				continue;
			}
			const FSimPawnLooks Wanted = USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(View.looks)], B->GetDefs().get());
			if (!Pawn->HasLooks() || !(Pawn->GetLooks() == Wanted))
			{
				State->Error(TEXT("a pawn's actor doesn't show its sim looks"));
				continue;
			}
			if (!FMath::IsNearlyEqual(Pawn->GetBody()->GetRelativeScale3D().Z, Wanted.Height, 1.0e-3))
			{
				State->Error(FString::Printf(TEXT("a pawn is drawn at scale %.3f, not its height %.3f"), Pawn->GetBody()->GetRelativeScale3D().Z, Wanted.Height));
			}
			if (Pawn->GetShapedMeshes().Num() != 4)
			{
				State->Error(FString::Printf(TEXT("a MetaHuman has %d meshes with body shapes, not 4 (body, top, trousers, shoes)"), Pawn->GetShapedMeshes().Num()));
			}
			for (const USkeletalMeshComponent* Mesh : Pawn->GetShapedMeshes())
			{
				// A material not marked for morph targets draws as the default material.
				for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
				{
					const UMaterialInterface* Material = Mesh->GetMaterial(Slot);
					const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
					if (Base && !Base->bUsedWithMorphTargets)
					{
						State->Error(FString::Printf(TEXT("%s: %s is not marked for morph targets"), *Mesh->GetName(), *Material->GetName()));
					}
				}
				for (const TPair<FName, float>& Morph : Wanted.MorphWeights())
				{
					if (!FMath::IsNearlyEqual(Mesh->GetMorphTarget(Morph.Key), Morph.Value, 1.0e-4f))
					{
						State->Error(FString::Printf(TEXT("%s: morph %s is %.3f, wanted %.3f"), *Mesh->GetName(), *Morph.Key.ToString(),
							Mesh->GetMorphTarget(Morph.Key), Morph.Value));
					}
				}
			}
			if (Pawn->GetSkinMeshes().Num() != 2)
			{
				State->Error(FString::Printf(TEXT("a MetaHuman has %d meshes with our skin, not 2 (body, face)"), Pawn->GetSkinMeshes().Num()));
			}
			for (const USkeletalMeshComponent* Mesh : Pawn->GetSkinMeshes())
			{
				bool bOurs = false;
				for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
				{
					bOurs = bOurs || (Mesh->GetMaterial(Slot) && Mesh->GetMaterial(Slot)->GetPathName().StartsWith(PawnLooksFolder));
				}
				const TArray<float>& Data = Mesh->GetCustomPrimitiveData().Data;
				const bool bColour = Data.Num() >= 3 && FMath::IsNearlyEqual(Data[0], Wanted.Skin.R, 1.0e-4f)
					&& FMath::IsNearlyEqual(Data[1], Wanted.Skin.G, 1.0e-4f) && FMath::IsNearlyEqual(Data[2], Wanted.Skin.B, 1.0e-4f);
				if (!bOurs || !bColour)
				{
					State->Error(FString::Printf(TEXT("%s: skin material ours %d, colour set %d"), *Mesh->GetName(), bOurs, bColour));
				}
			}
			const bool bEmber = State->Ember.Contains(View.id);
			const int32 VisibleHorns = Pawn->GetHorns().FilterByPredicate([](const UStaticMeshComponent* H) { return H && H->IsVisible() && H->GetStaticMesh(); }).Num();
			if (VisibleHorns != (Wanted.HornLength > 0.0f ? 2 : 0) || (Wanted.HornLength > 0.0f) != bEmber)
			{
				State->Error(FString::Printf(TEXT("a pawn (Emberkin %d) has %d horns"), bEmber, VisibleHorns));
			}
			// How big horns and heads are on screen.
			for (const UStaticMeshComponent* Horn : Pawn->GetHorns())
			{
				const FTransform T = Horn->GetComponentTransform();
				const double Pixels = LooksSmokePixels(PC, T.TransformPosition(FVector::ZeroVector), T.TransformPosition(FVector(0.0, -0.35, 1.0)));
				if (Pixels >= 0.0)
				{
					State->HornPixelsMin = FMath::Min(State->HornPixelsMin, Pixels);
					State->HornPixelsSum += Pixels;
					++State->HornSamples;
				}
			}
			if (USkeletalMeshComponent* Head = Pawn->GetBody())
			{
				const FVector Top = Head->GetSocketLocation(TEXT("head"));
				const double Pixels = LooksSmokePixels(PC, Top - FVector(0.0, 0.0, 12.0), Top + FVector(0.0, 0.0, 12.0));
				if (Pixels >= 0.0)
				{
					State->HeadPixelsSum += Pixels;
					++State->HeadSamples;
				}
			}
		}
	};

	const auto VerifyFrames = [=, this](const TCHAR* Phase)
	{
		Notes.Add(FString::Printf(TEXT("%s: %lld frames checked; horns %.1f px long on average (shortest %.1f), heads %.1f px tall"), Phase,
			State->Frames, State->HornSamples ? State->HornPixelsSum / State->HornSamples : 0.0,
			State->HornSamples ? State->HornPixelsMin : 0.0, State->HeadSamples ? State->HeadPixelsSum / State->HeadSamples : 0.0));
		if (State->Frames < 10)
		{
			Fail(FString::Printf(TEXT("%s: too few frames checked (%lld)"), Phase, State->Frames));
		}
		if (State->Errors > 0)
		{
			Fail(FString::Printf(TEXT("%s: %d problems; the first: %s"), Phase, State->Errors, *State->FirstError));
		}
	};

	// 1. The Crashlanded colonists and more pawns as MetaHumans, some of them Emberkin, close up.
	FPhase Close;
	Close.Name = TEXT("looks_close_up");
	Close.WarmupSeconds = 4.0f;
	Close.MeasureSeconds = 3.0f;
	Close.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !PawnsOf())
		{
			Fail(TEXT("USimSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		LooksSmokeMetaHumans(true);
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 96;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(LooksSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		for (const std::string& Problem : B->GetSim()->setupErrors())
		{
			Fail(FString::Printf(TEXT("Sim setup: %s"), UTF8_TO_TCHAR(Problem.c_str())));
		}
		const colony::CellPos Middle = B->GetSim()->stockpileCell().value_or(colony::CellPos{48, 48, 0});
		for (int32 i = 0; i < LooksSmokeExtraPawns; ++i)
		{
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{Middle.x + 3, Middle.y - 7 + i, 0}});
		}
		B->StepOnce();
		int32 Index = 0;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && Index++ % LooksSmokeEmberEvery == 1)
			{
				B->Submit(colony::SetLineageCommand{View.id, "Emberkin"});
				State->Ember.Add(View.id);
			}
		}
		B->StepOnce();
		B->SetSpeed(0);
		BuildTerrainMesh(B->GetSim()->map(), FPlatformTime::Seconds());
		// Facing the row of spawned pawns (they face +X, the way a pawn faces before it has walked).
		const FVector Target = B->SimToWorld(Middle.x + 3.5f, Middle.y - 3.0f, 0) + FVector(0.0, 0.0, 110.0);
		const FVector From = Target + FVector(520.0, -60.0, 60.0);
		ViewFrom(From, (Target - From).Rotation());
		State->Reset();
	};
	Close.OnFrame = CheckFrame;
	Close.Verify = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		VerifyFrames(TEXT("looks_close_up"));
		// The looks vary: genders, body shapes, heights and skin colours, and Emberkin are red.
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		int32 Women = 0;
		int32 Men = 0;
		TSet<int32> Bodies;
		float MinHeight = 9.0f;
		float MaxHeight = 0.0f;
		float MinWeight = 9.0f;
		float MaxWeight = -9.0f;
		float EmberRed = 1.0f;
		float OtherRed = -1.0f;
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.looks < 0)
			{
				continue;
			}
			const colony::PawnLooksView& L = Snapshot.looks[static_cast<size_t>(View.looks)];
			const FSimPawnLooks Looks = USimPawnSubsystem::MakeLooks(L, B->GetDefs().get());
			(L.female ? Women : Men) += 1;
			Bodies.Add(static_cast<int32>(L.body));
			MinHeight = FMath::Min(MinHeight, L.height);
			MaxHeight = FMath::Max(MaxHeight, L.height);
			MinWeight = FMath::Min(MinWeight, L.weight);
			MaxWeight = FMath::Max(MaxWeight, L.weight);
			if (State->Ember.Contains(View.id))
			{
				EmberRed = FMath::Min(EmberRed, LooksSmokeRedness(Looks.Skin));
			}
			else
			{
				OtherRed = FMath::Max(OtherRed, LooksSmokeRedness(Looks.Skin));
			}
		}
		Notes.Add(FString::Printf(TEXT("looks_close_up: %d men, %d women, %d body types, heights %.2f to %.2f, weights %.2f to %.2f, %d Emberkin"),
			Men, Women, Bodies.Num(), MinHeight, MaxHeight, MinWeight, MaxWeight, State->Ember.Num()));
		if (Men == 0 || Women == 0 || Bodies.Num() < 3 || MaxHeight - MinHeight < 0.06f || MaxWeight - MinWeight < 0.8f)
		{
			Fail(TEXT("looks_close_up: the pawns' looks hardly vary"));
		}
		if (State->Ember.Num() < 3 || EmberRed <= OtherRed)
		{
			Fail(FString::Printf(TEXT("looks_close_up: Emberkin skin is not the reddest (least red Emberkin %.3f, reddest other %.3f)"), EmberRed, OtherRed));
		}
	};
	Phases.Add(MoveTemp(Close));

	// 2. The overseer camera at its usual distance: how big Emberkin horns and heads are on screen.
	FPhase Overseer;
	Overseer.Name = TEXT("looks_overseer_view");
	Overseer.WarmupSeconds = 3.0f;
	Overseer.MeasureSeconds = 2.0f;
	Overseer.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		UOverseerSubsystem* O = GetWorld()->GetSubsystem<UOverseerSubsystem>();
		AOverseerPawn* Body = O ? O->EnterOverseerMode() : nullptr;
		if (!B || !B->GetSim() || !Body)
		{
			Fail(TEXT("The overseer camera did not start"));
			return;
		}
		const colony::CellPos Middle = B->GetSim()->stockpileCell().value_or(colony::CellPos{48, 48, 0});
		Body->FocusOn(B->SimToWorld(Middle.x + 0.5f, Middle.y - 3.0f, 0));
		Body->SnapCamera();
		State->Reset();
	};
	Overseer.OnFrame = CheckFrame;
	Overseer.Verify = [=, this]()
	{
		VerifyFrames(TEXT("looks_overseer_view"));
		if (State->HornSamples == 0)
		{
			Fail(TEXT("looks_overseer_view: no horns on screen"));
		}
	};
	Phases.Add(MoveTemp(Overseer));

	// 3. Three wheel notches closer.
	FPhase Nearer;
	Nearer.Name = TEXT("looks_overseer_zoomed");
	Nearer.WarmupSeconds = 2.0f;
	Nearer.MeasureSeconds = 2.0f;
	Nearer.Setup = [=, this]()
	{
		if (AOverseerPawn* Body = Cast<AOverseerPawn>(UGameplayStatics::GetPlayerPawn(GetWorld(), 0)))
		{
			Body->Zoom(3.0f);
		}
		State->Reset();
	};
	Nearer.OnFrame = CheckFrame;
	Nearer.Verify = [=, this]() { VerifyFrames(TEXT("looks_overseer_zoomed")); };
	Phases.Add(MoveTemp(Nearer));

	// 4. Lineages change: the Emberkin go back to baseline (horns gone, skin back) and one baseline
	// pawn becomes Emberkin.
	FPhase Change;
	Change.Name = TEXT("looks_lineage_change");
	Change.WarmupSeconds = 2.5f;
	Change.MeasureSeconds = 1.0f;
	Change.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		const TArray<colony::Entity> Was = State->Ember;
		State->Ember.Reset();
		for (const colony::Entity Pawn : Was)
		{
			B->Submit(colony::SetLineageCommand{Pawn, "Baseline"});
		}
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && !Was.Contains(View.id))
			{
				B->Submit(colony::SetLineageCommand{View.id, "Emberkin"});
				State->Ember.Add(View.id);
				break;
			}
		}
		B->StepOnce();
		State->Reset();
	};
	Change.OnFrame = CheckFrame;
	Change.Verify = [=, this]() { VerifyFrames(TEXT("looks_lineage_change")); };
	Phases.Add(MoveTemp(Change));

	// 5. What it costs: 100 shaped MetaHumans walking about, from the overseer camera.
	FPhase Crowd;
	Crowd.Name = TEXT("looks_crowd_100");
	Crowd.WarmupSeconds = 4.0f;
	Crowd.MeasureSeconds = 6.0f;
	Crowd.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		const colony::CellPos Middle = B->GetSim()->stockpileCell().value_or(colony::CellPos{48, 48, 0});
		int32 Pawns = 0;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			Pawns += View.category == colony::ThingCategory::Pawn ? 1 : 0;
		}
		for (int32 i = 0; Pawns + i < 100; ++i)
		{
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{Middle.x - 10 + i % 20, Middle.y + 4 + i / 20 * 2, 0}});
		}
		B->SetSpeed(1);
		State->Reset();
	};
	Crowd.OnFrame = CheckFrame;
	Crowd.Verify = [=, this]() { VerifyFrames(TEXT("looks_crowd_100")); };
	Phases.Add(MoveTemp(Crowd));
}
