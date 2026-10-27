// Smoke test `faces` (tasks V2, V3): seeded faces, hair, beards, eyes and skin marks on MetaHumans, in the game, offscreen.
#include "SmokeTest.h"

#include "Components/DecalComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/PointLight.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "GroomAsset.h"
#include "GroomBindingAsset.h"
#include "GroomComponent.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include "PawnActor.h"
#include "PawnFaces.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

namespace
{
	const FVector FacesSmokeOrigin(-320000.0, 0.0, 0.0);
	// Pawns in a row, a metre apart, facing +X (the way a pawn faces before it has walked).
	constexpr int32 FacesSmokeRow = 12;

	struct FFacesSmokeState
	{
		TArray<colony::Entity> Row;
		colony::CellPos Middle{48, 48, 0};
		int64 Frames = 0;
		int32 Errors = 0;
		FString FirstError;
		TWeakObjectPtr<APointLight> Light;
		colony::Entity Clothed = colony::kNoEntity;
		int32 MarkPawns = 0;
		int32 MarkDecals = 0;

		void Reset()
		{
			Frames = 0;
			Errors = 0;
			FirstError.Reset();
			MarkPawns = MarkDecals = 0;
		}

		void Error(const FString& Message)
		{
			if (Errors++ == 0)
			{
				FirstError = Message;
			}
		}
	};

	FString FacesSmokeExpectedHair(const FSimPawnLooks& Looks)
	{
		return Looks.HairStyle == TEXT("Bald") ? FString() : PawnFacesGroomName(TEXT("Hair"), Looks.HairStyle);
	}

	bool FacesSmokeHasBeard(const FSimPawnLooks& Looks)
	{
		return !Looks.Beard.IsEmpty() && Looks.Beard != TEXT("None");
	}
}

void USmokeTestSubsystem::BuildFacesPhases()
{
	const TSharedRef<FFacesSmokeState> State = MakeShared<FFacesSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto PawnsOf = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };

	// Every frame: each pawn in the row draws the face, eyes, hair, beard, brows and marks its sim looks give it.
	const auto CheckFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!B || !B->GetSim() || !Pawns || PhaseTime < 3.0f)
		{
			return;
		}
		++State->Frames;
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		for (const colony::Entity Thing : State->Row)
		{
			const colony::ThingView* View = Snapshot.find(Thing);
			ASimPawnActor* Pawn = Pawns->FindPawn(Thing);
			if (!View || !Pawn || View->looks < 0 || View->looks >= static_cast<int32>(Snapshot.looks.size()))
			{
				State->Error(TEXT("a pawn of the row has no actor or no looks"));
				continue;
			}
			const FSimPawnLooks Wanted = USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(View->looks)], B->GetDefs().get());
			USkeletalMeshComponent* Face = Pawn->GetFaceMesh();
			if (!Pawn->HasLooks() || !(Pawn->GetLooks() == Wanted) || !Wanted.HasFace())
			{
				State->Error(TEXT("a pawn's actor doesn't show its sim looks, or they have no face"));
				continue;
			}
			if (!Face || !Face->GetSkeletalMeshAsset() || !Face->GetSkeletalMeshAsset()->GetName().StartsWith(TEXT("SK_Face_")))
			{
				State->Error(TEXT("a MetaHuman has no shaped face (is game/Content/Rim3D/Generated/PawnFaces made? docs/design/looks.md)"));
				continue;
			}
			for (const TPair<FName, float>& Morph : Wanted.FaceMorphWeights())
			{
				if (!FMath::IsNearlyEqual(Face->GetMorphTarget(Morph.Key), Morph.Value, 1.0e-4f))
				{
					State->Error(FString::Printf(TEXT("face morph %s is %.3f, wanted %.3f"), *Morph.Key.ToString(), Face->GetMorphTarget(Morph.Key), Morph.Value));
				}
			}
			for (int32 Slot = 0; Slot < Face->GetNumMaterials(); ++Slot)
			{
				const UMaterialInterface* Material = Face->GetMaterial(Slot);
				const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
				if (Base && !Base->bUsedWithMorphTargets)
				{
					State->Error(FString::Printf(TEXT("face slot %d: %s is not marked for morph targets"), Slot, *Material->GetName()));
				}
			}
			if (Pawn->GetEyeSlots().Num() != 2)
			{
				State->Error(FString::Printf(TEXT("the face has %d eye slots, not 2"), Pawn->GetEyeSlots().Num()));
			}
			for (const int32 Slot : Pawn->GetEyeSlots())
			{
				const UMaterialInterface* Eye = Face->GetMaterial(Slot);
				if (!Eye || Eye->GetName() != PawnFacesEyeInstanceName(Wanted.Eyes))
				{
					State->Error(FString::Printf(TEXT("eye material %s, wanted %s"), Eye ? *Eye->GetName() : TEXT("none"), *PawnFacesEyeInstanceName(Wanted.Eyes)));
				}
			}
			const FString Hair = Pawn->GetGroomName(TEXT("Hair"));
			const FString Beard = Pawn->GetGroomName(TEXT("Beard"));
			const FString Moustache = Pawn->GetGroomName(TEXT("Mustache"));
			const FString Brows = Pawn->GetGroomName(TEXT("Eyebrows"));
			if (Hair != FacesSmokeExpectedHair(Wanted))
			{
				State->Error(FString::Printf(TEXT("hair style %s drawn as %s"), *Wanted.HairStyle, *Hair));
			}
			const bool bBeard = FacesSmokeHasBeard(Wanted);
			const FString WantBeard = bBeard && Wanted.Beard != TEXT("Moustache") ? PawnFacesGroomName(TEXT("Beard"), Wanted.Beard) : FString();
			const FString WantMoustache = bBeard ? PawnFacesGroomName(TEXT("Moustache"), Wanted.Beard) : FString();
			if (Beard != WantBeard || Moustache != WantMoustache)
			{
				State->Error(FString::Printf(TEXT("beard %s drawn as %s and %s"), *Wanted.Beard, *Beard, *Moustache));
			}
			if (!Brows.StartsWith(PawnFacesGroomName(TEXT("Brows"), Wanted.bFemale ? TEXT("F") : TEXT("M"))))
			{
				State->Error(FString::Printf(TEXT("eyebrows %s for a %s"), *Brows, Wanted.bFemale ? TEXT("woman") : TEXT("man")));
			}
			if (Wanted.bFemale && (!Beard.IsEmpty() || !Moustache.IsEmpty()))
			{
				State->Error(TEXT("a woman has a beard or moustache"));
			}
			// V2b: a woman's face texture is one of the female Creator skins; a man's a male one or Kellan's.
			const UMaterialInterface* FaceSkin = Pawn->GetFaceSkin();
			const FString SkinName = FaceSkin ? FaceSkin->GetName() : FString();
			const bool bOtherGender = SkinName.StartsWith(PawnFacesSkinName(!Wanted.bFemale, 0).LeftChop(1));
			if (!FaceSkin || bOtherGender || (Wanted.bFemale && !SkinName.StartsWith(PawnFacesSkinName(true, 0).LeftChop(1))))
			{
				State->Error(FString::Printf(TEXT("a %s's face skin is %s"), Wanted.bFemale ? TEXT("woman") : TEXT("man"), FaceSkin ? *SkinName : TEXT("missing")));
			}
			// V2b: the skin under the clothes is the skin material in the pawn's own colour.
			if (Pawn->GetBareSkins().Num() != Pawn->GetBareLayer().Num() || Pawn->GetBareSkins().IsEmpty())
			{
				State->Error(FString::Printf(TEXT("%d bare skin materials for %d bare layers"), Pawn->GetBareSkins().Num(), Pawn->GetBareLayer().Num()));
			}
			for (const UMaterialInstanceDynamic* BareSkin : Pawn->GetBareSkins())
			{
				FLinearColor Color;
				if (!BareSkin || !BareSkin->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("SkinColor")), Color) || !Color.Equals(Wanted.Skin, 1.0e-4f))
				{
					State->Error(TEXT("the bare skin is not the pawn's skin colour"));
				}
			}
			// Marks: at full detail one decal per tattoo and scar, and for body hair four (forearms and shins), six on men (chest and belly).
			if (Pawn->GetDetail() == ESimPawnDetail::Full)
			{
				const int32 Want = Wanted.Tattoos.Num() + Wanted.Scars.Num() + (Wanted.BodyHair > 0.05f ? (Wanted.bFemale ? 4 : 6) : 0);
				if (Pawn->GetMarkDecals().Num() != Want)
				{
					State->Error(FString::Printf(TEXT("%d mark decals, wanted %d (%d tattoos, %d scars, body hair %.2f)"), Pawn->GetMarkDecals().Num(), Want,
						Wanted.Tattoos.Num(), Wanted.Scars.Num(), Wanted.BodyHair));
				}
				State->MarkPawns += Want > 0 ? 1 : 0;
				State->MarkDecals += Pawn->GetMarkDecals().Num();
			}
		}
	};

	const auto VerifyFrames = [=, this](const TCHAR* Phase)
	{
		Notes.Add(FString::Printf(TEXT("%s: %lld frames checked"), Phase, State->Frames));
		if (State->Frames < 10)
		{
			Fail(FString::Printf(TEXT("%s: too few frames checked (%lld)"), Phase, State->Frames));
		}
		if (State->Errors > 0)
		{
			Fail(FString::Printf(TEXT("%s: %d problems; the first: %s"), Phase, State->Errors, *State->FirstError));
		}
	};

	// Looks at pawns First to Last of the row from in front, at head height, from Distance cm.
	const auto LookAtRow = [=, this](int32 First, int32 Last, float Distance, float Height)
	{
		USimSubsystem* B = Bridge();
		if (!B)
		{
			return;
		}
		const float MidY = State->Middle.y - FacesSmokeRow / 2 + (First + Last) * 0.5f + 0.5f;
		const FVector Target = B->SimToWorld(State->Middle.x + 0.5f, MidY, 0) + FVector(0.0, 0.0, Height);
		// In front of where the pawns face (they keep the way they faced when they stopped), a little to one side.
		FVector Facing = FVector::ZeroVector;
		USimPawnSubsystem* Pawns = PawnsOf();
		for (int32 i = First; Pawns && i <= Last && i < State->Row.Num(); ++i)
		{
			if (const ASimPawnActor* Pawn = Pawns->FindPawn(State->Row[i]))
			{
				Facing += Pawn->GetActorForwardVector();
			}
		}
		Facing = Facing.GetSafeNormal2D();
		Facing = Facing.IsNearlyZero() ? FVector::XAxisVector : Facing;
		const FVector From = Target + (Facing + FVector::CrossProduct(FVector::UpVector, Facing) * 0.25f) * Distance;
		ViewFrom(From, (Target - From).Rotation());
	};

	// 1. Twelve pawns side by side: no two look alike.
	FPhase Lineup;
	Lineup.Name = TEXT("faces_lineup");
	Lineup.WarmupSeconds = 8.0f;  // grooms bind on first use
	Lineup.MeasureSeconds = 3.0f;
	Lineup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !PawnsOf())
		{
			Fail(TEXT("USimSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		// MetaHumans, on a summer morning with the sun low on their right (the sim's own clock starts at midnight).
		for (const TPair<const TCHAR*, float>& Var : {TPair<const TCHAR*, float>(TEXT("Pawns.MetaHumans"), 1.0f), TPair<const TCHAR*, float>(TEXT("Sky.Hour"), 8.5f),
			TPair<const TCHAR*, float>(TEXT("Sky.DayOfYear"), 22.5f)})
		{
			if (IConsoleVariable* Found = IConsoleManager::Get().FindConsoleVariable(Var.Key))
			{
				Found->Set(Var.Value, ECVF_SetByCode);
			}
		}
		FSimStartOptions Options;
		Options.Seed = 7;
		Options.MapSize = 96;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(FacesSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		State->Middle = B->GetSim()->stockpileCell().value_or(colony::CellPos{48, 48, 0});
		State->Middle.x += 3;
		for (int32 i = 0; i < FacesSmokeRow; ++i)
		{
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{State->Middle.x, State->Middle.y - FacesSmokeRow / 2 + i, 0}});
		}
		B->StepOnce();
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.x >= State->Middle.x && View.x < State->Middle.x + 1)
			{
				State->Row.Add(View.id);
			}
		}
		State->Row.Sort([&Snapshot](const colony::Entity A, const colony::Entity Bv) { return Snapshot.find(A)->y < Snapshot.find(Bv)->y; });
		LookAtRow(0, FacesSmokeRow - 1, 900.0f, 120.0f);
		State->Reset();
	};
	Lineup.OnFrame = CheckFrame;
	Lineup.Verify = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		VerifyFrames(TEXT("faces_lineup"));
		if (State->Row.Num() != FacesSmokeRow)
		{
			Fail(FString::Printf(TEXT("faces_lineup: %d pawns in the row, not %d"), State->Row.Num(), FacesSmokeRow));
		}
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		TSet<FString> Looks, Hair, Eyes, Skins;
		int32 Women = 0, Bearded = 0;
		double Closest = 9.0;
		TArray<FSimPawnLooks> All;
		for (const colony::Entity Thing : State->Row)
		{
			const colony::ThingView* View = Snapshot.find(Thing);
			if (!View || View->looks < 0)
			{
				continue;
			}
			const FSimPawnLooks L = USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(View->looks)], B->GetDefs().get());
			for (const FSimPawnLooks& Other : All)
			{
				Closest = FMath::Min(Closest, static_cast<double>(FVector4(L.FaceShape - Other.FaceShape, L.Brow - Other.Brow, L.Nose - Other.Nose, L.Jaw - Other.Jaw).Size()));
			}
			All.Add(L);
			Looks.Add(FString::Printf(TEXT("%d %s %s %s %.1f %.1f %.1f %.1f"), L.bFemale, *L.HairStyle, *L.Beard, *L.Eyes.ToHex(), L.FaceShape, L.Brow, L.Nose, L.Jaw));
			Hair.Add(L.HairStyle);
			Eyes.Add(L.Eyes.ToHex());
			Women += L.bFemale ? 1 : 0;
			Bearded += FacesSmokeHasBeard(L) ? 1 : 0;
			const ASimPawnActor* Pawn = PawnsOf() ? PawnsOf()->FindPawn(Thing) : nullptr;
			const UMaterialInterface* FaceSkin = Pawn ? Pawn->GetFaceSkin() : nullptr;
			Skins.Add(FaceSkin ? FaceSkin->GetName() : FString());
			Notes.Add(FString::Printf(TEXT("  %s, %s hair, beard %s, eyes #%s, face %.2f brow %.2f nose %.2f jaw %.2f, body hair %.2f, %d tattoos, %d scars"),
				L.bFemale ? TEXT("woman") : TEXT("man"), *L.HairStyle, *L.Beard, *L.Eyes.ToHex().Left(6), L.FaceShape, L.Brow, L.Nose, L.Jaw, L.BodyHair,
				L.Tattoos.Num(), L.Scars.Num()));
		}
		Notes.Add(FString::Printf(TEXT("faces_lineup: %d different looks of %d, %d hair styles, %d eye colours, %d face skins (%s), %d women, %d with beards, closest faces %.2f apart (sliders)"),
			Looks.Num(), All.Num(), Hair.Num(), Eyes.Num(), Skins.Num(), *FString::Join(Skins.Array(), TEXT(" ")), Women, Bearded, Closest));
		if (Looks.Num() != All.Num() || Hair.Num() < 5 || Eyes.Num() < 3 || Skins.Num() < 4 || Women == 0 || Bearded == 0)
		{
			Fail(TEXT("faces_lineup: the pawns look too alike"));
		}
	};
	Phases.Add(MoveTemp(Lineup));

	// 2 to 4. Close up, four faces at a time.
	for (int32 Group = 0; Group < 3; ++Group)
	{
		FPhase Close;
		Close.Name = FString::Printf(TEXT("faces_closeup_%d"), Group + 1);
		Close.WarmupSeconds = 2.5f;
		Close.MeasureSeconds = 1.0f;
		Close.Setup = [=, this]()
		{
			LookAtRow(Group * 4, Group * 4 + 3, 280.0f, 160.0f);
			State->Reset();
			// What each groom draws, for the notes.
			USimPawnSubsystem* Pawns = PawnsOf();
			for (int32 i = Group * 4; Pawns && i < Group * 4 + 4 && i < State->Row.Num(); ++i)
			{
				const ASimPawnActor* Pawn = Pawns->FindPawn(State->Row[i]);
				const AActor* MetaHuman = Pawn ? Pawn->GetMetaHuman() : nullptr;
				FString Line;
				TArray<UGroomComponent*> Grooms;
				if (MetaHuman)
				{
					MetaHuman->GetComponents(Grooms);
				}
				for (const UGroomComponent* Groom : Grooms)
				{
					Line += FString::Printf(TEXT(" %s=%s/%s vis %d r %.0f;"), *Groom->GetName(), Groom->GroomAsset ? *Groom->GroomAsset->GetName() : TEXT("-"),
						Groom->BindingAsset ? *Groom->BindingAsset->GetName() : TEXT("-"), Groom->IsVisible(), Groom->Bounds.SphereRadius);
				}
				Notes.Add(FString::Printf(TEXT("  pawn %d:%s"), i, *Line));
			}
		};
		Close.OnFrame = CheckFrame;
		Close.Verify = [=, this]() { VerifyFrames(*FString::Printf(TEXT("faces_closeup_%d"), Group + 1)); };
		Phases.Add(MoveTemp(Close));
	}

	// Portraits, at full HD: four faces from 70 cm, in front of the head whichever way it faces, from the side with the clearer view.
	for (int32 Shot = 0; Shot < 4; ++Shot)
	{
		FPhase Portrait;
		Portrait.Name = FString::Printf(TEXT("faces_portrait_%d"), Shot + 1);
		Portrait.WarmupSeconds = 3.0f;
		Portrait.MeasureSeconds = 1.0f;
		Portrait.Setup = [=, this]()
		{
			if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
			{
				PC->ConsoleCommand(TEXT("r.SetRes 1920x1080w"));
			}
			State->Reset();
			// Every third pawn of the row (0, 3, 6, 9), or the next one with a clear view.
			USimPawnSubsystem* Pawns = PawnsOf();
			for (int32 i = Shot * 3; Pawns && i < State->Row.Num(); ++i)
			{
				const ASimPawnActor* Pawn = Pawns->FindPawn(State->Row[i]);
				const USkeletalMeshComponent* Face = Pawn ? Pawn->GetFaceMesh() : nullptr;
				if (!Face)
				{
					continue;
				}
				const FVector Head = Face->GetSocketLocation(TEXT("head")) + FVector(0.0, 0.0, 6.0) + Pawn->GetActorForwardVector() * 4.0;
				for (const double Side : {0.3, -0.3, 0.0})
				{
					const FVector From = Head + (Pawn->GetActorForwardVector() + Pawn->GetActorRightVector() * Side) * 70.0;
					FHitResult Hit;
					FCollisionQueryParams Params;
					Params.AddIgnoredActor(Pawn);
					Params.AddIgnoredActor(Pawn->GetMetaHuman());
					if (!GetWorld()->LineTraceSingleByChannel(Hit, From, Head, ECC_Visibility, Params))
					{
						Notes.Add(FString::Printf(TEXT("faces_portrait_%d: pawn %d of the row"), Shot + 1, i));
						ViewFrom(From, (Head - From).Rotation());
						// A lamp beside the camera: the sun never comes from where these pawns face.
						if (!State->Light.IsValid())
						{
							FActorSpawnParameters SpawnParams;
							State->Light = GetWorld()->SpawnActor<APointLight>(From, FRotator::ZeroRotator, SpawnParams);
							Spawned.Add(State->Light.Get());
							if (State->Light.IsValid())
							{
								State->Light->PointLightComponent->SetMobility(EComponentMobility::Movable);
							}
						}
						if (APointLight* Light = State->Light.Get())
						{
							Light->SetActorLocation(From + FVector(0.0, 0.0, 40.0) + Pawn->GetActorRightVector() * 30.0);
							Light->PointLightComponent->SetIntensity(4000.0f);
							Light->PointLightComponent->SetAttenuationRadius(400.0f);
						}
						return;
					}
				}
			}
		};
		Portrait.OnFrame = CheckFrame;
		Portrait.Verify = [=, this]() { VerifyFrames(*FString::Printf(TEXT("faces_portrait_%d"), Shot + 1)); };
		Phases.Add(MoveTemp(Portrait));
	}

	// Looks at one pawn from in front of it, Distance cm away, at Drop cm under the head (30: the chest), with the lamp beside the camera.
	const auto LookAtPawn = [=, this](colony::Entity Thing, float Distance, float Drop)
	{
		USimPawnSubsystem* Pawns = PawnsOf();
		const ASimPawnActor* Pawn = Pawns ? Pawns->FindPawn(Thing) : nullptr;
		const USkeletalMeshComponent* Face = Pawn ? Pawn->GetFaceMesh() : nullptr;
		if (!Face)
		{
			return;
		}
		const FVector Chest = Face->GetSocketLocation(TEXT("head")) - FVector(0.0, 0.0, Drop);
		const FVector From = Chest + (Pawn->GetActorForwardVector() + Pawn->GetActorRightVector() * 0.3) * Distance;
		ViewFrom(From, (Chest - From).Rotation());
		if (!State->Light.IsValid())
		{
			State->Light = GetWorld()->SpawnActor<APointLight>(From, FRotator::ZeroRotator, FActorSpawnParameters());
			Spawned.Add(State->Light.Get());
		}
		if (APointLight* Light = State->Light.Get())
		{
			Light->PointLightComponent->SetMobility(EComponentMobility::Movable);
			Light->SetActorLocation(From + FVector(0.0, 0.0, 40.0));
			Light->PointLightComponent->SetIntensity(6000.0f);
			Light->PointLightComponent->SetAttenuationRadius(500.0f);
		}
	};

	// 4b. Skin (V2b): the first woman of the row, then the darkest-skinned pawn, face, neck and bare shoulders in one view, so
	// a woman's face texture and the bare skin against the face and hands can be seen.
	for (const bool bWoman : {true, false})
	{
		FPhase Skin;
		Skin.Name = bWoman ? TEXT("faces_skin_woman") : TEXT("faces_skin_dark");
		Skin.WarmupSeconds = 2.5f;
		Skin.MeasureSeconds = 1.0f;
		Skin.Setup = [=, this]()
		{
			USimSubsystem* B = Bridge();
			USimPawnSubsystem* Pawns = PawnsOf();
			if (!B || !Pawns)
			{
				return;
			}
			colony::Entity Pick = colony::kNoEntity;
			float Darkest = 9.0f;
			for (const colony::Entity Thing : State->Row)
			{
				const ASimPawnActor* Pawn = Pawns->FindPawn(Thing);
				if (!Pawn || !Pawn->HasLooks())
				{
					continue;
				}
				const FSimPawnLooks& L = Pawn->GetLooks();
				if (bWoman ? (L.bFemale && Pick == colony::kNoEntity) : (L.Skin.GetLuminance() < Darkest))
				{
					Pick = Thing;
					Darkest = L.Skin.GetLuminance();
				}
			}
			if (Pick == colony::kNoEntity)
			{
				Fail(FString::Printf(TEXT("%s: no such pawn in the row"), bWoman ? TEXT("faces_skin_woman") : TEXT("faces_skin_dark")));
				return;
			}
			const ASimPawnActor* Pawn = Pawns->FindPawn(Pick);
			const UMaterialInterface* FaceSkin = Pawn->GetFaceSkin();
			Notes.Add(FString::Printf(TEXT("%s: pawn %d of the row, skin %s, face skin %s"), bWoman ? TEXT("faces_skin_woman") : TEXT("faces_skin_dark"),
				State->Row.IndexOfByKey(Pick), *Pawn->GetLooks().Skin.ToFColorSRGB().ToHex().Left(6), FaceSkin ? *FaceSkin->GetName() : TEXT("none")));
			LookAtPawn(Pick, 95.0f, 12.0f);
			State->Reset();
		};
		Skin.OnFrame = CheckFrame;
		Skin.Verify = [=, this]() { VerifyFrames(bWoman ? TEXT("faces_skin_woman") : TEXT("faces_skin_dark")); };
		Phases.Add(MoveTemp(Skin));
	}

	// 5. Marks: the first of the row (bare: it wears nothing) and a clothed colonist get tattoos and scars on every region
	// and thick body hair. They show on the bare skin, never on the clothes.
	FPhase Marks;
	Marks.Name = TEXT("faces_marks");
	Marks.WarmupSeconds = 3.0f;
	Marks.MeasureSeconds = 1.5f;
	Marks.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		colony::Sim* Sim = B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr;
		if (!Sim || State->Row.Num() < 2)
		{
			return;
		}
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.apparelCount > 0 && !State->Row.Contains(View.id))
			{
				State->Clothed = View.id;
				break;
			}
		}
		for (const colony::Entity Pawn : {State->Row[0], State->Clothed})
		{
			if (colony::PawnLooks* L = Pawn != colony::kNoEntity ? Sim->things().registry().try_get<colony::PawnLooks>(Pawn) : nullptr)
			{
				L->bodyHair = 1.0f;
				L->tattoos = {{colony::BodyRegion::Torso, 0, 0.08f, 0.7f, 1.2f}, {colony::BodyRegion::Torso, 3, 0.92f, 0.45f, 1.0f},
					{colony::BodyRegion::LeftArm, 8, 0.0f, 0.3f, 1.0f}, {colony::BodyRegion::Neck, 4, 0.0f, 0.5f, 0.8f}, {colony::BodyRegion::Face, 9, 0.3f, 0.75f, 0.6f}};
				L->scars = {{colony::BodyRegion::Torso, 0, 0.5f, 0.45f, 1.2f}, {colony::BodyRegion::RightArm, 2, 0.0f, 0.6f, 1.0f},
					{colony::BodyRegion::LeftLeg, 5, 0.0f, 0.4f, 1.0f}, {colony::BodyRegion::Face, 1, 0.75f, 0.4f, 0.8f}};
			}
		}
		B->StepOnce();
		LookAtPawn(State->Row[0], 110.0f, 30.0f);
		State->Reset();
	};
	Marks.OnFrame = CheckFrame;
	Marks.Verify = [=, this]()
	{
		VerifyFrames(TEXT("faces_marks"));
		Notes.Add(FString::Printf(TEXT("faces_marks: %d pawns with marks drew %d decals a frame on average"), State->Frames ? State->MarkPawns / static_cast<int32>(State->Frames) : 0,
			State->Frames ? State->MarkDecals / static_cast<int32>(State->Frames) : 0));
		if (State->MarkDecals == 0)
		{
			Fail(TEXT("faces_marks: no mark decals"));
		}
	};
	Phases.Add(MoveTemp(Marks));

	// 5b. The clothed colonist with the same marks: only the face, neck and hands may show them.
	FPhase Clothed;
	Clothed.Name = TEXT("faces_marks_clothed");
	Clothed.WarmupSeconds = 2.5f;
	Clothed.MeasureSeconds = 1.0f;
	Clothed.Setup = [=, this]()
	{
		if (State->Clothed == colony::kNoEntity)
		{
			Fail(TEXT("faces_marks_clothed: no clothed colonist"));
			return;
		}
		LookAtPawn(State->Clothed, 110.0f, 30.0f);
		USimPawnSubsystem* Pawns = PawnsOf();
		const ASimPawnActor* Pawn = Pawns ? Pawns->FindPawn(State->Clothed) : nullptr;
		Notes.Add(FString::Printf(TEXT("faces_marks_clothed: the colonist wears %d meshes and has %d mark decals"), Pawn ? Pawn->GetWornMeshes().Num() : -1,
			Pawn ? Pawn->GetMarkDecals().Num() : -1));
	};
	Clothed.Verify = [=, this]() {};
	Phases.Add(MoveTemp(Clothed));

	// 6. What it costs: 100 MetaHumans with faces and hair walking about, from further off.
	FPhase Crowd;
	Crowd.Name = TEXT("faces_crowd_100");
	Crowd.WarmupSeconds = 8.0f;
	Crowd.MeasureSeconds = 6.0f;
	Crowd.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		int32 Pawns = 0;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			Pawns += View.category == colony::ThingCategory::Pawn ? 1 : 0;
		}
		for (int32 i = 0; Pawns + i < 100; ++i)
		{
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{State->Middle.x - 20 + i % 20, State->Middle.y + 8 + i / 20 * 2, 0}});
		}
		B->SetSpeed(1);
		const FVector Target = B->SimToWorld(State->Middle.x - 10.0f, State->Middle.y + 10.0f, 0);
		ViewFrom(Target + FVector(1800.0, -600.0, 1400.0), (-FVector(1800.0, -600.0, 1400.0)).Rotation());
		State->Reset();
	};
	Crowd.OnFrame = CheckFrame;
	Crowd.Verify = [=, this]() { VerifyFrames(TEXT("faces_crowd_100")); };
	Phases.Add(MoveTemp(Crowd));
}
