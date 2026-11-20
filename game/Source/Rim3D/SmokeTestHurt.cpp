// Smoke test `hurt` (docs/design/body-health.md part E): what a hurt body shows. A colonist is hit on each of its six
// zones (the struck zone must wobble and settle while the animation underneath plays on), walks on a hurt leg (the
// body must lurch over it), patches a bleeding wound (bent over it) and is downed by the torso (it lies in the crawl,
// not as a ragdoll).
#include "SmokeTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/PointLight.h"

#include "PawnActor.h"
#include "PawnAnimInstance.h"
#include "BloodSubsystem.h"
#include "FightSubsystem.h"
#include "PawnSubsystem.h"
#include "SeveredPieceActor.h"
#include "SimSubsystem.h"
#include "TerrainSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Health.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

namespace
{
	const FVector HurtSmokeOrigin(340000.0, 0.0, 0.0);

	struct FHurtSmokeState
	{
		colony::Entity Pawn = colony::kNoEntity;
		// wobble
		int32 KicksSent = 0;
		uint32 ZonesSeen = 0;
		float MaxWobble = 0.0f;
		float LastKickAt = 0.0f;
		FName AssetAtStart;
		int32 AssetChanges = 0;
		FName LastAsset;
		// limp
		FVector WalkStart = FVector::ZeroVector;
		bool bWalkSent = false;
		float MaxLimp = 0.0f;
		// patch
		bool bPatchSent = false;
		bool bPatchSeen = false;
		bool bPatchPosed = false;
		// crawl
		int32 Blows = 0;
		float LastBlowAt = -10.0f;
		bool bCrawlSeen = false;
		bool bCrawlPlayed = false;
		bool bRagdollWhileCrawling = false;
		// dismember
		bool bKillSent = false;
		int32 MaxPiecesShown = 0;
		int32 PiecesAtRest = 0;
		int32 BloodHitsAtStart = 0;
		int32 CloseUpsTaken = 0;
		// The close-ups clear the plants round the pieces and the corpse and bring a fill light along (else plants
		// stood in front of the cut or shaded it black).
		bool bCloseSetUp = false;
		TArray<TWeakObjectPtr<UPrimitiveComponent>> CloseHidden;
		TWeakObjectPtr<APointLight> CloseLight;
	};

	/** The real bodies with their clothes instead of the bare mannequins (the pawn actors are made again). */
	void HurtSmokeRealBodies(bool bOn)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Pawns.MetaHumans")))
		{
			Var->Set(bOn ? 1 : 0, ECVF_SetByCode);
		}
		// Noon while they are on: the cut body and its pieces are looked at in the pictures.
		if (IConsoleVariable* Hour = IConsoleManager::Get().FindConsoleVariable(TEXT("Sky.Hour")))
		{
			Hour->Set(bOn ? 12.0f : -1.0f, ECVF_SetByCode);
		}
	}

	int32 HurtSmokePart(const colony::Sim& Sim, const colony::ThingView& View, const char* Label)
	{
		const std::vector<std::string> Labels = Sim.health().partLabels(View.def);
		for (size_t i = 0; i < Labels.size(); ++i)
		{
			if (Labels[i] == Label)
			{
				return static_cast<int32>(i);
			}
		}
		return -1;
	}
}

void USmokeTestSubsystem::BuildHurtPhases()
{
	const TSharedRef<FHurtSmokeState> State = MakeShared<FHurtSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Pawns = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto ActorOf = [=]() -> ASimPawnActor* { return Pawns() ? Pawns()->FindPawn(State->Pawn) : nullptr; };
	const auto View = [=, this]()
	{
		if (const ASimPawnActor* Actor = ActorOf())
		{
			const FVector P = Actor->GetActorLocation();
			const FVector Eye = P + FVector(330.0, 260.0, 190.0);
			ViewFrom(Eye, (P + FVector(0.0, 0.0, 85.0) - Eye).Rotation());
		}
	};
	const auto Damage = [=](const char* Type, float Amount, const char* PartLabel)
	{
		USimSubsystem* B = Bridge();
		const colony::ThingView* Now = B && B->GetSim() ? B->GetCurrentSnapshot().find(State->Pawn) : nullptr;
		if (!Now)
		{
			return false;
		}
		const int32 Part = HurtSmokePart(*B->GetSim(), *Now, PartLabel);
		if (Part < 0)
		{
			return false;
		}
		colony::DamagePawnCommand Command;
		Command.pawn = State->Pawn;
		Command.damageType = Type;
		Command.amount = Amount;
		Command.part = Part;
		B->Submit(Command);
		return true;
	};

	{
		FPhase Phase;
		Phase.Name = TEXT("setup");
		Phase.Setup = [=, this]()
		{
			USimSubsystem* B = Bridge();
			if (!B || !Pawns())
			{
				Fail(TEXT("setup: the sim or pawn subsystem did not start"));
				return;
			}
			FSimStartOptions Options;
			Options.Seed = 42;
			Options.MapSize = 128;
			Options.bFlatMap = true;
			Options.Scenario = TEXT("Crashlanded");
			B->SetWorldOrigin(HurtSmokeOrigin);
			if (!B->StartSim(Options))
			{
				Fail(TEXT("setup: the game's data did not load"));
				return;
			}
			B->SetSpeed(1);
			for (const colony::ThingView& Thing : B->GetCurrentSnapshot().things)
			{
				// Not the protagonist: plot armour means no bleeding to patch, no downing and no dismemberment.
				const colony::Health* Health = B->GetSim()->things().registry().try_get<colony::Health>(Thing.id);
				if (Thing.category == colony::ThingCategory::Pawn && B->GetSim()->colonistOf(Thing.id) && Health && !Health->protagonist)
				{
					State->Pawn = Thing.id;
					break;
				}
			}
			if (State->Pawn == colony::kNoEntity)
			{
				Fail(TEXT("setup: no colonist besides the protagonist"));
				return;
			}
			B->Submit(colony::DraftCommand{State->Pawn, true});
		};
		Phase.WarmupSeconds = 3.0f;
		Phase.MeasureSeconds = 0.5f;
		Phase.OnFrame = View;
		Phase.Verify = [=, this]()
		{
			const ASimPawnActor* Actor = ActorOf();
			if (!Actor || !Actor->GetAnim())
			{
				Fail(TEXT("setup: the colonist has no pawn actor or no animation"));
				return;
			}
			const colony::Sim* Sim = Bridge() ? Bridge()->GetSim() : nullptr;
			if (Sim && Sim->health().zoneNames(State->Pawn).size() != 6)
			{
				Fail(FString::Printf(TEXT("setup: the colonist's body has %d zones, not 6"), static_cast<int32>(Sim->health().zoneNames(State->Pawn).size())));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// A hit on each zone in turn, 1.2 s apart, from the front, the back and the sides.
		FPhase Phase;
		Phase.Name = TEXT("wobble");
		Phase.Setup = [=]()
		{
			if (const ASimPawnActor* Actor = ActorOf(); Actor && Actor->GetAnim())
			{
				State->AssetAtStart = Actor->GetAnim()->GetStats().PlayingAsset;
				State->LastAsset = State->AssetAtStart;
			}
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 9.5f;
		Phase.OnFrame = [=, this]()
		{
			View();
			USimSubsystem* B = Bridge();
			ASimPawnActor* Actor = ActorOf();
			if (!B || !Actor || !Actor->GetAnim())
			{
				return;
			}
			if (State->KicksSent < 6 && PhaseTime >= 0.5f + 1.2f * State->KicksSent)
			{
				// Where on the body (up as a share of its height, right in cm), and the way the blow travels in the actor's frame.
				static const float Up[6] = {0.93f, 0.68f, 0.68f, 0.68f, 0.25f, 0.25f};
				static const float Right[6] = {0.0f, 0.0f, -26.0f, 26.0f, -10.0f, 10.0f};
				static const FVector Way[6] = {FVector(-1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0), FVector(0.0, 1.0, 0.0), FVector(1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0), FVector(0.0, -1.0, 0.0)};
				const int32 i = State->KicksSent;
				const float Height = 180.0f * Actor->GetBodyScale();
				const FTransform Frame = Actor->GetActorTransform();
				Actor->KickZone(*B, -1, Frame.TransformPositionNoScale(FVector(0.0, Right[i] * Actor->GetBodyScale(), Up[i] * Height)), Frame.TransformVectorNoScale(Way[i]), 1.0f);
				State->ZonesSeen |= 1u << static_cast<uint32>(Actor->GetLastKickZone());
				++State->KicksSent;
				State->LastKickAt = PhaseTime;
			}
			const FSimPawnAnimStats& Stats = Actor->GetAnim()->GetStats();
			State->MaxWobble = FMath::Max(State->MaxWobble, Stats.WobbleDegrees);
			if (Stats.PlayingAsset != State->LastAsset)
			{
				State->LastAsset = Stats.PlayingAsset;
				++State->AssetChanges;
			}
			UE_LOG(LogTemp, Log, TEXT("HURTTRACE wobble t=%.3f kicks=%d wobble=%.2f asset=%s"), PhaseTime, Stats.Kicks, Stats.WobbleDegrees, *Stats.PlayingAsset.ToString());
		};
		Phase.Verify = [=, this]()
		{
			const ASimPawnActor* Actor = ActorOf();
			if (!Actor || !Actor->GetAnim())
			{
				Fail(TEXT("wobble: no pawn actor"));
				return;
			}
			const FSimPawnAnimStats& Stats = Actor->GetAnim()->GetStats();
			PhaseNumbers.Add(TEXT("kicksSent"), State->KicksSent);
			PhaseNumbers.Add(TEXT("kicksTaken"), Stats.Kicks);
			PhaseNumbers.Add(TEXT("maxWobbleDegrees"), State->MaxWobble);
			PhaseNumbers.Add(TEXT("wobbleAtEndDegrees"), Stats.WobbleDegrees);
			PhaseNumbers.Add(TEXT("animationChanges"), State->AssetChanges);
			if (State->KicksSent != 6 || Stats.Kicks != 6)
			{
				Fail(FString::Printf(TEXT("wobble: %d hits sent, the animation took %d"), State->KicksSent, Stats.Kicks));
			}
			if (State->ZonesSeen != 0x3f)
			{
				Fail(FString::Printf(TEXT("wobble: the six hits landed on zones 0x%x, not one on each"), State->ZonesSeen));
			}
			if (State->MaxWobble < 4.0f || State->MaxWobble > 36.0f)
			{
				Fail(FString::Printf(TEXT("wobble: the struck zones bent %.1f degrees at most (4 to 36 expected)"), State->MaxWobble));
			}
			if (Stats.WobbleDegrees > 1.0f)
			{
				Fail(FString::Printf(TEXT("wobble: still %.1f degrees off %.1f s after the last hit"), Stats.WobbleDegrees, PhaseTime - State->LastKickAt));
			}
			Notes.Add(FString::Printf(TEXT("wobble: 6 hits, one a zone; bent %.1f degrees at most, %.2f at the end; the animation underneath changed %d times (idle variety only)"),
				State->MaxWobble, Stats.WobbleDegrees, State->AssetChanges));
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// A bruised left leg (30 of its 75 hit points), then a walk of 14 m.
		FPhase Phase;
		Phase.Name = TEXT("limp");
		Phase.Setup = [=, this]()
		{
			if (!Damage("Blunt", 30.0f, "left leg"))
			{
				Fail(TEXT("limp: could not hurt the left leg"));
			}
			if (const ASimPawnActor* Actor = ActorOf())
			{
				State->WalkStart = Actor->GetActorLocation();
			}
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 9.0f;
		Phase.OnFrame = [=, this]()
		{
			View();
			USimSubsystem* B = Bridge();
			const ASimPawnActor* Actor = ActorOf();
			const colony::ThingView* Now = B ? B->GetCurrentSnapshot().find(State->Pawn) : nullptr;
			if (!B || !Actor || !Actor->GetAnim() || !Now)
			{
				return;
			}
			if (!State->bWalkSent && PhaseTime > 0.6f)
			{
				State->bWalkSent = true;
				colony::MoveOrderCommand Order;
				Order.pawn = State->Pawn;
				Order.cell = colony::CellPos{static_cast<int>(Now->x) + 14, static_cast<int>(Now->y), 0};
				B->Submit(Order);
			}
			const FSimPawnAnimStats& Stats = Actor->GetAnim()->GetStats();
			State->MaxLimp = FMath::Max(State->MaxLimp, Stats.LimpNow);
			UE_LOG(LogTemp, Log, TEXT("HURTTRACE limp t=%.3f speed=%.0f limp=%.2f"), PhaseTime, Actor->GetGroundSpeed(), Stats.LimpNow);
		};
		Phase.Verify = [=, this]()
		{
			const ASimPawnActor* Actor = ActorOf();
			const colony::Sim* Sim = Bridge() ? Bridge()->GetSim() : nullptr;
			const double Walked = Actor ? FVector::Dist2D(Actor->GetActorLocation(), State->WalkStart) : 0.0;
			float LegHealth = 1.0f;
			if (Sim)
			{
				const std::vector<std::string> Names = Sim->health().zoneNames(State->Pawn);
				for (size_t i = 0; i < Names.size(); ++i)
				{
					if (Names[i] == "Leg")
					{
						LegHealth = Sim->health().zoneHealth(State->Pawn, static_cast<int>(i));
						break;
					}
				}
			}
			PhaseNumbers.Add(TEXT("walkedCm"), Walked);
			PhaseNumbers.Add(TEXT("leftLegHealth"), LegHealth);
			PhaseNumbers.Add(TEXT("maxLimp"), State->MaxLimp);
			if (LegHealth > 0.8f)
			{
				Fail(FString::Printf(TEXT("limp: the left leg is at %.0f%% after 30 blunt damage"), LegHealth * 100.0f));
			}
			if (Walked < 300.0)
			{
				Fail(FString::Printf(TEXT("limp: the colonist walked only %.0f cm"), Walked));
			}
			else if (State->MaxLimp < 0.12f)
			{
				Fail(FString::Printf(TEXT("limp: the body leaned over the hurt leg by %.2f at most (0 none, 1 full)"), State->MaxLimp));
			}
			Notes.Add(FString::Printf(TEXT("limp: left leg at %.0f%%, walked %.0f cm, leaned over it up to %.2f of the full limp"), LegHealth * 100.0f, Walked, State->MaxLimp));
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// A cut that takes the torso to 70% bleeds (a zone bleeds at 75% or less); the colonist patches it (on the order,
		// or by itself first).
		FPhase Phase;
		Phase.Name = TEXT("patch");
		Phase.Setup = [=, this]()
		{
			if (!Damage("Cut", 30.0f, "torso"))
			{
				Fail(TEXT("patch: could not cut the torso"));
			}
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 7.0f;
		Phase.OnFrame = [=, this]()
		{
			View();
			USimSubsystem* B = Bridge();
			const ASimPawnActor* Actor = ActorOf();
			const colony::ThingView* Now = B ? B->GetCurrentSnapshot().find(State->Pawn) : nullptr;
			if (!B || !Actor || !Now)
			{
				return;
			}
			if (!State->bPatchSent && PhaseTime > 0.5f && Now->patchZone < 0 && !State->bPatchSeen)
			{
				State->bPatchSent = true;
				B->Submit(colony::PatchCommand{State->Pawn});
			}
			State->bPatchSeen = State->bPatchSeen || Now->patchZone >= 0;
			State->bPatchPosed = State->bPatchPosed || Actor->IsPatchPosed();
		};
		Phase.Verify = [=, this]()
		{
			if (!State->bPatchSeen)
			{
				Fail(TEXT("patch: the colonist never patched its cut (the sim showed no patching)"));
			}
			else if (!State->bPatchPosed)
			{
				Fail(TEXT("patch: the sim patched, the body never bent over the wound"));
			}
			if (const ASimPawnActor* Actor = ActorOf(); Actor && Actor->IsPatchPosed())
			{
				Fail(TEXT("patch: still bent over the wound 7 s on"));
			}
			Notes.Add(FString::Printf(TEXT("patch: patched %s, the body bent over the wound"), State->bPatchSent ? TEXT("on the order") : TEXT("by itself")));
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// Blows to the torso until it is under a quarter: downed and awake, so it crawls instead of going limp.
		FPhase Phase;
		Phase.Name = TEXT("crawl");
		Phase.Setup = []() {};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 8.0f;
		Phase.ScreenshotAt = 6.5f;  // after the blows that down it: the picture shows the crawl
		Phase.OnFrame = [=, this]()
		{
			View();
			USimSubsystem* B = Bridge();
			const ASimPawnActor* Actor = ActorOf();
			const colony::ThingView* Now = B ? B->GetCurrentSnapshot().find(State->Pawn) : nullptr;
			if (!B || !Actor || !Now)
			{
				return;
			}
			if (!Now->downed && !Now->dead && State->Blows < 8 && PhaseTime - State->LastBlowAt > 0.5f)
			{
				Damage("Blunt", 20.0f, "torso");
				++State->Blows;
				State->LastBlowAt = PhaseTime;
			}
			if (Now->crawling && Now->downed && !Now->dead)
			{
				State->bCrawlSeen = true;
				State->bCrawlPlayed = State->bCrawlPlayed || Actor->IsCrawling();
				// A frame or two can pass before the fight subsystem sees the pawn again.
				State->bRagdollWhileCrawling = State->bRagdollWhileCrawling || (Actor->IsRagdoll() && PhaseTime - State->LastBlowAt > 1.0f);
			}
		};
		Phase.Verify = [=, this]()
		{
			const ASimPawnActor* Actor = ActorOf();
			PhaseNumbers.Add(TEXT("blows"), State->Blows);
			if (!State->bCrawlSeen)
			{
				Fail(FString::Printf(TEXT("crawl: %d blows of 20 to the torso never left the colonist downed and crawling"), State->Blows));
			}
			else
			{
				if (State->bRagdollWhileCrawling)
				{
					Fail(TEXT("crawl: the crawling colonist was a ragdoll"));
				}
				if (!State->bCrawlPlayed)
				{
					// The clips are Mixamo's, downloaded on the PC (not in Git): without them the body just lies in its idle.
					Notes.Add(TEXT("crawl: NO CRAWL CLIP under /Game/Rim3D/Generated/Animations/Mixamo/Crawling: the body does not show the crawl"));
				}
			}
			Notes.Add(FString::Printf(TEXT("crawl: downed by the torso after %d blows; crawl clip %s (played %d times)"), State->Blows, State->bCrawlPlayed ? TEXT("playing") : TEXT("missing"),
				Actor ? Actor->GetCrawlsPlayed() : 0));
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// Part F: a blow to the torso far over what it has left kills, and the body comes apart: the head and all four
		// limbs are folded away on the corpse and lie about as five pieces.
		FPhase Phase;
		Phase.Name = TEXT("dismember");
		// On the real, dressed bodies: the sleeve and the trouser leg must come off with the limb.
		Phase.Setup = []() { HurtSmokeRealBodies(true); };
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 21.0f;
		Phase.ScreenshotAt = 7.5f;  // after the cut (2.5 s in) and the pieces' landing: the picture shows the result
		Phase.OnFrame = [=, this]()
		{
			View();
			// From 8 s on, a second on each piece from close by, then the corpse: dismember_close<N>.png, to judge the look.
			if (PhaseTime >= 8.0f)
			{
				TArray<const AActor*> Close;
				for (TActorIterator<ASeveredPieceActor> It(GetWorld()); It; ++It)
				{
					Close.Add(*It);
				}
				Close.Sort([](const AActor& A, const AActor& B) { return A.GetName() < B.GetName(); });
				Close.Add(ActorOf());
				if (!State->bCloseSetUp)
				{
					State->bCloseSetUp = true;
					const UTerrainSubsystem* Terrain = GetWorld()->GetSubsystem<UTerrainSubsystem>();
					if (AActor* Ground = Terrain ? Terrain->GetTerrainActor() : nullptr)
					{
						TInlineComponentArray<UInstancedStaticMeshComponent*> Plants;
						Ground->GetComponents(Plants);
						for (UInstancedStaticMeshComponent* Kind : Plants)
						{
							for (const AActor* Near : Close)
							{
								if (Kind && Near && Kind->IsVisible() && Kind->GetInstancesOverlappingSphere(Near->GetActorLocation(), 160.0f, true).Num() > 0)
								{
									Kind->SetVisibility(false);
									State->CloseHidden.Add(Kind);
									break;
								}
							}
						}
					}
					FActorSpawnParameters LightParams;
					LightParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					if (APointLight* Fill = GetWorld()->SpawnActor<APointLight>(APointLight::StaticClass(), FTransform::Identity, LightParams))
					{
						Fill->SetMobility(EComponentMobility::Movable);
						Fill->PointLightComponent->SetIntensityUnits(ELightUnits::Candelas);
						Fill->PointLightComponent->SetIntensity(600.0f);
						Fill->PointLightComponent->SetAttenuationRadius(250.0f);
						Fill->PointLightComponent->SetCastShadows(false);
						State->CloseLight = Fill;
					}
					UE_LOG(LogTemp, Log, TEXT("Smoke hurt: close-ups hide %d plant groups and bring a fill light"), State->CloseHidden.Num());
				}
				// Each from two opposite sides (a head has a face on one of them).
				const int32 Shot = FMath::FloorToInt(PhaseTime - 8.0f);
				const int32 Which = Shot / 2;
				if (Close.IsValidIndex(Which) && Close[Which])
				{
					const bool bCorpse = Which == Close.Num() - 1;
					const double Side = Shot % 2 == 0 ? 1.0 : -1.0;
					FVector P = Close[Which]->GetActorLocation() + FVector(0.0, 0.0, bCorpse ? 20.0 : 0.0);
					FVector Eye = P + (bCorpse ? FVector(70.0 * Side, 50.0 * Side, 260.0) : FVector(75.0 * Side, 60.0 * Side, 65.0));
					// A piece's first shot looks into its cut from 40 cm, a little off its line, to judge the stump (skin
					// rim, fat, muscle, bone).
					const ASeveredPieceActor* PieceShown = Cast<ASeveredPieceActor>(Close[Which]);
					if (!bCorpse && Side > 0.0 && PieceShown && PieceShown->GetCutEnd())
					{
						P = PieceShown->GetCutEnd()->GetComponentLocation();
						// Keep the cut's own sideways direction and only lift the eye a little: a head lies on its side with
						// its neck facing along the ground, and clamping straight up put the camera in its hair.
						const FVector CutOut = PieceShown->GetCutEnd()->GetUpVector();
						FVector Across(CutOut.X, CutOut.Y, 0.0);
						if (Across.SizeSquared() < 0.04)
						{
							Across = FVector(0.8, 0.6, 0.0);
						}
						const double Lift = FMath::Max(CutOut.Z, 0.3);
						const FVector Look = Across.GetSafeNormal() * FMath::Sqrt(1.0 - Lift * Lift) + FVector(0.0, 0.0, Lift);
						Eye = P + Look * 40.0 + FVector(4.0, 3.0, 2.0);
					}
					ViewFrom(Eye, (P - Eye).Rotation());
					if (APointLight* Fill = State->CloseLight.Get())
					{
						// Beside and above the camera, so the cut is lit from the viewer's side.
						Fill->SetActorLocation(Eye + FVector(0.0, 0.0, 25.0));
					}
					if (State->CloseUpsTaken == Shot && PhaseTime - 8.0f - Shot > 0.5f)
					{
						++State->CloseUpsTaken;
						FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke") / TEXT("hurt") / FString::Printf(TEXT("dismember_close%d%s.png"), Which, Shot % 2 == 0 ? TEXT("a") : TEXT("b")), false, false);
					}
				}
				else if (Which >= Close.Num() && (State->CloseLight.IsValid() || !State->CloseHidden.IsEmpty()))
				{
					// All taken: the plants and the light as they were.
					for (const TWeakObjectPtr<UPrimitiveComponent>& Hidden : State->CloseHidden)
					{
						if (UPrimitiveComponent* Kind = Hidden.Get())
						{
							Kind->SetVisibility(true);
						}
					}
					State->CloseHidden.Reset();
					if (APointLight* Fill = State->CloseLight.Get())
					{
						Fill->Destroy();
					}
					State->CloseLight.Reset();
				}
			}
			if (!State->bKillSent && PhaseTime > 2.5f)
			{
				if (const UBloodSubsystem* Blood = GetWorld()->GetSubsystem<UBloodSubsystem>())
				{
					State->BloodHitsAtStart = Blood->GetHits();
				}
				State->bKillSent = Damage("Blunt", 400.0f, "torso");
			}
			if (const UFightSubsystem* Fight = GetWorld()->GetSubsystem<UFightSubsystem>())
			{
				State->MaxPiecesShown = FMath::Max(State->MaxPiecesShown, Fight->GetSeveredPiecesShown());
			}
		};
		Phase.Verify = [=, this]()
		{
			USimSubsystem* B = Bridge();
			const ASimPawnActor* Actor = ActorOf();
			const colony::ThingView* Now = B ? B->GetCurrentSnapshot().find(State->Pawn) : nullptr;
			const uint16 All = colony::kSeveredHead | colony::kSeveredLeftArm | colony::kSeveredRightArm | colony::kSeveredLeftLeg | colony::kSeveredRightLeg;
			const int32 SimPieces = B ? static_cast<int32>(B->GetCurrentSnapshot().severedPieces.size()) : 0;
			PhaseNumbers.Add(TEXT("sim_pieces"), SimPieces);
			PhaseNumbers.Add(TEXT("pieces_shown"), State->MaxPiecesShown);
			PhaseNumbers.Add(TEXT("severed_sim"), Now ? Now->severed : 0);
			PhaseNumbers.Add(TEXT("severed_shown"), Actor ? Actor->GetSeveredShown() : 0);
			PhaseNumbers.Add(TEXT("stump_caps"), Actor ? Actor->GetSeveredCaps() : 0);
			PhaseNumbers.Add(TEXT("real_body"), Actor && Actor->GetMetaHuman() ? 1 : 0);
			if (const UBloodSubsystem* Blood = GetWorld()->GetSubsystem<UBloodSubsystem>())
			{
				PhaseNumbers.Add(TEXT("blood_hits"), Blood->GetHits() - State->BloodHitsAtStart);
			}
			HurtSmokeRealBodies(false);
			if (!Now || !Now->dead)
			{
				Fail(TEXT("dismember: 400 blunt damage to the torso did not kill the colonist"));
				return;
			}
			if (Now->severed != All || SimPieces != 5)
			{
				Fail(FString::Printf(TEXT("dismember: the sim took off 0x%x (0x%x expected) and left %d pieces (5 expected)"), Now->severed, All, SimPieces));
			}
			if (!Actor || Actor->GetSeveredShown() != Now->severed)
			{
				Fail(FString::Printf(TEXT("dismember: the body shows 0x%x cut off, the sim says 0x%x"), Actor ? Actor->GetSeveredShown() : 0, Now->severed));
			}
			if (State->MaxPiecesShown != SimPieces)
			{
				Fail(FString::Printf(TEXT("dismember: %d pieces drawn for the sim's %d"), State->MaxPiecesShown, SimPieces));
			}
			int32 AtRest = 0;
			int32 Meshes = 0;
			for (TActorIterator<ASeveredPieceActor> It(GetWorld()); It; ++It)
			{
				AtRest += It->IsAtRest() ? 1 : 0;
				Meshes += It->GetMeshCount();
			}
			PhaseNumbers.Add(TEXT("pieces_at_rest"), AtRest);
			PhaseNumbers.Add(TEXT("piece_meshes"), Meshes);
			if (Actor && Actor->GetSeveredCaps() != 5)
			{
				Fail(FString::Printf(TEXT("dismember: %d of 5 stumps got a flesh cap"), Actor->GetSeveredCaps()));
			}
			if (Actor && Actor->GetMetaHuman() && Meshes < 2 * State->MaxPiecesShown)
			{
				Fail(FString::Printf(TEXT("dismember: the %d pieces of a dressed body have only %d meshes (skin and clothes expected)"), State->MaxPiecesShown, Meshes));
			}
			if (AtRest != State->MaxPiecesShown)
			{
				Fail(FString::Printf(TEXT("dismember: %d of %d pieces had come to rest after 5 s"), AtRest, State->MaxPiecesShown));
			}
			Notes.Add(FString::Printf(TEXT("dismember: head and four limbs off a %s body; %d pieces thrown, %d meshes in them, %d at rest"), Actor && Actor->GetMetaHuman() ? TEXT("real, dressed") : TEXT("mannequin (NO REAL BODY: clothes not checked)"),
				State->MaxPiecesShown, Meshes, AtRest));
		};
		Phases.Add(MoveTemp(Phase));
	}
}
