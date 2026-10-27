// Smoke test `clothes` (task U16): the apparel the sim says pawns wear, drawn on MetaHumans over their skin, tinted by stuff,
// torn by hit points, and taken off again, in the game, offscreen.
#include "SmokeTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"

#include "PawnActor.h"
#include "PawnLooks.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <string>
#include <vector>

namespace
{
	const FVector ClothesSmokeOrigin(-1300000.0, 0.0, 0.0);

	// Twelve pawns in a row, what each wears (stuff by the def: cloth, plain leather or none).
	const std::vector<std::vector<const char*>> ClothesSmokeOutfits = {
		{"Apparel_Pants", "Apparel_TShirt", "Apparel_CowboyHat"},
		{"Apparel_Pants", "Apparel_ButtonShirt", "Apparel_Tuque"},
		{"Apparel_Tribalwear"},
		{"Apparel_Pants", "Apparel_ButtonShirt", "Apparel_Duster"},
		{"Apparel_Pants", "Apparel_TShirt", "Apparel_Jacket"},
		{"Apparel_Pants", "Apparel_ButtonShirt", "Apparel_Parka", "Apparel_Tuque"},
		{"Apparel_FlakPants", "Apparel_TShirt", "Apparel_FlakVest", "Apparel_SimpleHelmet"},
		{},
		{"Apparel_TShirt"},
		{"Apparel_Pants"},
		{"Apparel_Pants", "Apparel_ButtonShirt"},
		{"Apparel_Tribalwear", "Apparel_Jacket", "Apparel_CowboyHat"},
	};

	struct FClothesSmokeState
	{
		/** The twelve test pawns, left to right (by their y). */
		TArray<colony::Entity> Row;
		colony::CellPos Middle = colony::CellPos{48, 48, 0};
		int32 Errors = 0;
		FString FirstError;
		int64 Frames = 0;
		int32 PiecesSeen = 0;
		int32 MeshesSeen = 0;
		int32 WornSeen = 0;

		void Reset()
		{
			Errors = 0;
			FirstError.Reset();
			Frames = 0;
			PiecesSeen = 0;
			MeshesSeen = 0;
			WornSeen = 0;
		}

		void Error(const FString& Message)
		{
			if (Errors++ == 0)
			{
				FirstError = Message;
			}
		}
	};

	void ClothesSmokeMetaHumans(bool bOn)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Pawns.MetaHumans")))
		{
			Var->Set(bOn ? 1 : 0, ECVF_SetByCode);
		}
	}

	/** A bright clear noon in summer (the sim's own clock starts at midnight); false puts the sim's clock back. */
	void ClothesSmokeNoon(bool bOn)
	{
		for (const TPair<const TCHAR*, float>& Var : {TPair<const TCHAR*, float>(TEXT("Sky.Hour"), bOn ? 11.0f : -1.0f), TPair<const TCHAR*, float>(TEXT("Sky.DayOfYear"), bOn ? 22.5f : -1.0f)})
		{
			if (IConsoleVariable* Console = IConsoleManager::Get().FindConsoleVariable(Var.Key))
			{
				Console->Set(Var.Value, ECVF_SetByCode);
			}
		}
	}

	float ClothesSmokeWear(float Share)
	{
		const float Wear = FMath::Clamp((0.7f - Share) / 0.7f, 0.0f, 1.0f);
		return FMath::RoundToFloat(Wear * 16.0f) * (1.0f / 16.0f);
	}
}

void USmokeTestSubsystem::BuildClothesPhases()
{
	const TSharedRef<FClothesSmokeState> State = MakeShared<FClothesSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto PawnsOf = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };

	// Puts the pieces on a pawn: new items made of cloth (or plain leather for the duster and jackets).
	const auto Dress = [=, this](colony::Entity Pawn, const std::vector<const char*>& Pieces)
	{
		colony::Sim* Sim = SimOf();
		for (const char* Name : Pieces)
		{
			const colony::DefId Def = Sim->defs().idOf("Things", Name);
			if (!Def.valid())
			{
				State->Error(FString::Printf(TEXT("no def %s"), UTF8_TO_TCHAR(Name)));
				continue;
			}
			const bool bStuffed = Sim->defs().get(Def).has("stuffCategories");
			const std::string Stuff = std::string(Name) == "Apparel_Duster" || std::string(Name) == "Apparel_Jacket" ? "Leather_Plain" : "Cloth";
			const colony::DefId StuffId = bStuffed ? Sim->defs().idOf("Things", Stuff) : colony::DefId{};
			std::string Why;
			const colony::Entity Item = Sim->apparel().make(Def, StuffId, -1, colony::CellPos{State->Middle.x, State->Middle.y, 0}, &Why);
			if (Item == colony::kNoEntity)
			{
				State->Error(FString::Printf(TEXT("could not make %s: %s"), UTF8_TO_TCHAR(Name), UTF8_TO_TCHAR(Why.c_str())));
				continue;
			}
			std::string Error;
			if (!Sim->apparel().putOn(Pawn, Item, Error))
			{
				State->Error(FString::Printf(TEXT("%s would not go on: %s"), UTF8_TO_TCHAR(Name), UTF8_TO_TCHAR(Error.c_str())));
			}
		}
	};

	// Every frame: each pawn's actor matches what the snapshot says it wears.
	const auto CheckFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!B || !B->GetSim() || !Pawns || PhaseTime < 2.5f)
		{
			return;
		}
		++State->Frames;
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		const colony::DefDatabase& Defs = *B->GetDefs();
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category != colony::ThingCategory::Pawn || View.lifeStage >= 0)
			{
				continue;
			}
			ASimPawnActor* Pawn = Pawns->FindPawn(View.id);
			if (!Pawn || !Pawn->GetMetaHuman())
			{
				State->Error(TEXT("a pawn has no actor or is not a MetaHuman"));
				continue;
			}
			if (Pawn->GetBareLayer().Num() != 2 || !Pawn->GetDefaultTop() || !Pawn->GetDefaultTrousers())
			{
				State->Error(FString::Printf(TEXT("a pawn has %d skin layers and top %d trousers %d, wanted 2 layers and Kellan's top and trousers found"),
					Pawn->GetBareLayer().Num(), Pawn->GetDefaultTop() != nullptr, Pawn->GetDefaultTrousers() != nullptr));
				continue;
			}
			if (Pawn->GetDefaultTop()->IsVisible() || Pawn->GetDefaultTrousers()->IsVisible())
			{
				State->Error(TEXT("Kellan's own top or trousers are still visible"));
			}
			for (const USkeletalMeshComponent* Layer : Pawn->GetBareLayer())
			{
				const TArray<float>& Data = Layer->GetCustomPrimitiveData().Data;
				if (!Layer->IsVisible() || !Layer->GetSkeletalMeshAsset() || !Layer->LeaderPoseComponent.IsValid())
				{
					State->Error(TEXT("a skin layer is hidden, has no mesh or follows nobody"));
				}
				else if (Pawn->HasLooks() && (Data.Num() < 3 || !FMath::IsNearlyEqual(Data[0], Pawn->GetLooks().Skin.R, 1.0e-4f) || !FMath::IsNearlyEqual(Data[2], Pawn->GetLooks().Skin.B, 1.0e-4f)))
				{
					State->Error(TEXT("a skin layer is not coloured with the pawn's skin"));
				}
			}

			// What the snapshot says is worn, against the meshes on the actor.
			int32 Wanted = 0;
			bool bHidesHair = false;
			for (uint32 I = 0; I < View.apparelCount; ++I)
			{
				const colony::ApparelView& Piece = Snapshot.apparel[View.firstApparel + I];
				const colony::Def& Def = Defs.get(Piece.def);
				const int32 Count = static_cast<int32>(Def.count("visual.worn"));
				Wanted += Count;
				bHidesHair = bHidesHair || Def.flag("apparel.hidesHair", false);
				++State->PiecesSeen;
				int32 Found = 0;
				for (int32 M = 0; M < Pawn->GetWornMeshItems().Num(); ++M)
				{
					if (Pawn->GetWornMeshItems()[M] == static_cast<uint32>(entt::to_integral(Piece.item)))
					{
						++Found;
						const UMeshComponent* Mesh = Pawn->GetWornMeshes()[M];
						if (!Mesh || !Mesh->IsVisible())
						{
							State->Error(FString::Printf(TEXT("%s: a mesh is missing or hidden"), UTF8_TO_TCHAR(Def.defName().c_str())));
							continue;
						}
						const TArray<float>& Data = Mesh->GetCustomPrimitiveData().Data;
						const float WearWanted = ClothesSmokeWear(Piece.hitPointShare);
						if (Data.Num() < 4 || !FMath::IsNearlyEqual(Data[3], WearWanted, 1.0e-4f))
						{
							State->Error(FString::Printf(TEXT("%s: wear %.3f, wanted %.3f for %.0f%% hit points"), UTF8_TO_TCHAR(Def.defName().c_str()),
								Data.Num() >= 4 ? Data[3] : -1.0f, WearWanted, Piece.hitPointShare * 100.0f));
						}
						if (const USkeletalMeshComponent* Skinned = Cast<USkeletalMeshComponent>(Mesh))
						{
							++State->MeshesSeen;
							if (!Skinned->GetSkeletalMeshAsset() || !Skinned->LeaderPoseComponent.IsValid())
							{
								State->Error(FString::Printf(TEXT("%s: a garment has no mesh or follows nobody"), UTF8_TO_TCHAR(Def.defName().c_str())));
							}
							if (Pawn->HasLooks())
							{
								for (const TPair<FName, float>& Morph : Pawn->GetLooks().MorphWeights())
								{
									if (!FMath::IsNearlyEqual(Skinned->GetMorphTarget(Morph.Key), Morph.Value, 1.0e-4f))
									{
										State->Error(FString::Printf(TEXT("%s: morph %s is %.3f, wanted %.3f"), UTF8_TO_TCHAR(Def.defName().c_str()), *Morph.Key.ToString(),
											Skinned->GetMorphTarget(Morph.Key), Morph.Value));
									}
								}
							}
						}
						else if (const UStaticMeshComponent* Prop = Cast<UStaticMeshComponent>(Mesh))
						{
							++State->MeshesSeen;
							if (Prop->GetAttachSocketName() != TEXT("head") || !Prop->GetStaticMesh())
							{
								State->Error(FString::Printf(TEXT("%s: a head piece is on socket %s"), UTF8_TO_TCHAR(Def.defName().c_str()), *Prop->GetAttachSocketName().ToString()));
							}
						}
					}
				}
				if (Found != Count)
				{
					State->Error(FString::Printf(TEXT("%s: %d of %d meshes are on the pawn"), UTF8_TO_TCHAR(Def.defName().c_str()), Found, Count));
				}
			}
			State->WornSeen += Pawn->GetWornMeshes().Num();
			if (Pawn->GetWornMeshes().Num() != Wanted)
			{
				State->Error(FString::Printf(TEXT("a pawn has %d worn meshes but its apparel needs %d"), Pawn->GetWornMeshes().Num(), Wanted));
			}
			if (Pawn->IsHairHidden() != bHidesHair)
			{
				State->Error(FString::Printf(TEXT("hair hidden %d but a covering hat is worn: %d"), Pawn->IsHairHidden(), bHidesHair));
			}
		}
	};
	const auto VerifyFrames = [=, this](const TCHAR* Phase)
	{
		Notes.Add(FString::Printf(TEXT("%s: %lld frames, %d pieces and %d garment and hat meshes checked"), Phase, State->Frames, State->PiecesSeen, State->MeshesSeen));
		if (State->Frames < 10)
		{
			Fail(FString::Printf(TEXT("%s: too few frames checked (%lld)"), Phase, State->Frames));
		}
		if (State->Errors > 0)
		{
			Fail(FString::Printf(TEXT("%s: %d problems; the first: %s"), Phase, State->Errors, *State->FirstError));
		}
	};
	const auto Look = [=, this](int32 FirstPawn, int32 Count, float Distance, float Height, float Pitch)
	{
		USimSubsystem* B = Bridge();
		const float Y = State->Middle.y - 6.0f + FirstPawn + (Count - 1) * 0.5f + 0.5f;
		const FVector Target = B->SimToWorld(State->Middle.x + 0.5f, Y, 0) + FVector(0.0, 0.0, Height);
		const FVector From = Target + FVector(Distance, 0.0, 30.0);
		ViewFrom(From, (Target - From).Rotation() + FRotator(Pitch, 0.0f, 0.0f));
	};

	// 1. Twelve pawns, dressed in everything A5 made, left half.
	FPhase Dressed;
	Dressed.Name = TEXT("clothes_dressed_left");
	Dressed.WarmupSeconds = 8.0f;
	Dressed.MeasureSeconds = 3.0f;
	Dressed.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !PawnsOf())
		{
			Fail(TEXT("USimSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		ClothesSmokeMetaHumans(true);
		ClothesSmokeNoon(true);
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 96;
		Options.bFlatMap = true;
		B->SetWorldOrigin(ClothesSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		State->Middle = colony::CellPos{48, 48, 0};
		B->SetSpeed(0);
		for (int32 I = 0; I < ClothesSmokeOutfits.size(); ++I)
		{
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{State->Middle.x, State->Middle.y - 6 + I, 0}});
		}
		B->StepOnce();
		TArray<TPair<float, colony::Entity>> ByY;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.lifeStage < 0)
			{
				ByY.Emplace(View.y, View.id);
			}
		}
		ByY.Sort([](const TPair<float, colony::Entity>& A, const TPair<float, colony::Entity>& C) { return A.Key < C.Key; });
		State->Row.Reset();
		for (const TPair<float, colony::Entity>& Pair : ByY)
		{
			State->Row.Add(Pair.Value);
		}
		if (State->Row.Num() != ClothesSmokeOutfits.size())
		{
			Fail(FString::Printf(TEXT("%d pawns, wanted %d"), State->Row.Num(), static_cast<int32>(ClothesSmokeOutfits.size())));
			return;
		}
		State->Reset();
		for (int32 I = 0; I < State->Row.Num(); ++I)
		{
			Dress(State->Row[I], ClothesSmokeOutfits[I]);
		}
		B->StepOnce();
		B->SetSpeed(0);
		Look(0, 6, 520.0f, 95.0f, 0.0f);
	};
	Dressed.OnFrame = CheckFrame;
	Dressed.Verify = [=, this]()
	{
		VerifyFrames(TEXT("clothes_dressed_left"));
		if (State->WornSeen < 20)
		{
			Fail(FString::Printf(TEXT("clothes_dressed_left: only %d worn meshes on the pawns"), State->WornSeen));
		}
	};
	Phases.Add(MoveTemp(Dressed));

	// 2. The right half.
	FPhase Right;
	Right.Name = TEXT("clothes_dressed_right");
	Right.WarmupSeconds = 2.0f;
	Right.MeasureSeconds = 3.0f;
	Right.Setup = [=, this]() { State->Reset(); Look(6, 6, 520.0f, 95.0f, 0.0f); };
	Right.OnFrame = CheckFrame;
	Right.Verify = [=, this]() { VerifyFrames(TEXT("clothes_dressed_right")); };
	Phases.Add(MoveTemp(Right));

	// 3. Close on one pawn from the front and the side: fit on the body.
	FPhase Close;
	Close.Name = TEXT("clothes_close");
	Close.WarmupSeconds = 2.0f;
	Close.MeasureSeconds = 3.0f;
	Close.Setup = [=, this]() { State->Reset(); Look(3, 1, 230.0f, 100.0f, 0.0f); };
	Close.OnFrame = CheckFrame;
	Close.Verify = [=, this]() { VerifyFrames(TEXT("clothes_close")); };
	Phases.Add(MoveTemp(Close));

	// 4. Wear: the shirt and trousers of the pawn in the 11th place go to 55%, 30% and 5% of their hit points. The cloth gets
	// holes and goes darker as it wears.
	FPhase Wear;
	Wear.Name = TEXT("clothes_torn");
	Wear.WarmupSeconds = 2.0f;
	Wear.MeasureSeconds = 3.0f;
	Wear.Setup = [=, this]()
	{
		colony::Sim* Sim = SimOf();
		if (!Sim || State->Row.Num() < 12)
		{
			return;
		}
		const float Shares[] = {0.55f, 0.05f, 0.30f};
		int32 I = 0;
		for (const colony::Entity Item : Sim->apparel().worn(State->Row[10]))
		{
			if (colony::HitPoints* Hp = Sim->things().registry().try_get<colony::HitPoints>(Item))
			{
				Hp->current = FMath::Max(1, FMath::RoundToInt(Hp->max * Shares[I++ % 3]));
			}
		}
		// And the ninth (a T-shirt) at 20%, the tenth's trousers at 45%.
		for (const colony::Entity Item : Sim->apparel().worn(State->Row[8]))
		{
			if (colony::HitPoints* Hp = Sim->things().registry().try_get<colony::HitPoints>(Item))
			{
				Hp->current = FMath::Max(1, Hp->max / 5);
			}
		}
		for (const colony::Entity Item : Sim->apparel().worn(State->Row[9]))
		{
			if (colony::HitPoints* Hp = Sim->things().registry().try_get<colony::HitPoints>(Item))
			{
				Hp->current = FMath::Max(1, FMath::RoundToInt(Hp->max * 0.45f));
			}
		}
		Bridge()->StepOnce();
		State->Reset();
		Look(8, 3, 330.0f, 95.0f, 0.0f);
	};
	Wear.OnFrame = CheckFrame;
	Wear.Verify = [=, this]()
	{
		VerifyFrames(TEXT("clothes_torn"));
		const USimPawnSubsystem* Pawns = PawnsOf();
		const ASimPawnActor* Pawn = Pawns && State->Row.Num() > 10 ? Pawns->FindPawn(State->Row[10]) : nullptr;
		float Worst = 0.0f;
		for (const UMeshComponent* Mesh : (Pawn ? Pawn->GetWornMeshes() : TArray<TObjectPtr<UMeshComponent>>()))
		{
			const TArray<float>& Data = Mesh->GetCustomPrimitiveData().Data;
			Worst = FMath::Max(Worst, Data.Num() >= 4 ? Data[3] : 0.0f);
		}
		Notes.Add(FString::Printf(TEXT("clothes_torn: the worst piece on the 11th pawn is drawn with wear %.2f"), Worst));
		if (Worst < 0.9f)
		{
			Fail(FString::Printf(TEXT("clothes_torn: a piece at 5%% hit points is drawn with wear only %.2f"), Worst));
		}
	};
	Phases.Add(MoveTemp(Wear));

	// 5. Clothes come off: the duster is taken off the fourth pawn, the seventh is stripped, the sixth's tuque wears out. The
	// meshes go, the skin shows, and the hair comes back where a hat had hidden it.
	FPhase Off;
	Off.Name = TEXT("clothes_off");
	Off.WarmupSeconds = 3.0f;
	Off.MeasureSeconds = 3.0f;
	Off.Setup = [=, this]()
	{
		colony::Sim* Sim = SimOf();
		if (!Sim || State->Row.Num() < 12)
		{
			return;
		}
		std::string Error;
		const colony::DefId Duster = Sim->defs().idOf("Things", "Apparel_Duster");
		for (const colony::Entity Item : std::vector<colony::Entity>(Sim->apparel().worn(State->Row[3])))
		{
			if (Sim->things().registry().get<colony::Thing>(Item).def == Duster)
			{
				Sim->apparel().takeOff(State->Row[3], Item, Error);
			}
		}
		Sim->apparel().strip(State->Row[6]);
		const colony::DefId Tuque = Sim->defs().idOf("Things", "Apparel_Tuque");
		for (const colony::Entity Item : std::vector<colony::Entity>(Sim->apparel().worn(State->Row[5])))
		{
			if (Sim->things().registry().get<colony::Thing>(Item).def == Tuque)
			{
				colony::PawnHitResult Hit;
				Hit.wear.emplace_back(Item, 100000);
				Sim->combat().wearApparel(State->Row[5], Hit);
			}
		}
		Bridge()->StepOnce();
		State->Reset();
		Look(3, 4, 420.0f, 95.0f, 0.0f);
	};
	Off.OnFrame = CheckFrame;
	Off.Verify = [=, this]()
	{
		VerifyFrames(TEXT("clothes_off"));
		const USimPawnSubsystem* Pawns = PawnsOf();
		const colony::Sim* Sim = SimOf();
		if (!Pawns || !Sim || State->Row.Num() < 12)
		{
			return;
		}
		const ASimPawnActor* Stripped = Pawns->FindPawn(State->Row[6]);
		const ASimPawnActor* Hatless = Pawns->FindPawn(State->Row[5]);
		Notes.Add(FString::Printf(TEXT("clothes_off: the stripped pawn has %d worn meshes, the 6th %d (was 7)"), Stripped ? Stripped->GetWornMeshes().Num() : -1,
			Hatless ? Hatless->GetWornMeshes().Num() : -1));
		if (!Stripped || Stripped->GetWornMeshes().Num() != 0 || Stripped->IsHairHidden())
		{
			Fail(TEXT("clothes_off: the stripped pawn still wears meshes or has its hair hidden"));
		}
		if (!Hatless || Hatless->IsHairHidden())
		{
			Fail(TEXT("clothes_off: the pawn whose tuque wore out still has its hair hidden"));
		}
	};
	Phases.Add(MoveTemp(Off));

	// 5b. The skin under the clothes close up: the stripped pawn and the one who wore nothing, front and back.
	FPhase Bare;
	Bare.Name = TEXT("clothes_bare");
	Bare.WarmupSeconds = 2.0f;
	Bare.MeasureSeconds = 3.0f;
	Bare.Setup = [=, this]() { State->Reset(); Look(6, 2, 300.0f, 95.0f, 0.0f); };
	Bare.OnFrame = CheckFrame;
	Bare.Verify = [=, this]()
	{
		VerifyFrames(TEXT("clothes_bare"));
		const USimPawnSubsystem* Pawns = PawnsOf();
		const colony::Snapshot& Snapshot = Bridge()->GetCurrentSnapshot();
		int32 Female = 0;
		int32 Heavy = 0;
		int32 Thin = 0;
		for (const colony::Entity Pawn : State->Row)
		{
			const colony::ThingView* View = Snapshot.find(Pawn);
			if (View && View->looks >= 0 && View->looks < static_cast<int32>(Snapshot.looks.size()))
			{
				const colony::PawnLooksView& Looks = Snapshot.looks[static_cast<size_t>(View->looks)];
				Female += Looks.female ? 1 : 0;
				Heavy += Looks.weight > 0.3f ? 1 : 0;
				Thin += Looks.weight < -0.3f ? 1 : 0;
			}
		}
		Notes.Add(FString::Printf(TEXT("clothes_bare: the twelve are %d women, %d heavy and %d thin (each piece is drawn on whichever body it fell to)"), Female, Heavy, Thin));
		(void)Pawns;
	};
	Phases.Add(MoveTemp(Bare));

	// 6. A crowd: 100 pawns in shirt and trousers (the cost of the clothes), against the same crowd as mannequins.
	FPhase Crowd;
	Crowd.Name = TEXT("clothes_crowd_100");
	Crowd.WarmupSeconds = 8.0f;
	Crowd.MeasureSeconds = 8.0f;
	Crowd.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		B->SetSpeed(0);
		for (int32 I = 0; I < 100 - State->Row.Num(); ++I)
		{
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{State->Middle.x - 12 + I % 25, State->Middle.y + 4 + I / 25 * 3, 0}});
		}
		B->StepOnce();
		TSet<colony::Entity> Known(State->Row);
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.lifeStage < 0 && !Known.Contains(View.id))
			{
				Dress(View.id, {"Apparel_Pants", "Apparel_TShirt"});
			}
		}
		B->StepOnce();
		B->SetSpeed(0);
		const FVector Target = B->SimToWorld(State->Middle.x, State->Middle.y + 8.0f, 0) + FVector(0.0, 0.0, 100.0);
		ViewFrom(Target + FVector(900.0, 0.0, 600.0), (FVector(-900.0, 0.0, -500.0)).Rotation());
		State->Reset();
	};
	Crowd.OnFrame = CheckFrame;
	Crowd.Verify = [=, this]()
	{
		VerifyFrames(TEXT("clothes_crowd_100"));
		const USimPawnSubsystem* Pawns = PawnsOf();
		Notes.Add(FString::Printf(TEXT("clothes_crowd_100: %d pawns, %d worn meshes on them"), Pawns ? Pawns->GetPawns().Num() : 0, State->WornSeen / FMath::Max<int32>(1, State->Frames)));
	};
	Phases.Add(MoveTemp(Crowd));

	// 7. Mannequins have no clothes to change: nothing breaks.
	FPhase Mannequins;
	Mannequins.Name = TEXT("clothes_mannequins");
	Mannequins.WarmupSeconds = 3.0f;
	Mannequins.MeasureSeconds = 2.0f;
	Mannequins.Setup = [=, this]() { ClothesSmokeMetaHumans(false); ClothesSmokeNoon(false); };
	Mannequins.Verify = [=, this]()
	{
		const USimPawnSubsystem* Pawns = PawnsOf();
		int32 Worn = 0;
		int32 Actors = 0;
		for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Pair : Pawns->GetPawns())
		{
			++Actors;
			Worn += Pair.Value->GetWornMeshes().Num();
			if (Pair.Value->GetMetaHuman())
			{
				Fail(TEXT("clothes_mannequins: a pawn is still a MetaHuman"));
			}
		}
		if (Actors == 0 || Worn != 0)
		{
			Fail(FString::Printf(TEXT("clothes_mannequins: %d actors with %d worn meshes"), Actors, Worn));
		}
	};
	Phases.Add(MoveTemp(Mannequins));
}
