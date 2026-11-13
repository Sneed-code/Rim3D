// Smoke test `bodies` (character creator step 1): the engine's whole MetaHuman body under the clothes, in all of its body
// types, and a showcase colonist (a pale, petite woman) from the front, side and back, in the game, offscreen.
#include "SmokeTest.h"

#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/PointLight.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "PawnActor.h"
#include "PawnFaces.h"
#include "PawnLooks.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <string>
#include <vector>

namespace
{
	const FVector BodiesSmokeOrigin(-1500000.0, 0.0, 0.0);

	// The row: the corners of each gender's 3 x 3 grid of body types and its middle, as {female, stature, build, skin sRGB}.
	struct FBodiesSmokeType
	{
		bool bFemale;
		float Stature;
		float Weight;
		FColor Skin;
		const TCHAR* Label;
	};
	const FBodiesSmokeType BodiesSmokeRow[] = {
		{true, -1.0f, -1.0f, FColor(214, 178, 158), TEXT("woman, short, thin")},
		{true, 0.0f, 0.0f, FColor(158, 112, 86), TEXT("woman, medium, normal")},
		{true, 1.0f, 1.0f, FColor(92, 60, 44), TEXT("woman, tall, heavy")},
		{true, -1.0f, 1.0f, FColor(190, 140, 112), TEXT("woman, short, heavy")},
		{false, -1.0f, -1.0f, FColor(200, 160, 136), TEXT("man, short, thin")},
		{false, 0.0f, 0.0f, FColor(132, 90, 66), TEXT("man, medium, normal (Kellan's own body)")},
		{false, 1.0f, 1.0f, FColor(70, 46, 34), TEXT("man, tall, heavy")},
		{false, 0.0f, 1.0f, FColor(176, 128, 100), TEXT("man, medium, heavy")},
	};
	constexpr int32 BodiesSmokeRowCount = UE_ARRAY_COUNT(BodiesSmokeRow);

	struct FBodiesSmokeState
	{
		TArray<colony::Entity> Row;  // left to right, then the showcase woman last
		colony::Entity Showcase = colony::kNoEntity;
		colony::CellPos Middle = colony::CellPos{48, 48, 0};
		TWeakObjectPtr<APointLight> Light;
		int32 Errors = 0;
		FString FirstError;
		int64 Frames = 0;

		void Reset()
		{
			Errors = 0;
			FirstError.Reset();
			Frames = 0;
		}

		void Error(const FString& Message)
		{
			if (Errors++ == 0)
			{
				FirstError = Message;
			}
		}
	};

	void BodiesSmokeSet(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByCode);
		}
	}

	// Height for a stature: the gender's LooksRules range (heightFemale 0.88 to 0.97, heightMale 0.95 to 1.05).
	float BodiesSmokeHeight(bool bFemale, float Stature)
	{
		const float Low = bFemale ? 0.88f : 0.95f;
		const float High = bFemale ? 0.97f : 1.05f;
		return (Low + High) * 0.5f + Stature * (High - Low) * 0.5f;
	}
}

void USmokeTestSubsystem::BuildBodiesPhases()
{
	const TSharedRef<FBodiesSmokeState> State = MakeShared<FBodiesSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto PawnsOf = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };

	// Every frame: each pawn wears the whole body, with no skin layer, in its body type's morphs, Kellan's clothes away.
	const auto CheckFrame = [=, this]()
	{
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!Pawns || PhaseTime < 2.0f)
		{
			return;
		}
		++State->Frames;
		for (const colony::Entity Thing : State->Row)
		{
			const ASimPawnActor* Pawn = Pawns->FindPawn(Thing);
			const USkeletalMeshComponent* Skin = Pawn ? Pawn->GetSkinBody() : nullptr;
			if (!Skin || !Pawn->HasLooks())
			{
				State->Error(TEXT("a pawn has no MetaHuman body or no looks"));
				continue;
			}
			if (!Pawn->HasFullBody() || !Skin->GetSkeletalMeshAsset() || !Skin->GetSkeletalMeshAsset()->FindMorphTarget(PawnLooksFullBodyMarker))
			{
				State->Error(TEXT("a pawn does not wear the whole body (are the PawnLooks and Clothes assets made again? docs/design/looks.md)"));
				continue;
			}
			if (!Pawn->GetBareLayer().IsEmpty() || !Skin->IsVisible())
			{
				State->Error(TEXT("the whole body is hidden, or a skin layer is drawn over it"));
			}
			if ((Pawn->GetDefaultTop() && Pawn->GetDefaultTop()->IsVisible()) || (Pawn->GetDefaultTrousers() && Pawn->GetDefaultTrousers()->IsVisible()))
			{
				State->Error(TEXT("Kellan's own top or trousers are still drawn"));
			}
			float Total = 0.0f;
			for (const TPair<FName, float>& Morph : Pawn->GetLooks().MorphWeights())
			{
				Total += Morph.Value;
				if (!FMath::IsNearlyEqual(Skin->GetMorphTarget(Morph.Key), Morph.Value, 1.0e-4f))
				{
					State->Error(FString::Printf(TEXT("body morph %s is %.3f, wanted %.3f"), *Morph.Key.ToString(), Skin->GetMorphTarget(Morph.Key), Morph.Value));
				}
			}
			// The four corner weights add up to 1, less Kellan's own corner (no morph) for men.
			if (Pawn->GetLooks().bFemale && !FMath::IsNearlyEqual(Total, 1.0f, 1.0e-3f))
			{
				State->Error(FString::Printf(TEXT("a woman's body morphs add up to %.3f"), Total));
			}
			for (const UMeshComponent* Worn : Pawn->GetWornMeshes())
			{
				if (const USkeletalMeshComponent* Garment = Cast<USkeletalMeshComponent>(Worn))
				{
					for (const TPair<FName, float>& Morph : Pawn->GetLooks().MorphWeights())
					{
						if (!FMath::IsNearlyEqual(Garment->GetMorphTarget(Morph.Key), Morph.Value, 1.0e-4f))
						{
							State->Error(FString::Printf(TEXT("a garment's morph %s is %.3f, wanted %.3f"), *Morph.Key.ToString(), Garment->GetMorphTarget(Morph.Key), Morph.Value));
						}
					}
				}
			}
		}
	};
	const auto VerifyFrames = [=, this](const TCHAR* Phase)
	{
		Notes.Add(FString::Printf(TEXT("%s: %lld frames checked"), Phase, State->Frames));
		if (State->Frames < 5)
		{
			Fail(FString::Printf(TEXT("%s: too few frames checked (%lld)"), Phase, State->Frames));
		}
		if (State->Errors > 0)
		{
			Fail(FString::Printf(TEXT("%s: %d problems; the first: %s"), Phase, State->Errors, *State->FirstError));
		}
	};
	// A lamp near the camera (the sun only lights one side of a row).
	const auto Lamp = [=, this](const FVector& At, float Intensity)
	{
		if (!State->Light.IsValid())
		{
			State->Light = GetWorld()->SpawnActor<APointLight>(At, FRotator::ZeroRotator, FActorSpawnParameters());
			Spawned.Add(State->Light.Get());
		}
		if (APointLight* Light = State->Light.Get())
		{
			Light->PointLightComponent->SetMobility(EComponentMobility::Movable);
			Light->SetActorLocation(At);
			Light->PointLightComponent->SetIntensity(Intensity);
			Light->PointLightComponent->SetAttenuationRadius(1200.0f);
		}
	};
	// Looks at row pawns First to Last from in front of them, at Height cm over the ground, from Distance cm.
	const auto LookAtRow = [=, this](int32 First, int32 Last, float Distance, float Height)
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!B || !Pawns)
		{
			return;
		}
		// From the cells (the actors may not be there yet); a pawn faces +X until it has walked.
		const float MidY = State->Middle.y - BodiesSmokeRowCount / 2 + (First + Last) * 0.5f + 0.5f;
		const FVector Facing = FVector::XAxisVector;
		const FVector Target = B->SimToWorld(State->Middle.x + 0.5f, MidY, 0) + FVector(0.0, 0.0, Height);
		const FVector From = Target + Facing * Distance + FVector(0.0, 0.0, 15.0);
		ViewFrom(From, (Target - From).Rotation());
		Lamp(From + FVector(0.0, 0.0, 120.0), 120000.0f);
	};
	// Looks at the showcase from Yaw degrees around her (0 the front, 90 her left, 180 the back), Distance cm away, at Height cm over the ground.
	const auto LookAtShowcase = [=, this](float Yaw, float Distance, float Height, float Intensity)
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		const ASimPawnActor* Pawn = Pawns ? Pawns->FindPawn(State->Showcase) : nullptr;
		if (!B || !Pawn)
		{
			return;
		}
		// Height < 0: that far under the top of the head (the face mesh's head bone).
		const FVector Feet = Pawn->GetActorLocation();
		const USkeletalMeshComponent* Face = Pawn->GetFaceMesh();
		const FVector Target = Height < 0.0f && Face ? Face->GetSocketLocation(TEXT("head")) + FVector(0.0, 0.0, 6.0 + Height)
			: FVector(Feet.X, Feet.Y, B->SimToWorld(State->Middle.x + 6.5f, State->Middle.y - 8.5f, 0).Z + Height);
		const FVector Dir = Pawn->GetActorForwardVector().GetSafeNormal2D().RotateAngleAxis(Yaw, FVector::UpVector);
		const FVector From = Target + Dir * Distance;
		ViewFrom(From, (Target - From).Rotation());
		Lamp(From + FVector(0.0, 0.0, 60.0) + FVector::CrossProduct(FVector::UpVector, Dir) * 40.0, Intensity);
	};

	// 1. The row, bare: four women and four men in the corner body types.
	FPhase Bare;
	Bare.Name = TEXT("bodies_bare_women");
	Bare.WarmupSeconds = 8.0f;  // grooms bind on first use
	Bare.MeasureSeconds = 2.0f;
	Bare.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!B || !Pawns)
		{
			Fail(TEXT("USimSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		BodiesSmokeSet(TEXT("Pawns.MetaHumans"), 1.0f);
		float Hour = 7.0f;
		FParse::Value(FCommandLine::Get(), TEXT("BodiesHour="), Hour);
		BodiesSmokeSet(TEXT("Sky.Hour"), Hour);
		BodiesSmokeSet(TEXT("Sky.DayOfYear"), 22.5f);
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			PC->ConsoleCommand(TEXT("r.SetRes 1920x1080w"));
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 96;
		Options.bFlatMap = true;
		B->SetWorldOrigin(BodiesSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		State->Middle = colony::CellPos{48, 48, 0};
		for (int32 i = 0; i < BodiesSmokeRowCount; ++i)
		{
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{State->Middle.x, State->Middle.y - BodiesSmokeRowCount / 2 + i, 0}});
		}
		// A clear patch of ground: no trees, bushes or rocks near the row (their shadows and trunks hid the pawns).
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Pawn && FMath::Abs(View.x - State->Middle.x) < 22.0f && FMath::Abs(View.y - State->Middle.y) < 22.0f)
			{
				B->Submit(colony::DestroyThingCommand{View.id});
			}
		}
		if (colony::Sim* Sim = SimOf())
		{
			for (int32 x = State->Middle.x - 22; x <= State->Middle.x + 22; ++x)
			{
				for (int32 y = State->Middle.y - 22; y <= State->Middle.y + 22; ++y)
				{
					Sim->plants().remove(colony::CellPos{x, y, 0});
				}
			}
		}
		// The showcase stands on her own, in front of the row's left end.
		B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{State->Middle.x + 6, State->Middle.y - 9, 0}});
		B->StepOnce();
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		TArray<const colony::ThingView*> InRow;
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category != colony::ThingCategory::Pawn || View.lifeStage >= 0)
			{
				continue;
			}
			if (View.x >= State->Middle.x && View.x < State->Middle.x + 1)
			{
				InRow.Add(&View);
			}
			else if (View.x >= State->Middle.x + 6 && View.x < State->Middle.x + 7)
			{
				State->Showcase = View.id;
			}
		}
		InRow.Sort([](const colony::ThingView& A, const colony::ThingView& Bv) { return A.y < Bv.y; });
		const colony::DefDatabase* Defs = B->GetDefs().get();
		for (int32 i = 0; i < InRow.Num() && i < BodiesSmokeRowCount; ++i)
		{
			const colony::ThingView& View = *InRow[i];
			const FBodiesSmokeType& Type = BodiesSmokeRow[i];
			FSimPawnLooks Looks = View.looks >= 0 ? USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(View.looks)], Defs) : FSimPawnLooks();
			if (Looks.bFemale != Type.bFemale)
			{
				// The sim rolled the other gender: keep its face numbers, but the right gender's hair and no beard on a woman.
				Looks.bFemale = Type.bFemale;
				Looks.HairStyle = Type.bFemale ? TEXT("Ponytail") : TEXT("Short");
				Looks.Beard = Type.bFemale ? TEXT("None") : Looks.Beard;
			}
			Looks.Stature = Type.Stature;
			Looks.Weight = Type.Weight;
			Looks.Height = BodiesSmokeHeight(Type.bFemale, Type.Stature);
			Looks.Skin = FLinearColor(Type.Skin);
			Looks.Tattoos.Reset();
			Looks.Scars.Reset();
			Looks.BodyHair = Type.bFemale ? 0.0f : FMath::Min(Looks.BodyHair, 0.3f);
			Looks.HornLength = 0.0f;
			Pawns->SetLooksOverride(View.id, Looks);
			State->Row.Add(View.id);
			Notes.Add(FString::Printf(TEXT("  row %d: %s, height %.2f (%.0f cm)"), i, Type.Label, Looks.Height, Looks.Height * 180.0f));
		}
		if (State->Showcase != colony::kNoEntity)
		{
			// The showcase: very pale with light freckles, long straight black hair, light grey-green eyes, slim and petite.
			const colony::ThingView* View = Snapshot.find(State->Showcase);
			FSimPawnLooks Looks = View && View->looks >= 0 ? USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(View->looks)], Defs) : FSimPawnLooks();
			Looks.bFemale = true;
			Looks.Stature = -1.0f;
			Looks.Weight = -0.45f;
			Looks.Height = BodiesSmokeHeight(true, -1.0f);
			Looks.Skin = FLinearColor(FColor(232, 205, 190));
			Looks.Hair = FLinearColor(FColor(14, 12, 12));
			Looks.Eyes = FColor(107, 115, 120);
			Looks.HairStyle = TEXT("Long");
			Looks.Beard = TEXT("None");
			Looks.FaceShape = 0.45f;
			Looks.Brow = 0.3f;
			Looks.Nose = 0.45f;
			Looks.Jaw = 0.35f;
			Looks.BodyHair = 0.0f;
			Looks.Tattoos.Reset();
			Looks.Scars.Reset();
			Looks.HornLength = 0.0f;
			Looks.FaceSkin = 0;
			FParse::Value(FCommandLine::Get(), TEXT("BodiesFaceSkin="), Looks.FaceSkin);
			Pawns->SetLooksOverride(State->Showcase, Looks);
			State->Row.Add(State->Showcase);
		}
		// Bare: whatever the sim dressed them in comes off.
		if (colony::Sim* Sim = SimOf())
		{
			for (const colony::Entity Thing : State->Row)
			{
				const std::vector<colony::Entity> Worn = Sim->apparel().worn(Thing);
				for (const colony::Entity Item : Worn)
				{
					std::string Error;
					Sim->apparel().takeOff(Thing, Item, Error);
					if (Item != colony::kNoEntity)
					{
						B->Submit(colony::DestroyThingCommand{Item});
					}
				}
			}
			B->StepOnce();
		}
		if (State->Row.Num() != BodiesSmokeRowCount + 1)
		{
			Fail(FString::Printf(TEXT("%d test pawns, wanted %d"), State->Row.Num(), BodiesSmokeRowCount + 1));
		}
		LookAtRow(0, 3, 340.0f, 90.0f);
		State->Reset();
	};
	Bare.OnFrame = CheckFrame;
	Bare.Verify = [=, this]() { VerifyFrames(TEXT("bodies_bare_women")); };
	Phases.Add(MoveTemp(Bare));

	FPhase BareMen;
	BareMen.Name = TEXT("bodies_bare_men");
	BareMen.WarmupSeconds = 2.0f;
	BareMen.MeasureSeconds = 2.0f;
	BareMen.Setup = [=, this]() { LookAtRow(4, 7, 360.0f, 95.0f); State->Reset(); };
	BareMen.OnFrame = CheckFrame;
	BareMen.Verify = [=, this]() { VerifyFrames(TEXT("bodies_bare_men")); };
	Phases.Add(MoveTemp(BareMen));

	// 2. The same row in a T-shirt and trousers: the clothes take each body type's shape.
	for (const bool bWomen : {true, false})
	{
		FPhase Dressed;
		Dressed.Name = bWomen ? TEXT("bodies_dressed_women") : TEXT("bodies_dressed_men");
		Dressed.WarmupSeconds = 2.5f;
		Dressed.MeasureSeconds = 2.0f;
		Dressed.Setup = [=, this]()
		{
			colony::Sim* Sim = SimOf();
			if (Sim && bWomen)
			{
				for (int32 i = 0; i < BodiesSmokeRowCount && i < State->Row.Num(); ++i)
				{
					for (const char* Name : {"Apparel_Pants", "Apparel_TShirt"})
					{
						const colony::DefId Def = Sim->defs().idOf("Things", Name);
						const colony::DefId Cloth = Sim->defs().idOf("Things", "Cloth");
						std::string Why;
						const colony::Entity Item = Sim->apparel().make(Def, Cloth, -1, State->Middle, &Why);
						std::string Error;
						if (Item == colony::kNoEntity || !Sim->apparel().putOn(State->Row[i], Item, Error))
						{
							Fail(FString::Printf(TEXT("could not dress row pawn %d in %s: %s%s"), i, UTF8_TO_TCHAR(Name), UTF8_TO_TCHAR(Why.c_str()), UTF8_TO_TCHAR(Error.c_str())));
						}
					}
				}
				if (USimSubsystem* B = Bridge())
				{
					B->StepOnce();
				}
			}
			LookAtRow(bWomen ? 0 : 4, bWomen ? 3 : 7, bWomen ? 340.0f : 360.0f, bWomen ? 90.0f : 95.0f);
			State->Reset();
		};
		Dressed.OnFrame = CheckFrame;
		Dressed.Verify = [=, this]() { VerifyFrames(bWomen ? TEXT("bodies_dressed_women") : TEXT("bodies_dressed_men")); };
		Phases.Add(MoveTemp(Dressed));
	}

	// 3. The showcase woman: front, side, back, and her face.
	struct FShot
	{
		const TCHAR* Name;
		float Yaw;
		float Distance;
		float Height;
		float Intensity;
	};
	for (const FShot& Shot : {FShot{TEXT("bodies_showcase_front"), 12.0f, 230.0f, 82.0f, 160000.0f}, FShot{TEXT("bodies_showcase_side"), 90.0f, 230.0f, 82.0f, 160000.0f},
		FShot{TEXT("bodies_showcase_back"), 180.0f, 230.0f, 82.0f, 160000.0f}, FShot{TEXT("bodies_showcase_face"), 15.0f, 65.0f, -4.0f, 25000.0f}})
	{
		FPhase Phase;
		Phase.Name = Shot.Name;
		Phase.WarmupSeconds = 2.5f;
		Phase.MeasureSeconds = 1.0f;
		Phase.Setup = [=, this]() { LookAtShowcase(Shot.Yaw, Shot.Distance, Shot.Height, Shot.Intensity); State->Reset(); };
		Phase.OnFrame = CheckFrame;
		Phase.Verify = [=, this]()
		{
			VerifyFrames(Shot.Name);
			USimPawnSubsystem* Pawns = PawnsOf();
			const ASimPawnActor* Pawn = Pawns ? Pawns->FindPawn(State->Showcase) : nullptr;
			const UMaterialInterface* Skin = Pawn ? Pawn->GetFaceSkin() : nullptr;
			Notes.Add(FString::Printf(TEXT("%s: face skin %s"), Shot.Name, Skin ? *Skin->GetName() : TEXT("none")));
		};
		Phases.Add(MoveTemp(Phase));
	}

	// 4. Her face in each of the women's face skins (to pick the one with freckles that suits her).
	if (FParse::Param(FCommandLine::Get(), TEXT("BodiesAllFaceSkins")))
	{
		for (int32 Skin = 0; Skin < 13; ++Skin)
		{
			FPhase Phase;
			Phase.Name = FString::Printf(TEXT("bodies_showcase_skin_%02d"), Skin);
			Phase.WarmupSeconds = 2.0f;
			Phase.MeasureSeconds = 1.0f;
			Phase.Setup = [=, this]()
			{
				if (USimPawnSubsystem* Pawns = PawnsOf())
				{
					if (const ASimPawnActor* Pawn = Pawns->FindPawn(State->Showcase))
					{
						FSimPawnLooks Looks = Pawn->GetLooks();
						Looks.FaceSkin = Skin;
						Pawns->SetLooksOverride(State->Showcase, Looks);
					}
				}
				LookAtShowcase(15.0f, 65.0f, -4.0f, 25000.0f);
				State->Reset();
			};
			Phase.OnFrame = CheckFrame;
			Phase.Verify = [=, this]() { VerifyFrames(*FString::Printf(TEXT("bodies_showcase_skin_%02d"), Skin)); };
			Phases.Add(MoveTemp(Phase));
		}
	}

	FPhase Done;
	Done.Name = TEXT("bodies_cleanup");
	Done.WarmupSeconds = 0.5f;
	Done.MeasureSeconds = 0.5f;
	Done.Setup = [=, this]()
	{
		if (USimPawnSubsystem* Pawns = PawnsOf())
		{
			Pawns->ClearLooksOverrides();
		}
		BodiesSmokeSet(TEXT("Sky.Hour"), -1.0f);
		BodiesSmokeSet(TEXT("Sky.DayOfYear"), -1.0f);
	};
	Phases.Add(MoveTemp(Done));
}
