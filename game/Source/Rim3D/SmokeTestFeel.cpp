// Smoke test `feel` (task U21): the aim-hold camera blend, the gun's kick on the camera, shake and blur from sim events, the dodge roll,
// grenades and blasts with debris, and the feel settings, in the game, offscreen.
#include "SmokeTest.h"

#include "Components/StaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"

#include "BlastFxSubsystem.h"
#include "FightSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "ShooterFeelSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Explosives.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

#include <string>

namespace
{
	const FVector FeelSmokeOrigin(-1700000.0, 0.0, 0.0);

	struct FFeelSmokeState
	{
		colony::Entity Me = colony::kNoEntity;
		FVector2D Start = FVector2D::ZeroVector;
		float MaxKickPitch = 0.0f;
		float MaxFeelPitch = 0.0f;
		float MinFeelPitch = 0.0f;
		float MaxAimDirectionError = 0.0f;
		float MaxShake = 0.0f;
		float MaxBlur = 0.0f;
		float MaxRollAngle = 0.0f;
		float MaxFlash = 0.0f;
		int32 MaxBlast = 0;
		int32 MaxSmoke = 0;
		int32 BlastsAtStart = 0;
		float BlastX = 0.0f;
		float BlastY = 0.0f;
		int32 ScorchesAtStart = 0;
		int32 MaxGrenades = 0;
		int32 MaxDebris = 0;
		int32 ShotsAtStart = 0;
		int32 ShakesAtStart = 0;
		int32 ExplosionsAtStart = 0;
		bool bRollRefused = false;
		bool bRollSent = false;
		bool bBodyHiddenAimed = false;
		bool bThrown = false;
		FVector RollFrom = FVector::ZeroVector;
		double RollMoved = 0.0;
		double MaxCameraTrail = 0.0;
		double CameraTrailAtEnd = 0.0;

		void Reset()
		{
			MaxKickPitch = MaxFeelPitch = MinFeelPitch = MaxAimDirectionError = MaxShake = MaxBlur = MaxRollAngle = MaxFlash = 0.0f;
			MaxBlast = MaxGrenades = MaxDebris = MaxSmoke = 0;
			MaxCameraTrail = CameraTrailAtEnd = 0.0;
		}
	};

	void FeelSmokeSet(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByCode);
		}
	}
}

void USmokeTestSubsystem::BuildFeelPhases()
{
	const TSharedRef<FFeelSmokeState> State = MakeShared<FFeelSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Prot = [this]() { return GetWorld()->GetSubsystem<UProtagonistSubsystem>(); };
	const auto Fight = [this]() { return GetWorld()->GetSubsystem<UFightSubsystem>(); };
	const auto Feel = [this]() { return GetWorld()->GetSubsystem<UShooterFeelSubsystem>(); };
	const auto Pawns = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };
	const auto MyView = [=, this]() -> const colony::ThingView* { return Bridge()->GetCurrentSnapshot().find(State->Me); };

	// What the camera does, every frame.
	const auto Watch = [=, this]()
	{
		UProtagonistSubsystem* P = Prot();
		UShooterFeelSubsystem* F = Feel();
		if (!P || !P->GetBody() || !F)
		{
			return;
		}
		const AProtagonistPawn* Body = P->GetBody();
		State->MaxKickPitch = FMath::Max(State->MaxKickPitch, F->GetKickPitch());
		const float Pitch = Body->GetFeelRotation().Pitch;
		State->MaxFeelPitch = FMath::Max(State->MaxFeelPitch, Pitch);
		State->MinFeelPitch = FMath::Min(State->MinFeelPitch, Pitch);
		// The aim direction the shots use is the view's own, never the kicked one.
		const FRotator Aim = Body->GetViewDirection().Rotation();
		State->MaxAimDirectionError = FMath::Max(State->MaxAimDirectionError, FMath::Abs(FRotator::NormalizeAxis(Aim.Pitch - Body->GetPitch())) + FMath::Abs(FRotator::NormalizeAxis(Aim.Yaw - Body->GetYaw())));
		State->MaxShake = FMath::Max(State->MaxShake, F->GetLargestShakeSeen());
		State->MaxBlur = FMath::Max(State->MaxBlur, Body->GetMotionBlurAmount());
		State->MaxFlash = FMath::Max(State->MaxFlash, F->GetFlashIntensity());
		if (const UBlastFxSubsystem* Fx = GetWorld()->GetSubsystem<UBlastFxSubsystem>())
		{
			State->MaxFlash = FMath::Max(State->MaxFlash, Fx->GetFlashCandelas());
			State->MaxBlast = FMath::Max(State->MaxBlast, Fx->GetSpritesAlive());
			State->MaxSmoke = FMath::Max(State->MaxSmoke, Fx->GetSmokeAlive());
		}
		State->MaxCameraTrail = FMath::Max(State->MaxCameraTrail, Body->GetCameraTrail());
		State->CameraTrailAtEnd = Body->GetCameraTrail();
		State->MaxBlast = FMath::Max(State->MaxBlast, F->GetBlastParticles());
		State->MaxGrenades = FMath::Max(State->MaxGrenades, F->GetGrenadesDrawn());
		State->MaxDebris = FMath::Max(State->MaxDebris, F->GetDebrisAlive());
		if (const ASimPawnActor* Actor = Pawns() ? Pawns()->FindPawn(State->Me) : nullptr)
		{
			State->MaxRollAngle = FMath::Max(State->MaxRollAngle, Actor->GetRollAngle());
		}
	};

	// 1. The Crashlanded start, the protagonist under direct control in third person with an assault rifle, looking east.
	FPhase Setup;
	Setup.Name = TEXT("feel_setup");
	Setup.WarmupSeconds = 4.0f;
	Setup.MeasureSeconds = 1.0f;
	Setup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!B || !P || !Fight() || !Feel() || !Pawns())
		{
			Fail(TEXT("USimSubsystem, UProtagonistSubsystem, UFightSubsystem, UShooterFeelSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		FeelSmokeSet(TEXT("Sky.Hour"), 11.0f);
		FeelSmokeSet(TEXT("Sky.DayOfYear"), 22.5f);
		FeelSmokeSet(TEXT("Feel.Shake"), 1.0f);
		FeelSmokeSet(TEXT("Feel.Blur"), 1.0f);
		FeelSmokeSet(TEXT("Feel.Kick"), 1.0f);
		FeelSmokeSet(TEXT("Feel.AimView"), 0.0f);
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 128;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(FeelSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(1);
		State->Me = P->FindProtagonist();
		const colony::ThingView* Me = MyView();
		if (!Me)
		{
			Fail(TEXT("no protagonist"));
			return;
		}
		State->Start = FVector2D(Me->x, Me->y);
		B->Submit(colony::SpawnThingCommand{"Gun_AssaultRifle", colony::CellPos{FMath::FloorToInt32(Me->x) + 2, FMath::FloorToInt32(Me->y), 0}, 1});
		for (int32 I = 0; I < 3; ++I)
		{
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{FMath::FloorToInt32(Me->x) + 14, FMath::FloorToInt32(Me->y) + (I - 1) * 4, 0}});
		}
		B->StepOnce();
		if (!P->EnterDirectMode(State->Me))
		{
			Fail(TEXT("could not take control of the protagonist"));
			return;
		}
		Fight()->EquipNearest();
		P->GetBody()->SetFirstPerson(false);
		P->GetBody()->Look(FRotator::NormalizeAxis(-P->GetBody()->GetYaw()), -4.0f - P->GetBody()->GetPitch());
		P->GetBody()->SnapCamera();
	};
	Setup.OnFrame = Watch;
	Setup.Verify = [=, this]()
	{
		const colony::ThingView* Me = MyView();
		if (!Me || !Me->weapon.valid() || !Bridge()->GetDefs()->get(Me->weapon).has("verbs"))
		{
			Fail(TEXT("feel_setup: the protagonist holds no gun"));
		}
		if (State->MaxFeelPitch > 0.01f || State->MinFeelPitch < -0.01f)
		{
			Fail(FString::Printf(TEXT("feel_setup: the camera is nudged (%.3f to %.3f degrees) with nothing going on"), State->MinFeelPitch, State->MaxFeelPitch));
		}
		if (Prot()->GetBody()->GetFieldOfView() < 79.0f)
		{
			Fail(TEXT("feel_setup: the field of view is not the normal 80"));
		}
	};
	Phases.Add(MoveTemp(Setup));

	// 2. Holding aim: the view blends to over the shoulder (closer, zoomed in), and back when it is let go.
	FPhase Aim;
	Aim.Name = TEXT("feel_aim_shoulder");
	Aim.WarmupSeconds = 1.5f;
	Aim.MeasureSeconds = 1.5f;
	Aim.Setup = [=, this]()
	{
		State->Reset();
		Prot()->GetBody()->SetScriptedAim(true);
	};
	Aim.OnFrame = Watch;
	Aim.Verify = [=, this]()
	{
		AProtagonistPawn* Body = Prot()->GetBody();
		Notes.Add(FString::Printf(TEXT("feel_aim_shoulder: blend %.2f, field of view %.1f, arm %.0f of %.0f cm"), Body->GetAimBlend(), Body->GetFieldOfView(), Body->GetEffectiveArmLength(), Body->GetArmLength()));
		if (Body->GetAimBlend() < 0.99f || Body->GetFieldOfView() > UShooterFeelSubsystem::GetAimFov() + 1.0f)
		{
			Fail(FString::Printf(TEXT("feel_aim_shoulder: aim blend %.2f and field of view %.1f after holding aim for three seconds"), Body->GetAimBlend(), Body->GetFieldOfView()));
		}
		if (Body->GetEffectiveArmLength() > 125.0f || Body->IsBodyHiddenInView())
		{
			Fail(FString::Printf(TEXT("feel_aim_shoulder: the camera is %.0f cm back and the body hidden %d (over the shoulder: close and still seen)"), Body->GetEffectiveArmLength(), Body->IsBodyHiddenInView()));
		}
	};
	Phases.Add(MoveTemp(Aim));

	FPhase AimFirst;
	AimFirst.Name = TEXT("feel_aim_no_first_person");
	AimFirst.WarmupSeconds = 1.5f;
	AimFirst.MeasureSeconds = 1.5f;
	AimFirst.Setup = [=, this]()
	{
		State->Reset();
		FeelSmokeSet(TEXT("Feel.AimView"), 1.0f);
	};
	AimFirst.OnFrame = Watch;
	AimFirst.Verify = [=, this]()
	{
		AProtagonistPawn* Body = Prot()->GetBody();
		// Third person only (sneed 2026-11-22): an old "aim in first person" setting is ignored.
		if (Body->IsBodyHiddenInView() || Body->GetEffectiveArmLength() < 60.0f)
		{
			Fail(FString::Printf(TEXT("feel_aim_no_first_person: body hidden %d and the camera %.0f cm back (should stay over the shoulder)"), Body->IsBodyHiddenInView(), Body->GetEffectiveArmLength()));
		}
		const USimPawnSubsystem* P = Pawns();
		const ASimPawnActor* Actor = P ? P->FindPawn(State->Me) : nullptr;
		if (!Actor || Actor->IsBodyHidden())
		{
			Fail(TEXT("feel_aim_no_first_person: the colonist's body is hidden"));
		}
	};
	Phases.Add(MoveTemp(AimFirst));

	FPhase Release;
	Release.Name = TEXT("feel_aim_release");
	Release.WarmupSeconds = 1.5f;
	Release.MeasureSeconds = 1.0f;
	Release.Setup = [=, this]()
	{
		State->Reset();
		FeelSmokeSet(TEXT("Feel.AimView"), 0.0f);
		Prot()->GetBody()->SetScriptedAim(false);
	};
	Release.Verify = [=, this]()
	{
		AProtagonistPawn* Body = Prot()->GetBody();
		if (Body->GetAimBlend() > 0.01f || Body->GetFieldOfView() < 79.0f || FMath::Abs(Body->GetEffectiveArmLength() - Body->GetArmLength()) > 1.0f || Body->IsBodyHiddenInView())
		{
			Fail(FString::Printf(TEXT("feel_aim_release: blend %.2f, field of view %.1f, arm %.0f of %.0f after letting go"), Body->GetAimBlend(), Body->GetFieldOfView(), Body->GetEffectiveArmLength(), Body->GetArmLength()));
		}
	};
	Phases.Add(MoveTemp(Release));

	// 3. A burst: the sim's kick lifts the view (and only the view), a shake rides the shots, and it all settles.
	FPhase Kick;
	Kick.Name = TEXT("feel_kick");
	Kick.WarmupSeconds = 1.0f;
	Kick.MeasureSeconds = 3.0f;
	Kick.Setup = [=, this]()
	{
		State->Reset();
		State->ShotsAtStart = Feel()->GetShotsFelt();
	};
	Kick.OnFrame = [=, this]()
	{
		Watch();
		// A burst in the first second.
		if (PhaseTime < 1.4f)
		{
			Fight()->Fire();
		}
	};
	Kick.Verify = [=, this]()
	{
		UShooterFeelSubsystem* F = Feel();
		const int32 Shots = F->GetShotsFelt() - State->ShotsAtStart;
		Notes.Add(FString::Printf(TEXT("feel_kick: %d shots felt, the view lifted %.2f degrees at most, aim off by %.4f degrees at most, shake up to %.2f degrees, kick now %.3f"), Shots,
			State->MaxFeelPitch, State->MaxAimDirectionError, State->MaxShake, F->GetKickPitch()));
		if (Shots < 2)
		{
			Fail(FString::Printf(TEXT("feel_kick: the camera felt only %d of the player's shots"), Shots));
		}
		if (State->MaxKickPitch < 0.3f || State->MaxFeelPitch < 0.3f)
		{
			Fail(FString::Printf(TEXT("feel_kick: the kick reached %.2f degrees and the view %.2f"), State->MaxKickPitch, State->MaxFeelPitch));
		}
		if (State->MaxAimDirectionError > 0.001f)
		{
			Fail(FString::Printf(TEXT("feel_kick: the aim the shots use moved with the kick (%.4f degrees)"), State->MaxAimDirectionError));
		}
		if (State->MaxShake < 0.1f)
		{
			Fail(FString::Printf(TEXT("feel_kick: no camera shake from the shots (%.3f)"), State->MaxShake));
		}
		if (F->GetKickPitch() > 0.05f)
		{
			Fail(FString::Printf(TEXT("feel_kick: the kick has not worn off (%.3f degrees)"), F->GetKickPitch()));
		}
	};
	Phases.Add(MoveTemp(Kick));

	// 4. The same burst with shake, kick and blur at 0 (the settings): the view stays put.
	FPhase Off;
	Off.Name = TEXT("feel_settings_off");
	Off.WarmupSeconds = 1.0f;
	Off.MeasureSeconds = 2.5f;
	Off.Setup = [=, this]()
	{
		State->Reset();
		FeelSmokeSet(TEXT("Feel.Shake"), 0.0f);
		FeelSmokeSet(TEXT("Feel.Kick"), 0.0f);
		FeelSmokeSet(TEXT("Feel.Blur"), 0.0f);
		State->ShotsAtStart = Feel()->GetShotsFelt();
	};
	Off.OnFrame = [=, this]()
	{
		Watch();
		if (PhaseTime < 1.4f)
		{
			Fight()->Fire();
		}
	};
	Off.Verify = [=, this]()
	{
		const int32 Shots = Feel()->GetShotsFelt() - State->ShotsAtStart;
		if (Shots < 2)
		{
			Fail(FString::Printf(TEXT("feel_settings_off: only %d shots were fired"), Shots));
		}
		if (State->MaxFeelPitch > 0.01f || State->MinFeelPitch < -0.01f || State->MaxBlur > 0.001f)
		{
			Fail(FString::Printf(TEXT("feel_settings_off: with shake, kick and blur at 0 the view still moved (%.3f to %.3f degrees, blur %.2f)"), State->MinFeelPitch, State->MaxFeelPitch, State->MaxBlur));
		}
		// The settings are kept in the user settings file.
		UShooterFeelSubsystem::SaveSettings();
		float Saved = -1.0f;
		const bool bFound = GConfig && GConfig->GetFloat(TEXT("Rim3DFeel"), TEXT("Shake"), Saved, GGameUserSettingsIni);
		if (!bFound || Saved != 0.0f)
		{
			Fail(FString::Printf(TEXT("feel_settings_off: the saved shake setting is %.2f (found %d)"), Saved, bFound));
		}
		FeelSmokeSet(TEXT("Feel.Shake"), 1.0f);
		FeelSmokeSet(TEXT("Feel.Kick"), 1.0f);
		FeelSmokeSet(TEXT("Feel.Blur"), 1.0f);
		UShooterFeelSubsystem::SaveSettings();
	};
	Phases.Add(MoveTemp(Off));

	// 5. A dodge roll: the sim moves the pawn four metres in 27 steps, the body tumbles over once in its middle; the camera
	// doesn't duck but glides after the body and settles back on it (Elden Ring, sneed 2026-11-19).
	FPhase Roll;
	Roll.Name = TEXT("feel_roll");
	Roll.WarmupSeconds = 0.5f;
	Roll.MeasureSeconds = 3.0f;
	Roll.Setup = [=, this]()
	{
		State->Reset();
		State->bRollSent = false;
		State->bRollRefused = false;
		State->RollMoved = 0.0;
		AProtagonistPawn* Body = Prot()->GetBody();
		Body->SetFirstPerson(false);
		Body->SnapCamera();
	};
	Roll.OnFrame = [=, this]()
	{
		Watch();
		const colony::ThingView* Me = MyView();
		if (!Me)
		{
			return;
		}
		if (!State->bRollSent && PhaseTime > 0.8f)
		{
			// Forward (W): the way the view looks.
			FVector Here;
			Bridge()->GetBlendedLocation(State->Me, Here);
			State->RollFrom = Here;
			State->bRollSent = Fight()->Roll(FVector2D(0.0, 1.0));
		}
		if (State->bRollSent && Me->rollPhase != 0 && !State->bRollRefused)
		{
			// A second roll while the first runs is refused.
			State->bRollRefused = !Fight()->Roll(FVector2D(1.0, 0.0));
		}
		FVector Now;
		if (State->bRollSent && Bridge()->GetBlendedLocation(State->Me, Now))
		{
			State->RollMoved = FMath::Max(State->RollMoved, FVector::Dist2D(Now, State->RollFrom));
		}
	};
	Roll.Verify = [=, this]()
	{
		const ASimPawnActor* Actor = Pawns()->FindPawn(State->Me);
		Notes.Add(FString::Printf(TEXT("feel_roll: moved %.0f cm, the body turned over %.0f degrees, the camera ducked %.1f degrees, trailed up to %.0f cm and %.0f cm at the end, blur up to %.2f, rolls drawn %d"), State->RollMoved, State->MaxRollAngle,
			-State->MinFeelPitch, State->MaxCameraTrail, State->CameraTrailAtEnd, State->MaxBlur, Actor ? Actor->GetRollsSeen() : 0));
		if (!State->bRollSent)
		{
			Fail(TEXT("feel_roll: the roll command was not sent"));
			return;
		}
		if (!State->bRollRefused)
		{
			Fail(TEXT("feel_roll: a second roll was accepted while the first was running"));
		}
		if (State->RollMoved < 300.0 || State->RollMoved > 450.0)
		{
			Fail(FString::Printf(TEXT("feel_roll: the pawn moved %.0f cm (the sim rolls it 4 m)"), State->RollMoved));
		}
		if (State->MaxRollAngle < 330.0f || !Actor || Actor->GetRollsSeen() < 1 || Actor->GetRollAngle() != 0.0f)
		{
			Fail(FString::Printf(TEXT("feel_roll: the body turned over %.0f degrees at most and %.0f now (rolls drawn %d)"), State->MaxRollAngle, Actor ? Actor->GetRollAngle() : -1.0f, Actor ? Actor->GetRollsSeen() : 0));
		}
		if (State->MinFeelPitch < -1.0f || State->MaxBlur < 0.8f)
		{
			Fail(FString::Printf(TEXT("feel_roll: the camera ducked %.1f degrees (none wanted) and the blur reached %.2f"), -State->MinFeelPitch, State->MaxBlur));
		}
		if (State->MaxCameraTrail < 20.0 || State->MaxCameraTrail > 145.0 || State->CameraTrailAtEnd > 5.0)
		{
			Fail(FString::Printf(TEXT("feel_roll: the camera trailed the body by up to %.0f cm and %.0f cm at the end (a soft follow of 20 to 140 cm that settles)"), State->MaxCameraTrail, State->CameraTrailAtEnd));
		}
		const colony::ThingView* Me = MyView();
		if (!Me || Me->rollPhase != 0)
		{
			Fail(TEXT("feel_roll: the roll has not ended"));
		}
	};
	Phases.Add(MoveTemp(Roll));

	// 6. A grenade: drawn in the air and on the ground, then the blast (fireball, smoke, sparks, a flash, debris on an invisible floor),
	// a shake that grows with nearness, and everything gone a few seconds later.
	FPhase Grenade;
	Grenade.Name = TEXT("feel_grenade");
	Grenade.WarmupSeconds = 9.0f;
	Grenade.MeasureSeconds = 4.0f;
	Grenade.Setup = [=, this]()
	{
		State->Reset();
		State->bThrown = false;
		State->ExplosionsAtStart = Feel()->GetExplosionsSeen();
		colony::Sim* Sim = SimOf();
		const colony::DefId Item = Sim->defs().idOf("Things", "Grenade_Frag");
		if (!Item.valid() || Sim->explosives().give(State->Me, Item, 3) < 1)
		{
			Fail(TEXT("feel_grenade: no grenades to give the protagonist"));
		}
		// Looking down the field with the camera behind the colonist, the blast 12 m away.
		AProtagonistPawn* Body = Prot()->GetBody();
		Body->Look(FRotator::NormalizeAxis(-Body->GetYaw()), -9.0f - Body->GetPitch());
		Body->SnapCamera();
	};
	Grenade.OnFrame = [=, this]()
	{
		Watch();
		if (!State->bThrown && PhaseTime > 0.8f)
		{
			State->bThrown = Fight()->ThrowGrenade();
		}
	};
	Grenade.Verify = [=, this]()
	{
		UShooterFeelSubsystem* F = Feel();
		const int32 Blasts = F->GetExplosionsSeen() - State->ExplosionsAtStart;
		Notes.Add(FString::Printf(TEXT("feel_grenade: %d blast(s); a grenade drawn %d at once; %d blast particles, %d debris pieces (%d made), flash up to %.0f cd, camera shake up to %.2f degrees, blur up to %.2f; debris went %.0f cm out, %.0f up and %.0f down"),
			Blasts, State->MaxGrenades, State->MaxBlast, State->MaxDebris, F->GetDebrisMade(), State->MaxFlash, State->MaxShake, State->MaxBlur, F->GetDebrisFarthestCm(), F->GetDebrisHighestCm(), -F->GetDebrisLowestCm()));
		if (!State->bThrown)
		{
			Fail(TEXT("feel_grenade: the grenade was not thrown"));
			return;
		}
		if (Blasts < 1 || State->MaxGrenades < 1)
		{
			Fail(FString::Printf(TEXT("feel_grenade: %d blasts and %d grenades ever drawn"), Blasts, State->MaxGrenades));
			return;
		}
		if (State->MaxBlast < 25 || State->MaxFlash < 1000.0f)
		{
			Fail(FString::Printf(TEXT("feel_grenade: %d blast particles and a flash of %.0f at most"), State->MaxBlast, State->MaxFlash));
		}
		if (F->GetDebrisMade() < 15 || F->GetDebrisFarthestCm() < 150.0f || F->GetDebrisHighestCm() < 80.0f)
		{
			Fail(FString::Printf(TEXT("feel_grenade: %d debris pieces, the farthest %.0f cm out and the highest %.0f cm up"), F->GetDebrisMade(), F->GetDebrisFarthestCm(), F->GetDebrisHighestCm()));
		}
		// Chaos works over the invisible floor: nothing falls through the ground.
		if (F->GetDebrisLowestCm() < -40.0f)
		{
			Fail(FString::Printf(TEXT("feel_grenade: debris fell %.0f cm below where the blast went off"), -F->GetDebrisLowestCm()));
		}
		if (State->MaxShake < 0.6f)
		{
			Fail(FString::Printf(TEXT("feel_grenade: the blast shook the camera only %.2f degrees"), State->MaxShake));
		}
		// The sim's blast was what it should be: the debris and the ragdolls did not change anything it had decided.
		if (F->GetDebrisAlive() > 0 && PhaseTime > 8.0f)
		{
			Fail(FString::Printf(TEXT("feel_grenade: %d debris pieces are still there after the blast's time"), F->GetDebrisAlive()));
		}
	};
	Phases.Add(MoveTemp(Grenade));

	// 7. A big blast from the dev command near the camera: the shake and blur scale with it.
	FPhase Big;
	Big.Name = TEXT("feel_big_blast");
	Big.WarmupSeconds = 1.0f;
	Big.MeasureSeconds = 3.0f;
	Big.Setup = [=, this]()
	{
		State->Reset();
		State->ExplosionsAtStart = Feel()->GetExplosionsSeen();
		const colony::ThingView* Me = MyView();
		Bridge()->Submit(colony::ExplodeCommand{Me->x + 9.0f, Me->y, 5.0f, 30.0f, "Bomb", 0.1f});
	};
	Big.OnFrame = Watch;
	Big.Verify = [=, this]()
	{
		Notes.Add(FString::Printf(TEXT("feel_big_blast: shake up to %.2f degrees, blur up to %.2f, %d debris pieces alive at most"), State->MaxShake, State->MaxBlur, State->MaxDebris));
		if (Feel()->GetExplosionsSeen() - State->ExplosionsAtStart < 1)
		{
			Fail(TEXT("feel_big_blast: the dev blast was not seen"));
		}
		if (State->MaxShake < 1.2f || State->MaxBlur < 0.7f || State->MaxDebris < 20)
		{
			Fail(FString::Printf(TEXT("feel_big_blast: shake %.2f, blur %.2f and %d debris pieces for a blast 9 m away"), State->MaxShake, State->MaxBlur, State->MaxDebris));
		}
		FeelSmokeSet(TEXT("Sky.Hour"), -1.0f);
		FeelSmokeSet(TEXT("Sky.DayOfYear"), -1.0f);
	};
	Phases.Add(MoveTemp(Big));

	// 8. The realistic blast look (docs/design/blast-draw.md, needs -run=BlastArt): a frag blast 12 m ahead in a cleared patch, the camera turned to it, in daylight,
	// pictured at its fireball, its rising smoke and its lingering cloud and scorch; then a molotov's splash of fire.
	const auto Fx = [this]() { return GetWorld()->GetSubsystem<UBlastFxSubsystem>(); };
	// Turns the camera to look at a map point a little below eye height, so the blast there is in the picture.
	const auto LookAtBlast = [=, this](float X, float Y, float Pitch, float Aside)
	{
		const colony::ThingView* Me = MyView();
		AProtagonistPawn* Body = Prot() ? Prot()->GetBody() : nullptr;
		if (!Me || !Body)
		{
			return;
		}
		// Aside degrees off it: the over-the-shoulder camera looks through the player's own body, which hid the fireball,
		// the scorch and the molotov's flames in PC rounds 3 to 5. -20 puts the blast right of the body, +20 left of it
		// (away from the colonists standing about on the right: PC round 8's linger shot).
		const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Y - Me->y, X - Me->x)) + Aside;
		Body->Look(FRotator::NormalizeAxis(Yaw - Body->GetYaw()), Pitch - Body->GetPitch());
		Body->SnapCamera();
	};
	FPhase Fireball;
	Fireball.Name = TEXT("blast_fireball");
	Fireball.WarmupSeconds = 0.2f;
	Fireball.MeasureSeconds = 1.0f;
	Fireball.ScreenshotAt = 0.1f;
	Fireball.Setup = [=, this]()
	{
		State->Reset();
		FeelSmokeSet(TEXT("Sky.Hour"), 14.0f);
		const colony::ThingView* Me = MyView();
		State->BlastsAtStart = Fx() ? Fx()->GetBlastsDrawn() : 0;
		// A clearing ahead, so no tree stands between the camera and the blasts (the molotov's spot too).
		if (colony::Sim* Sim = SimOf())
		{
			const int32 X0 = FMath::FloorToInt32(Me->x);
			const int32 Y0 = FMath::FloorToInt32(Me->y);
			for (int32 Dx = -4; Dx <= 22; ++Dx)
			{
				for (int32 Dy = -12; Dy <= 12; ++Dy)
				{
					Sim->plants().remove(colony::CellPos{X0 + Dx, Y0 + Dy, 0});
				}
			}
		}
		// The earlier phases' blasts (a 5 m one among them) would still be smoking in the picture.
		if (Fx())
		{
			Fx()->ClearForTest();
		}
		State->ScorchesAtStart = Fx() ? Fx()->GetScorchMarks() : 0;
		// Off to one side, out of line with the colonists who stand about ahead (PC round 3: they hid the fireball).
		State->BlastX = Me->x + 9.0f;
		State->BlastY = Me->y - 7.0f;
		LookAtBlast(State->BlastX, State->BlastY, 0.0f, -20.0f);
		Bridge()->Submit(colony::ExplodeCommand{State->BlastX, State->BlastY, 1.9f, 50.0f, "Bomb", 0.1f});
	};
	Fireball.OnFrame = Watch;
	Fireball.Verify = [=, this]()
	{
		if (!Fx() || !Fx()->IsReady())
		{
			Notes.Add(TEXT("blast_fireball: no blast art (run -run=BlastArt); the simple look was drawn"));
			return;
		}
		Notes.Add(FString::Printf(TEXT("blast_fireball: %d blast(s) drawn, %d sprites at most, light up to %.0f cd"), Fx()->GetBlastsDrawn() - State->BlastsAtStart, State->MaxBlast, State->MaxFlash));
		if (Fx()->GetBlastsDrawn() - State->BlastsAtStart < 1 || State->MaxBlast < 40 || State->MaxFlash < 2000.0f)
		{
			Fail(FString::Printf(TEXT("blast_fireball: %d blasts drawn, %d sprites, %.0f cd"), Fx()->GetBlastsDrawn() - State->BlastsAtStart, State->MaxBlast, State->MaxFlash));
		}
		if (Fx()->GetScorchMarks() - State->ScorchesAtStart < 1)
		{
			Fail(TEXT("blast_fireball: the blast left no scorch mark"));
		}
	};
	Phases.Add(MoveTemp(Fireball));

	FPhase Rising;
	Rising.Name = TEXT("blast_smoke");
	Rising.WarmupSeconds = 0.0f;
	Rising.MeasureSeconds = 3.0f;
	Rising.ScreenshotAt = 2.5f;
	Rising.Setup = [=]() { State->MaxSmoke = 0; };
	Rising.OnFrame = Watch;
	Phases.Add(MoveTemp(Rising));

	FPhase Lingering;
	Lingering.Name = TEXT("blast_linger");
	Lingering.WarmupSeconds = 0.0f;
	Lingering.MeasureSeconds = 7.0f;
	Lingering.ScreenshotAt = 6.5f;
	// The smoke still hanging over the blast's spot.
	Lingering.Setup = [=]()
	{
		State->MaxSmoke = 0;
		// The hanging cloud, looked at a little upward. The scorch is pictured by blast_scorch: 11 m off it is a sliver at a
		// grazing angle, with the colonists standing on the right of it and the player's own body on the left (rounds 3 to 9).
		LookAtBlast(State->BlastX, State->BlastY, 6.0f, -20.0f);
	};
	Lingering.OnFrame = Watch;
	Lingering.Verify = [=, this]()
	{
		if (!Fx() || !Fx()->IsReady())
		{
			return;
		}
		// About 11 s after the blast: the smoke still hangs and the scorch is still there.
		Notes.Add(FString::Printf(TEXT("blast_linger: %d smoke puffs at the end, %d scorch marks"), Fx()->GetSmokeAlive(), Fx()->GetScorchMarks()));
		if (Fx()->GetSmokeAlive() < 3)
		{
			Fail(FString::Printf(TEXT("blast_linger: only %d smoke puffs left 11 s after the blast"), Fx()->GetSmokeAlive()));
		}
		if (Fx()->GetScorchMarks() < 1)
		{
			Fail(TEXT("blast_linger: the scorch mark is gone already"));
		}
	};
	Phases.Add(MoveTemp(Lingering));

	// The scorch mark alone, with no smoke over it and looked down at from 5 m (rounds 3 to 7 never saw it behind the smoke).
	FPhase ScorchOnly;
	ScorchOnly.Name = TEXT("blast_scorch");
	ScorchOnly.WarmupSeconds = 0.2f;
	ScorchOnly.MeasureSeconds = 1.0f;
	ScorchOnly.ScreenshotAt = 0.5f;
	ScorchOnly.Setup = [=, this]()
	{
		const colony::ThingView* Me = MyView();
		if (!Me || !Fx())
		{
			return;
		}
		Fx()->ClearForTest();
		Fx()->AddScorchForTest(Me->x + 5.0f, Me->y - 2.0f, 1.9f);
		LookAtBlast(Me->x + 5.0f, Me->y - 2.0f, -30.0f, -20.0f);
	};
	ScorchOnly.Verify = [=, this]()
	{
		if (!Fx() || !Fx()->IsReady())
		{
			return;
		}
		Notes.Add(FString::Printf(TEXT("blast_scorch: %d scorch mark(s), look for a dark burnt patch with rays on the ground"), Fx()->GetScorchMarks()));
		if (Fx()->GetScorchMarks() < 1)
		{
			Fail(TEXT("blast_scorch: no scorch mark was made"));
		}
	};
	Phases.Add(MoveTemp(ScorchOnly));

	// A fresh scorch close up at dusk, for its embers (PC round 12: from 5 m at noon an ember is about a pixel).
	FPhase ScorchClose;
	ScorchClose.Name = TEXT("blast_scorch_close");
	ScorchClose.WarmupSeconds = 1.5f;  // the exposure settles to dusk
	ScorchClose.MeasureSeconds = 0.6f;
	ScorchClose.ScreenshotAt = 0.2f;
	ScorchClose.Setup = [=, this]()
	{
		const colony::ThingView* Me = MyView();
		if (!Me || !Fx())
		{
			return;
		}
		FeelSmokeSet(TEXT("Sky.Hour"), 19.5f);
		Fx()->ClearForTest();
		Fx()->AddScorchForTest(Me->x + 2.8f, Me->y - 1.2f, 1.9f);
		LookAtBlast(Me->x + 2.8f, Me->y - 1.2f, -45.0f, -25.0f);
	};
	ScorchClose.Verify = [=, this]()
	{
		FeelSmokeSet(TEXT("Sky.Hour"), 14.0f);
		if (Fx() && Fx()->IsReady())
		{
			Notes.Add(TEXT("blast_scorch_close: look for orange embers glowing in the charred core"));
		}
	};
	Phases.Add(MoveTemp(ScorchClose));

	FPhase Molotov;
	Molotov.Name = TEXT("blast_molotov");
	Molotov.WarmupSeconds = 0.2f;
	Molotov.MeasureSeconds = 1.5f;
	Molotov.ScreenshotAt = 0.3f;
	Molotov.Setup = [=, this]()
	{
		State->Reset();
		const colony::ThingView* Me = MyView();
		// Nearer and looked down at, so the colonists standing about don't hide the burning ground (PC round 4).
		LookAtBlast(Me->x + 6.0f, Me->y + 6.0f, -12.0f, -20.0f);
		Bridge()->Submit(colony::ExplodeCommand{Me->x + 6.0f, Me->y + 6.0f, 1.1f, 10.0f, "Burn", 0.0f});
	};
	Molotov.OnFrame = Watch;
	Molotov.Verify = [=, this]()
	{
		Notes.Add(FString::Printf(TEXT("blast_molotov: %d sprites at most, light up to %.0f cd"), State->MaxBlast, State->MaxFlash));
		FeelSmokeSet(TEXT("Sky.Hour"), -1.0f);
	};
	Phases.Add(MoveTemp(Molotov));
}
