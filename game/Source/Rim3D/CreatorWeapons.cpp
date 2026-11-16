// The character creator's WEAPONS window and fight demo (docs/design/creator-weapons.md). Cosmetic only: Mixamo clips
// on the pawns' mannequins (the visible MakeHuman bodies follow them), weapons on the visible bodies' hands, our own
// blade-against-body test, and blood made of droplets and decals. Nothing here touches the sim.
#include "CreatorWeapons.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "CollisionQueryParams.h"
#include "Components/DecalComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/App.h"
#include "UnrealClient.h"

#include "FightSubsystem.h"
#include "MakeHumanAnimInstance.h"
#include "PawnActor.h"
#include "PawnAnimInstance.h"
#include "SimSubsystem.h"
#include "VisualMesh.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"

#include <imgui.h>

DEFINE_LOG_CATEGORY_STATIC(LogCreatorWeapons, Log, All);

namespace
{
	// ---------------------------------------------------------------------------------------------------------------
	// Styles, attacks and the clips they play (paths under /Game/Rim3D/Generated/Animations, made by MakeHumanArt -Clips).

	enum class ECwStyle : int32 { Club, GreatSword, SwordShield, Dual, Count };
	constexpr int32 CwStyleCount = static_cast<int32>(ECwStyle::Count);

	struct FCwAttack
	{
		const char* Label;
		const TCHAR* Clip;
		float Heavy;      // 0 light .. 1 heavy: how hard it hits (stagger, blood)
		bool bBlock;      // a guard, not a blow (preview only)
	};

	// What a style holds. "Shield" is the procedural shield; other names are melee weapon defs (their visual parts).
	struct FCwStyleDef
	{
		const char* Label;
		const char* Note;
		const char* RightDef;
		const char* LeftDef;      // null: empty left hand
		const TCHAR* Stance;
		const FCwAttack* Attacks;
		int32 AttackCount;
		const TCHAR* Blocks[3];   // guard clips (the defender's, in the demo); null ends
		const TCHAR* BlockReact;  // a blow landing on the guard
		const TCHAR* Impacts[5];  // hit from the front; null ends
		const TCHAR* Deaths[2];
	};

	const FCwAttack CwClubAttacks[] = {
		{"Overhead", TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingMeleeAttackDownward"), 0.8f, false},
		{"Horizontal", TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingMeleeAttackHorizontal"), 0.6f, false},
		{"Backhand", TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingMeleeAttackBackhand"), 0.5f, false},
		{"360 high", TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingMeleeAttack360High"), 0.9f, false},
		{"360 low", TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingMeleeAttack360Low"), 0.7f, false},
		{"Combo 1", TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingMeleeComboAttackVer1"), 0.6f, false},
		{"Combo 2", TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingMeleeComboAttackVer2"), 0.6f, false},
		{"Combo 3", TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingMeleeComboAttackVer3"), 0.7f, false},
		{"Heavy swing", TEXT("HeavySwing/A_HeavySwing"), 1.0f, false},
		{"Running jump attack", TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingMeleeRunJumpAttack"), 1.0f, false},
		{"Kick", TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingMeleeAttackKickVer1"), 0.4f, false},
		{"Guard", TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingBlockIdle"), 0.0f, true},
	};
	const FCwAttack CwGreatSwordAttacks[] = {
		{"Slash 1", TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordSlash"), 0.7f, false},
		{"Slash 2", TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordSlash2"), 0.7f, false},
		{"Slash 3", TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordSlash3"), 0.7f, false},
		{"Slash 4", TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordSlash4"), 0.8f, false},
		{"Slash 5", TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordSlash5"), 0.8f, false},
		{"Attack", TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordAttack"), 0.8f, false},
		{"High spin", TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordHighSpinAttack"), 1.0f, false},
		{"Jump attack", TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordJumpAttack"), 1.0f, false},
		{"Slide attack", TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordSlideAttack"), 0.9f, false},
		{"Kick", TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordKick"), 0.4f, false},
		{"Guard", TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordBlocking"), 0.0f, true},
	};
	const FCwAttack CwSwordShieldAttacks[] = {
		{"Attack 1", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldAttack"), 0.6f, false},
		{"Attack 2", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldAttack2"), 0.6f, false},
		{"Attack 3", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldAttack3"), 0.7f, false},
		{"Attack 4", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldAttack4"), 0.7f, false},
		{"Slash 1", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldSlash"), 0.6f, false},
		{"Slash 2", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldSlash2"), 0.6f, false},
		{"Slash 3", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldSlash3"), 0.6f, false},
		{"Slash 4", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldSlash4"), 0.7f, false},
		{"Slash 5", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldSlash5"), 0.7f, false},
		{"Kick", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldKick"), 0.4f, false},
		{"Block", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldBlockIdle"), 0.0f, true},
	};
	// No dual-wield clips exist: the dual sticks mocap plus one-handed slashes (the left sword mostly rides along).
	const FCwAttack CwDualAttacks[] = {
		{"Dual sticks flurry", TEXT("DualSticks/A_DualSticks"), 0.6f, false},
		{"Slash 1", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldSlash"), 0.6f, false},
		{"Slash 2", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldSlash3"), 0.6f, false},
		{"Slash 3", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldSlash5"), 0.7f, false},
		{"Thrust", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldAttack2"), 0.6f, false},
		{"Backhand", TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingMeleeAttackBackhand"), 0.5f, false},
		{"Guard", TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordBlocking"), 0.0f, true},
	};

	const FCwStyleDef CwStyles[CwStyleCount] = {
		{"Club (one hand)", nullptr, "Melee_Club", nullptr, TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingIdle"), CwClubAttacks, UE_ARRAY_COUNT(CwClubAttacks),
			{TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingBlockIdle"), nullptr, nullptr}, TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingBlockReactLarge"),
			{TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingReactLargeGut"), TEXT("Mixamo/Injured/A_HitReaction"), TEXT("Mixamo/Injured/A_HitToHead"), nullptr, nullptr},
			{TEXT("Mixamo/Magic/A_ProMagicStandingReactDeathBackward"), TEXT("Mixamo/Magic/A_ProMagicStandingReactDeathForward")}},
		{"Sword (two hands)", nullptr, "Melee_Longsword", nullptr, TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordIdle"), CwGreatSwordAttacks, UE_ARRAY_COUNT(CwGreatSwordAttacks),
			{TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordBlocking"), TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordBlocking2"), TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordBlocking3")},
			TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordBlocking3"),
			{TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordImpact"), TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordImpact2"), TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordImpact3"),
				TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordImpact4"), TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordImpact5")},
			{TEXT("Mixamo/GreatSword/A_GreatSwordTwoHandedSwordDeath"), TEXT("Mixamo/GreatSword/A_GreatSwordTwoHandedSwordDeath2")}},
		{"Sword and shield", nullptr, "Melee_Gladius", "Shield", TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldIdle"), CwSwordShieldAttacks,
			UE_ARRAY_COUNT(CwSwordShieldAttacks),
			{TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldBlock"), TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldBlock2"), nullptr},
			TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldBlock2"),
			{TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldImpact"), TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldImpact2"),
				TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldImpact3"), nullptr, nullptr},
			{TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldDeath"), TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldDeath2")}},
		{"Dual wield", "Plainer: there are no two-sword clips, so these are one-handed moves with a sword in each hand.", "Melee_Gladius", "Melee_Gladius",
			TEXT("Mixamo/MeleeAxe/A_ProMeleeAxeStandingIdle"), CwDualAttacks, UE_ARRAY_COUNT(CwDualAttacks),
			{TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordBlocking"), nullptr, nullptr}, TEXT("Mixamo/GreatSword/A_GreatSwordGreatSwordBlocking3"),
			{TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldImpact"), TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldImpact2"),
				TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldImpact3"), nullptr, nullptr},
			{TEXT("Mixamo/SwordandShield/A_ProSwordAndShieldSwordAndShieldDeath2"), TEXT("Mixamo/Magic/A_ProMagicStandingReactDeathBackward")}},
	};

	// Reactions by the side a blow came from (front uses the style's own impacts).
	const TCHAR* const CwReactSmall[4] = {TEXT("Mixamo/Magic/A_ProMagicStandingReactSmallFromFront"), TEXT("Mixamo/Magic/A_ProMagicStandingReactSmallFromRight"),
		TEXT("Mixamo/Magic/A_ProMagicStandingReactSmallFromBack"), TEXT("Mixamo/Magic/A_ProMagicStandingReactSmallFromLeft")};
	// The heaviest blows: a stumble back that ends on the ground, then getting up (never a snap from lying to standing).
	const TCHAR* const CwStumble = TEXT("Mixamo/Injured/A_StumbleBackwards");
	const TCHAR* const CwGetUp = TEXT("Mixamo/Falls/A_GettingUp");
	const TCHAR* const CwReactLarge[4] = {TEXT("Mixamo/Magic/A_ProMagicStandingReactLargeFromFront"), TEXT("Mixamo/Magic/A_ProMagicStandingReactLargeFromRight"),
		TEXT("Mixamo/Magic/A_ProMagicStandingReactLargeFromBack"), TEXT("Mixamo/Magic/A_ProMagicStandingReactLargeFromLeft")};
	const TCHAR* const CwDeathBySide[4] = {TEXT("Mixamo/Magic/A_ProMagicStandingReactDeathBackward"), TEXT("Mixamo/Magic/A_ProMagicStandingReactDeathLeft"),
		TEXT("Mixamo/Magic/A_ProMagicStandingReactDeathForward"), TEXT("Mixamo/Magic/A_ProMagicStandingReactDeathRight")};

	const TCHAR* const CwAnimRoot = TEXT("/Game/Rim3D/Generated/Animations");
	const TCHAR* const CwSolidMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");
	const TCHAR* const CwSplatMaterialPath = TEXT("/Game/Rim3D/Generated/Fights/M_BloodSplat.M_BloodSplat");
	const TCHAR* const CwWoundMaterialPath = TEXT("/Game/Rim3D/Generated/Fights/M_WoundDecal.M_WoundDecal");

	constexpr int32 CwMaxFloorSpots = 64;
	constexpr int32 CwMaxBodyStains = 24;
	constexpr int32 CwMaxDroplets = 400;
	constexpr float CwHitStopSeconds = 0.08f;
	constexpr float CwRecoilSeconds = 0.16f;

	// Body capsules for the blade test: bone to bone (UE4 mannequin names, which the MakeHuman game_engine skeleton has).
	struct FCwCapsule
	{
		const TCHAR* A;
		const TCHAR* B;
		float Radius;
	};
	const FCwCapsule CwCapsules[] = {
		{TEXT("pelvis"), TEXT("spine_02"), 15.0f}, {TEXT("spine_02"), TEXT("neck_01"), 15.0f}, {TEXT("neck_01"), TEXT("head"), 6.0f},
		{TEXT("upperarm_l"), TEXT("lowerarm_l"), 5.5f}, {TEXT("lowerarm_l"), TEXT("hand_l"), 4.5f},
		{TEXT("upperarm_r"), TEXT("lowerarm_r"), 5.5f}, {TEXT("lowerarm_r"), TEXT("hand_r"), 4.5f},
		{TEXT("thigh_l"), TEXT("calf_l"), 8.0f}, {TEXT("calf_l"), TEXT("foot_l"), 6.0f},
		{TEXT("thigh_r"), TEXT("calf_r"), 8.0f}, {TEXT("calf_r"), TEXT("foot_r"), 6.0f},
	};

	// ---------------------------------------------------------------------------------------------------------------
	// State.

	struct FCwStain
	{
		TWeakObjectPtr<UDecalComponent> Decal;
		TWeakObjectPtr<UMaterialInstanceDynamic> Mid;
		float Age = 0.0f;
		float Half = 6.0f;
		bool bStreak = false;
	};

	struct FCwSpot
	{
		TWeakObjectPtr<UDecalComponent> Decal;
		TWeakObjectPtr<UMaterialInstanceDynamic> Mid;
		float Age = 0.0f;
	};

	struct FCwDroplet
	{
		FVector P = FVector::ZeroVector;
		FVector V = FVector::ZeroVector;
		float Size = 0.5f;  // cm radius
		float Age = 0.0f;
		float GroundZ = 0.0f;
	};

	enum class ECwRole : uint8 { Idle, Stance, Attack, HitStop, Recoil, Block, React, Dying, Dead };

	struct FCwPerson
	{
		TWeakObjectPtr<ASimPawnActor> Pawn;
		TWeakObjectPtr<USkeletalMeshComponent> Mesh;   // the visible body the weapons hang on
		TWeakObjectPtr<UStaticMeshComponent> Right;
		TWeakObjectPtr<UStaticMeshComponent> Left;
		int32 HeldStyle = -1;
		// The blades last frame (world), for the swept test.
		FVector PrevHilt[2] = {FVector::ZeroVector, FVector::ZeroVector};
		FVector PrevTip[2] = {FVector::ZeroVector, FVector::ZeroVector};
		bool bPrevValid = false;
		// What it is doing.
		ECwRole Role = ECwRole::Idle;
		float RoleTime = 0.0f;
		TWeakObjectPtr<UAnimMontage> Montage;
		float PrevMontagePos = 0.0f;
		bool bOurMontage = false;
		// Hurt.
		int32 Hits = 0;
		float Blood = 0.0f;
		TArray<FCwStain> Stains;
		TWeakObjectPtr<UDecalComponent> Pool;
		TWeakObjectPtr<UMaterialInstanceDynamic> PoolMid;
		float PoolRadius = 0.0f;
		float DripIn = 1.0f;
		// The demo stands the body somewhere else (the mesh moved, the pawn not).
		bool bMoved = false;
		FTransform SavedRelative = FTransform::Identity;
		FVector Facing = FVector::ForwardVector;  // world, flat: where the demo has the body face
		FVector Spot = FVector::ZeroVector;       // world: where the demo stands the body (its feet)
		float LeftGrip = 0.0f;                    // 0..1: the left hand on a two-handed hilt (IK on the visible body)
		bool bFloored = false;                    // its react ends on the ground: getting up follows
		float GiveGround = 0.0f;                  // cm its demo spot has moved back, giving ground to a lunge
		float AnchorT = 0.0f;                     // s left holding its pelvis where it is (a clip change whose root differs)
		float PelvisAlong = 0.0f;                 // its pelvis ahead of its spot along its facing last frame (cm)
		bool bPelvisAlong = false;
	};

	enum class ECwDemo : uint8 { Off, Pause, Swing, Impact, Recover, DeathHold };

	struct FCwState
	{
		TWeakObjectPtr<UWorld> World;
		TArray<FCwPerson> People;
		int32 Selected = 0;
		bool bEquip = true;
		int32 Style = 0;
		int32 Attack = -1;          // -1: stance only
		float Speed = 1.0f;
		float PreviewPause = 0.0f;  // the gap between swings in the preview
		// Grip tweaks per style and hand: offset (cm, along the grip's axes) and turn (degrees).
		float Grip[CwStyleCount][2][6] = {};
		float LeftHandBelow = 9.0f; // two hands: the left hand this far below the right along the hilt (cm)
		// Demo.
		ECwDemo Demo = ECwDemo::Off;
		int32 DemoA = 0, DemoB = 1;
		int32 Attacker = 0;         // 0: DemoA attacks, 1: DemoB
		float DemoTime = 0.0f;
		float Distance = 160.0f;    // cm between them
		float ExtraApart = 0.0f;    // both fighters' give-ground together (log)
		float RawGap = -1.0f;       // pelvis to pelvis along the line last frame, less the give-ground (cm), -1 unknown
		float GapRate = 0.0f;       // how fast that closes (cm/s, negative closing), smoothed
		float SwingMinGap = 1.0e6f; // the closest the pelvises came this swing (log)
		bool bPickedOnly = false;
		bool bFinish = false;
		bool bBlocking = false;     // the defender guards this swing
		bool bContact = false;
		// -CwScript: a fixed run for pictures (styles, attacks, the demo), shots into Saved/Weapons.
		float ScriptT = -1.0f;
		int32 ScriptStep = 0;
		int32 ScriptHitShots = 0;
		FString PendingShot;
		int32 Recorded = 0;          // -CwRecord=<seconds>: frames of the demo saved so far
		bool bForceStagger = false;  // the next blow that reaches the body staggers (StumbleBackwards); no guarding meanwhile
		float StaggerT = -1.0f;      // -CwScript: seconds since the forced stagger landed (shots), -1 none
		int32 StaggerShots = 0;
		float MostApart = 0.0f;      // the largest push-apart so far (log)
		int32 SwingAttack = 0;
		int32 Swings = 0, BodyHits = 0, Blocks = 0, WorldHits = 0, Misses = 0;
		FString LastEvent;
		// Blood.
		bool bBlood = true;
		TArray<FCwDroplet> Droplets;
		TArray<FCwSpot> Spots;
		TWeakObjectPtr<AActor> Holder;
		TWeakObjectPtr<UInstancedStaticMeshComponent> DropletInstances;
		TWeakObjectPtr<UMaterialInterface> SplatBase;
		bool bSplatIsWound = false;
		bool bTriedMaterials = false;
		int32 DecalOrder = 10;
		float StainUpdate = 0.0f;
		// Meshes (weak: their components keep them).
		TMap<FString, TWeakObjectPtr<UStaticMesh>> Meshes;
		TMap<FString, FVector2f> Extents;  // per weapon: where its striking part starts and ends along x (cm)
		TMap<FString, TWeakObjectPtr<UAnimSequence>> Clips;
		TSet<FString> MissingClips;
		FRandomStream Random{1234};
	};

	FCwState& CwState()
	{
		static FCwState State;
		return State;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Helpers.

	UAnimSequence* CwLoadClip(FCwState& S, const TCHAR* Rel)
	{
		if (!Rel)
		{
			return nullptr;
		}
		const FString Key(Rel);
		if (const TWeakObjectPtr<UAnimSequence>* Found = S.Clips.Find(Key); Found && Found->IsValid())
		{
			return Found->Get();
		}
		if (S.MissingClips.Contains(Key))
		{
			return nullptr;
		}
		const FString Name = FPaths::GetBaseFilename(Key);
		const FString Path = FString(CwAnimRoot) / Key + TEXT(".") + Name;
		UAnimSequence* Seq = LoadObject<UAnimSequence>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Seq && !S.MissingClips.Contains(Key))
		{
			S.MissingClips.Add(Key);
			UE_LOG(LogCreatorWeapons, Warning, TEXT("No clip %s (run the MakeHumanArt commandlet with -Clips)"), *Path);
		}
		S.Clips.Add(Key, Seq);
		return Seq;
	}

	UAnimMontage* CwPlay(FCwState& S, FCwPerson& P, const TCHAR* Rel, float Rate, bool bLoop, float BlendIn = 0.2f, float BlendOut = 0.25f)
	{
		ASimPawnActor* Pawn = P.Pawn.Get();
		USimPawnAnimInstance* Anim = Pawn ? Pawn->GetAnim() : nullptr;
		UAnimSequence* Seq = CwLoadClip(S, Rel);
		if (!Anim || !Seq)
		{
			return nullptr;
		}
		UAnimMontage* Montage = Anim->PlaySlotAnimationAsDynamicMontage(Seq, FAnimSlotGroup::DefaultSlotName, BlendIn, BlendOut, Rate, bLoop ? 100000 : 1);
		P.Montage = Montage;
		P.PrevMontagePos = 0.0f;
		P.bOurMontage = Montage != nullptr;
		return Montage;
	}

	void CwStopMontage(FCwPerson& P, float BlendOut)
	{
		ASimPawnActor* Pawn = P.Pawn.Get();
		USimPawnAnimInstance* Anim = Pawn ? Pawn->GetAnim() : nullptr;
		if (Anim && P.bOurMontage && P.Montage.IsValid())
		{
			Anim->Montage_Stop(BlendOut, P.Montage.Get());
		}
		P.Montage = nullptr;
		P.bOurMontage = false;
	}

	/** 0..1 through the montage it plays now (1 when none). */
	float CwMontageFraction(const FCwPerson& P)
	{
		const ASimPawnActor* Pawn = P.Pawn.Get();
		const USimPawnAnimInstance* Anim = Pawn ? Pawn->GetAnim() : nullptr;
		const UAnimMontage* Montage = P.Montage.Get();
		if (!Anim || !Montage || !Anim->Montage_IsPlaying(Montage) || Montage->GetPlayLength() <= 0.0f)
		{
			return 1.0f;
		}
		return FMath::Clamp(Anim->Montage_GetPosition(Montage) / Montage->GetPlayLength(), 0.0f, 1.0f);
	}

	float CwMontagePos(const FCwPerson& P)
	{
		const ASimPawnActor* Pawn = P.Pawn.Get();
		const USimPawnAnimInstance* Anim = Pawn ? Pawn->GetAnim() : nullptr;
		return Anim && P.Montage.IsValid() ? Anim->Montage_GetPosition(P.Montage.Get()) : 0.0f;
	}

	void CwSetRate(FCwPerson& P, float Rate)
	{
		ASimPawnActor* Pawn = P.Pawn.Get();
		USimPawnAnimInstance* Anim = Pawn ? Pawn->GetAnim() : nullptr;
		if (Anim && P.Montage.IsValid())
		{
			Anim->Montage_SetPlayRate(P.Montage.Get(), Rate);
		}
	}

	void CwSetPos(FCwPerson& P, float Pos)
	{
		ASimPawnActor* Pawn = P.Pawn.Get();
		USimPawnAnimInstance* Anim = Pawn ? Pawn->GetAnim() : nullptr;
		if (Anim && P.Montage.IsValid())
		{
			Anim->Montage_SetPosition(P.Montage.Get(), FMath::Max(0.0f, Pos));
		}
	}

	const TCHAR* CwPick(FCwState& S, const TCHAR* const* List, int32 Max)
	{
		int32 Count = 0;
		while (Count < Max && List[Count])
		{
			++Count;
		}
		return Count > 0 ? List[S.Random.RandRange(0, Count - 1)] : nullptr;
	}

	bool CwHasBone(const USkeletalMeshComponent* Mesh, const TCHAR* Bone)
	{
		return Mesh && Mesh->GetBoneIndex(FName(Bone)) != INDEX_NONE;
	}

	FVector CwBone(const USkeletalMeshComponent* Mesh, const TCHAR* Bone)
	{
		return Mesh->GetBoneLocation(FName(Bone), EBoneSpaces::WorldSpace);
	}

	/** The holder actor for the droplets, and the materials. */
	void CwEnsureEffects(FCwState& S, UWorld* World)
	{
		if (!S.Holder.IsValid())
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AActor* Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
			USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("Root"));
			Holder->SetRootComponent(Root);
			Root->RegisterComponent();
			S.Holder = Holder;
		}
		if (!S.DropletInstances.IsValid())
		{
			if (UMaterialInterface* Solid = LoadObject<UMaterialInterface>(nullptr, CwSolidMaterialPath))
			{
				// A droplet: a 1 cm ball centred on its origin, stretched along its velocity per instance.
				VisualMesh::FPart Part;
				Part.Shape = VisualMesh::EShape::Sphere;
				Part.Size = FVector3f(1.0f, 1.0f, 2.0f);
				Part.At = FVector3f(0.0f, 0.0f, -1.0f);
				Part.Color = FLinearColor(0.16f, 0.006f, 0.005f);
				TArray<VisualMesh::FPart> Parts;
				Parts.Add(Part);
				UStaticMesh* Mesh = VisualMesh::MakeMesh(S.Holder.Get(), Solid, Parts, VisualMesh::EPartSet::All, nullptr, false);
				UInstancedStaticMeshComponent* Ism = NewObject<UInstancedStaticMeshComponent>(S.Holder.Get());
				Ism->SetStaticMesh(Mesh);
				Ism->SetMobility(EComponentMobility::Movable);
				Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Ism->SetGenerateOverlapEvents(false);
				Ism->SetCanEverAffectNavigation(false);
				Ism->SetCastShadow(false);
				Ism->bReceivesDecals = false;
				Ism->SetupAttachment(S.Holder->GetRootComponent());
				Ism->RegisterComponent();
				S.DropletInstances = Ism;
			}
		}
		if (!S.bTriedMaterials)
		{
			S.bTriedMaterials = true;
			UMaterialInterface* Splat = LoadObject<UMaterialInterface>(nullptr, CwSplatMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Splat)
			{
				Splat = LoadObject<UMaterialInterface>(nullptr, CwWoundMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
				S.bSplatIsWound = Splat != nullptr;
				UE_LOG(LogCreatorWeapons, Warning, TEXT("%s is missing (UnrealEditor-Cmd.exe Rim3D.uproject -run=FightMaterials): blood uses %s"), CwSplatMaterialPath,
					Splat ? CwWoundMaterialPath : TEXT("nothing"));
			}
			S.SplatBase = Splat;
		}
	}

	/** A blood decal material: fresh is wetter, brighter red and glossy; it darkens and dulls as it dries. */
	UMaterialInstanceDynamic* CwBloodMid(FCwState& S, float Opacity)
	{
		UMaterialInterface* Base = S.SplatBase.Get();
		if (!Base || !S.Holder.IsValid())
		{
			return nullptr;
		}
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, S.Holder.Get());
		Mid->SetScalarParameterValue(TEXT("Opacity"), Opacity);
		Mid->SetScalarParameterValue(TEXT("Seed"), S.Random.FRandRange(0.0f, 100.0f));
		return Mid;
	}

	void CwBloodAge(UMaterialInstanceDynamic* Mid, float Age, float DrySeconds)
	{
		if (!Mid)
		{
			return;
		}
		const float Fresh = FMath::Exp(-Age / DrySeconds);
		const FLinearColor Dry(0.045f, 0.0035f, 0.003f);
		const FLinearColor Wet(0.08f, 0.004f, 0.0035f);
		Mid->SetVectorParameterValue(TEXT("Color"), FMath::Lerp(Dry, Wet, Fresh));
		Mid->SetScalarParameterValue(TEXT("Roughness"), FMath::Lerp(0.55f, 0.1f, Fresh));
	}

	void CwDestroyDecal(TWeakObjectPtr<UDecalComponent>& Decal)
	{
		if (Decal.IsValid())
		{
			Decal->DestroyComponent();
		}
		Decal = nullptr;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Weapons.

	/** A mesh for a melee weapon def (its visual parts, along +x with the grip at the origin) or the shield. */
	UStaticMesh* CwWeaponMesh(FCwState& S, const char* Def, float& OutHilt, float& OutTip)
	{
		OutHilt = 8.0f;
		OutTip = 60.0f;
		const FString Key(Def);
		const TWeakObjectPtr<UStaticMesh>* Made = S.Meshes.Find(Key);
		if (const FVector2f* Extent = S.Extents.Find(Key); Extent && Made && Made->IsValid())
		{
			OutHilt = Extent->X;
			OutTip = Extent->Y;
			return Made->Get();
		}
		UMaterialInterface* Solid = LoadObject<UMaterialInterface>(nullptr, CwSolidMaterialPath);
		if (!Solid || !S.Holder.IsValid())
		{
			return nullptr;
		}
		TArray<VisualMesh::FPart> Parts;
		if (Key == TEXT("Shield"))
		{
			// A round shield: a wooden board, an iron rim behind it and an iron boss, its face along +z.
			// 50 cm across. The cylinder shape has only six sides, so the rim and the board are each 24 of them turned
			// 2.5 degrees apart (four turned 15 degrees apart left notches between the corners: a saw-blade edge); a
			// flattened ball makes the board's slight dome, an iron boss its middle.
			const FLinearColor Iron(0.20f, 0.20f, 0.22f);
			const FLinearColor Wood(0.28f, 0.16f, 0.075f);
			for (int32 Turn = 0; Turn < 24; ++Turn)
			{
				VisualMesh::FPart Rim;
				Rim.Shape = VisualMesh::EShape::Cylinder;
				Rim.Size = FVector3f(25.0f, 25.0f, 2.2f);
				Rim.At = FVector3f(0.0f, 0.0f, -1.6f);
				Rim.Rotation = FRotator3f(0.0f, Turn * 2.5f, 0.0f);
				Rim.Color = Iron;
				VisualMesh::FPart Board = Rim;
				Board.Size = FVector3f(23.8f, 23.8f, 2.6f);
				Board.At = FVector3f(0.0f, 0.0f, -1.2f);
				Board.Color = Wood;
				Parts.Add(Rim);
				Parts.Add(Board);
			}
			VisualMesh::FPart Dome;
			Dome.Shape = VisualMesh::EShape::Sphere;
			Dome.Size = FVector3f(23.0f, 23.0f, 5.0f);
			Dome.At = FVector3f(0.0f, 0.0f, -1.5f);
			Dome.Color = Wood;
			VisualMesh::FPart Boss;
			Boss.Shape = VisualMesh::EShape::Sphere;
			Boss.Size = FVector3f(5.5f, 5.5f, 5.5f);
			Boss.At = FVector3f(0.0f, 0.0f, -1.0f);
			Boss.Color = FLinearColor(0.32f, 0.32f, 0.34f);
			Parts.Add(Dome);
			Parts.Add(Boss);
			OutHilt = 0.0f;
			OutTip = 25.0f;  // the radius
		}
		else
		{
			const USimSubsystem* Bridge = S.World.IsValid() ? S.World->GetSubsystem<USimSubsystem>() : nullptr;
			const colony::Def* Found = Bridge && Bridge->GetDefs() ? Bridge->GetDefs()->find("Things", Def) : nullptr;
			if (Found)
			{
				VisualMesh::ReadParts(*Found, "visual", FVector2f(100.0f, 100.0f), Parts);
			}
			if (Parts.IsEmpty())
			{
				VisualMesh::FPart Fallback;
				Fallback.Size = FVector3f(60.0f, 4.0f, 5.0f);
				Fallback.At = FVector3f(30.0f, 0.0f, -2.5f);
				Fallback.Color = FLinearColor(0.36f, 0.23f, 0.12f);
				Parts.Add(Fallback);
			}
			float Tip = 0.0f;
			for (const VisualMesh::FPart& Part : Parts)
			{
				Tip = FMath::Max(Tip, Part.At.X + Part.Size.X * 0.5f);
			}
			OutTip = Tip;
			OutHilt = FMath::Min(8.0f, Tip * 0.3f);
		}
		S.Extents.Add(Key, FVector2f(OutHilt, OutTip));
		if (Made && Made->IsValid())
		{
			return Made->Get();
		}
		UStaticMesh* Mesh = VisualMesh::MakeMesh(S.Holder.Get(), Solid, Parts, VisualMesh::EPartSet::All, nullptr, false);
		S.Meshes.Add(Key, Mesh);
		return Mesh;
	}

	/**
	 * Where a weapon sits in a hand, world space: x along the grip from the little finger's knuckle to the index
	 * finger's (the blade leaves the fist on the thumb side), z the way the fingers point (a blade's edge), the grip in
	 * the curled fingers. False if the body has no such hand.
	 */
	bool CwGripFrame(const USkeletalMeshComponent* Mesh, bool bRight, FTransform& Out)
	{
		const TCHAR* Hand = bRight ? TEXT("hand_r") : TEXT("hand_l");
		if (!CwHasBone(Mesh, Hand))
		{
			return false;
		}
		const FVector H = CwBone(Mesh, Hand);
		const TCHAR* Index = bRight ? TEXT("index_01_r") : TEXT("index_01_l");
		const TCHAR* Pinky = bRight ? TEXT("pinky_01_r") : TEXT("pinky_01_l");
		const TCHAR* Middle = bRight ? TEXT("middle_01_r") : TEXT("middle_01_l");
		const TCHAR* IndexTip = bRight ? TEXT("index_03_r") : TEXT("index_03_l");
		FVector X, Fingers, Palm, Grip;
		if (CwHasBone(Mesh, Index) && CwHasBone(Mesh, Pinky) && CwHasBone(Mesh, Middle))
		{
			const FVector I = CwBone(Mesh, Index);
			const FVector K = CwBone(Mesh, Pinky);
			const FVector M = CwBone(Mesh, Middle);
			X = (I - K).GetSafeNormal();
			Fingers = (M - H).GetSafeNormal();
			Fingers = (Fingers - X * FVector::DotProduct(Fingers, X)).GetSafeNormal();
			Palm = FVector::CrossProduct(Fingers, X).GetSafeNormal();
			// Which side is the palm: the curled index finger's tip lies there; else by the hand's side.
			float Side = bRight ? 1.0f : -1.0f;
			if (CwHasBone(Mesh, IndexTip))
			{
				const float Curl = FVector::DotProduct(CwBone(Mesh, IndexTip) - I, Palm);
				if (FMath::Abs(Curl) > 0.5f)
				{
					Side = FMath::Sign(Curl);
				}
			}
			Palm *= Side;
			Grip = (I + K) * 0.5f + Palm * 2.5f + Fingers * 0.5f;
			// A held blade leans a little the way the fingers point (about 15 degrees), and never runs back along
			// the forearm (a twisted wrist pose or a noisy knuckle could flip it): its backward part is taken out.
			X = (X + Fingers * 0.27f).GetSafeNormal();
			const TCHAR* Elbow = bRight ? TEXT("lowerarm_r") : TEXT("lowerarm_l");
			if (CwHasBone(Mesh, Elbow))
			{
				const FVector Forearm = (H - CwBone(Mesh, Elbow)).GetSafeNormal();
				const double Back = FVector::DotProduct(X, Forearm);
				if (Back < -0.2)
				{
					X = (X - Forearm * (Back + 0.2)).GetSafeNormal();
				}
			}
			// Nor through its holder's own body (a hit reaction's twisted wrist pointed a two-handed blade back through the
			// chest): if the blade's line runs into the torso and the other way is clear, it goes the other way.
			if (CwHasBone(Mesh, TEXT("pelvis")) && CwHasBone(Mesh, TEXT("neck_01")))
			{
				const FVector Pelvis = CwBone(Mesh, TEXT("pelvis"));
				const FVector Neck = CwBone(Mesh, TEXT("neck_01"));
				auto IntoBody = [&](const FVector& Dir)
				{
					FVector P1, P2;
					FMath::SegmentDistToSegmentSafe(Grip + Dir * 12.0, Grip + Dir * 55.0, Pelvis, Neck, P1, P2);
					return FVector::Dist(P1, P2) < 13.0;
				};
				if (IntoBody(X) && !IntoBody(-X))
				{
					X = -X;
				}
			}
			Fingers = (Fingers - X * FVector::DotProduct(Fingers, X)).GetSafeNormal();
		}
		else
		{
			const FTransform Bone = Mesh->GetSocketTransform(FName(Hand), RTS_World);
			X = Bone.GetUnitAxis(EAxis::Y) * (bRight ? 1.0f : -1.0f);
			Fingers = Bone.GetUnitAxis(EAxis::X);
			Grip = H + Fingers * 8.0f;
		}
		Out = FTransform(FRotationMatrix::MakeFromXZ(X, Fingers).ToQuat(), Grip);
		return true;
	}

	/** The shield on the left forearm: its face (+z) out from the back of the arm, its x along the forearm. */
	bool CwShieldFrame(const USkeletalMeshComponent* Mesh, FTransform& Out)
	{
		if (!CwHasBone(Mesh, TEXT("lowerarm_l")) || !CwHasBone(Mesh, TEXT("hand_l")))
		{
			return false;
		}
		const FVector A = CwBone(Mesh, TEXT("lowerarm_l"));
		const FVector B = CwBone(Mesh, TEXT("hand_l"));
		const FVector Along = (B - A).GetSafeNormal();
		FTransform Hand;
		FVector Out3 = FVector::UpVector;
		if (CwGripFrame(Mesh, false, Hand))
		{
			// The back of the hand: away from the palm (the grip frame's palm side is -y x z... recomputed here).
			const FVector Fingers = Hand.GetUnitAxis(EAxis::Z);
			const FVector Across = Hand.GetUnitAxis(EAxis::X);
			FVector Palm = FVector::CrossProduct(Fingers, Across).GetSafeNormal();
			const FVector GripOffset = Hand.GetLocation() - B;
			if (FVector::DotProduct(GripOffset, Palm) < 0.0f)
			{
				Palm = -Palm;
			}
			Out3 = -Palm;
		}
		Out3 = (Out3 - Along * FVector::DotProduct(Out3, Along)).GetSafeNormal();
		const FVector Centre = FMath::Lerp(A, B, 0.55f) + Out3 * 5.5f;
		Out = FTransform(FRotationMatrix::MakeFromZX(Out3, Along).ToQuat(), Centre);
		return true;
	}

	UStaticMeshComponent* CwMakeWeaponComponent(ASimPawnActor& Pawn, UStaticMesh* Mesh)
	{
		UStaticMeshComponent* Weapon = NewObject<UStaticMeshComponent>(&Pawn);
		Weapon->SetStaticMesh(Mesh);
		Weapon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Weapon->SetGenerateOverlapEvents(false);
		Weapon->SetCanEverAffectNavigation(false);
		Weapon->SetCastShadow(true);
		Weapon->bReceivesDecals = true;
		Weapon->RegisterComponent();
		return Weapon;
	}

	void CwDropWeapons(FCwPerson& P)
	{
		for (TWeakObjectPtr<UStaticMeshComponent>* Comp : {&P.Right, &P.Left})
		{
			if (Comp->IsValid())
			{
				(*Comp)->DestroyComponent();
			}
			*Comp = nullptr;
		}
		P.HeldStyle = -1;
		P.bPrevValid = false;
	}

	struct FCwBlade
	{
		bool bValid = false;
		bool bShield = false;
		FVector Hilt = FVector::ZeroVector;
		FVector Tip = FVector::ZeroVector;
		FVector Centre = FVector::ZeroVector;  // shield
		FVector Normal = FVector::UpVector;    // shield
		float Radius = 30.0f;
	};

	/** Puts the held weapons where the hands are this frame and says where their blades are. */
	void CwPlaceWeapons(FCwState& S, FCwPerson& P, FCwBlade OutBlades[2])
	{
		OutBlades[0] = OutBlades[1] = FCwBlade();
		USkeletalMeshComponent* Mesh = P.Mesh.Get();
		if (!Mesh || P.HeldStyle < 0)
		{
			return;
		}
		const FCwStyleDef& Style = CwStyles[P.HeldStyle];
		for (int32 Hand = 0; Hand < 2; ++Hand)
		{
			UStaticMeshComponent* Comp = (Hand == 0 ? P.Right : P.Left).Get();
			const char* Def = Hand == 0 ? Style.RightDef : Style.LeftDef;
			if (!Comp || !Def)
			{
				continue;
			}
			float Hilt = 0.0f, Tip = 0.0f;
			CwWeaponMesh(S, Def, Hilt, Tip);
			const bool bShield = FCStringAnsi::Strcmp(Def, "Shield") == 0;
			FTransform Frame;
			if (!(bShield ? CwShieldFrame(Mesh, Frame) : CwGripFrame(Mesh, Hand == 0, Frame)))
			{
				continue;
			}
			const float* T = S.Grip[P.HeldStyle][Hand];
			const FTransform Tweak(FRotator(T[3], T[4], T[5]), FVector(T[0], T[1], T[2]));
			const FTransform World = Tweak * Frame;
			// Hung on the hand bone, so it moves with the hand between our updates.
			const FName Bone = bShield ? FName(TEXT("lowerarm_l")) : FName(Hand == 0 ? TEXT("hand_r") : TEXT("hand_l"));
			if (Comp->GetAttachParent() != Mesh || Comp->GetAttachSocketName() != Bone)
			{
				Comp->AttachToComponent(Mesh, FAttachmentTransformRules::KeepWorldTransform, Bone);
			}
			Comp->SetWorldTransform(World);
			FCwBlade& Blade = OutBlades[Hand];
			Blade.bValid = true;
			Blade.bShield = bShield;
			if (bShield)
			{
				Blade.Centre = World.GetLocation();
				Blade.Normal = World.GetUnitAxis(EAxis::Z);
				Blade.Radius = Tip;
			}
			else
			{
				Blade.Hilt = World.TransformPosition(FVector(Hilt, 0.0, 0.0));
				Blade.Tip = World.TransformPosition(FVector(Tip, 0.0, 0.0));
			}
		}
	}

	/** Holds the wanted style (or nothing) on the person's visible body. */
	void CwHold(FCwState& S, FCwPerson& P, int32 Wanted)
	{
		ASimPawnActor* Pawn = P.Pawn.Get();
		USkeletalMeshComponent* Mesh = Pawn ? (Pawn->GetMakeHumanMesh() ? Pawn->GetMakeHumanMesh() : Pawn->GetBody()) : nullptr;
		if (Mesh != P.Mesh.Get())
		{
			// A new body (gender switched): hang everything again.
			CwDropWeapons(P);
			P.Mesh = Mesh;
		}
		if (Wanted == P.HeldStyle)
		{
			return;
		}
		CwDropWeapons(P);
		if (Wanted < 0 || !Pawn || !Mesh)
		{
			return;
		}
		const FCwStyleDef& Style = CwStyles[Wanted];
		float Hilt = 0.0f, Tip = 0.0f;
		if (UStaticMesh* Right = CwWeaponMesh(S, Style.RightDef, Hilt, Tip))
		{
			P.Right = CwMakeWeaponComponent(*Pawn, Right);
		}
		if (Style.LeftDef)
		{
			if (UStaticMesh* Left = CwWeaponMesh(S, Style.LeftDef, Hilt, Tip))
			{
				P.Left = CwMakeWeaponComponent(*Pawn, Left);
			}
		}
		P.HeldStyle = Wanted;
		P.bPrevValid = false;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Blood.

	float CwGroundZ(UWorld* World, const FVector& At, const FCwPerson* A, const FCwPerson* B)
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(CreatorWeaponsGround), false);
		for (const FCwPerson* P : {A, B})
		{
			if (P && P->Pawn.IsValid())
			{
				Params.AddIgnoredActor(P->Pawn.Get());
			}
		}
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, At + FVector(0, 0, 20), At - FVector(0, 0, 400), ECC_Visibility, Params))
		{
			return static_cast<float>(Hit.ImpactPoint.Z);
		}
		return A && A->Pawn.IsValid() ? static_cast<float>(A->Pawn->GetActorLocation().Z) : static_cast<float>(At.Z) - 100.0f;
	}

	void CwFloorSpot(FCwState& S, UWorld* World, const FVector& At, float Radius, const FVector& Travel)
	{
		UMaterialInstanceDynamic* Mid = CwBloodMid(S, 0.95f);
		if (!Mid)
		{
			return;
		}
		CwBloodAge(Mid, 0.0f, 50.0f);
		const FVector2D Flat(Travel.X, Travel.Y);
		const float Stretch = 1.0f + FMath::Min(2.2f, static_cast<float>(Flat.Size()) / 350.0f);
		const float Yaw = Flat.IsNearlyZero() ? S.Random.FRandRange(0.0f, 360.0f) : FMath::RadiansToDegrees(FMath::Atan2(Flat.Y, Flat.X));
		// Pointing down; after the -90 pitch the decal's z runs along the yaw, so the spot is drawn out the way it flew.
		UDecalComponent* Decal = UGameplayStatics::SpawnDecalAtLocation(World, Mid, FVector(6.0f, Radius, Radius * Stretch), At + FVector(0, 0, 2), FRotator(-90.0f, Yaw, 0.0f), 0.0f);
		if (!Decal)
		{
			return;
		}
		Decal->SetFadeScreenSize(0.0f);
		Decal->SetSortOrder(S.DecalOrder++);
		FCwSpot Spot;
		Spot.Decal = Decal;
		Spot.Mid = Mid;
		S.Spots.Add(Spot);
		while (S.Spots.Num() > CwMaxFloorSpots)
		{
			CwDestroyDecal(S.Spots[0].Decal);
			S.Spots.RemoveAt(0);
		}
	}

	void CwSpray(FCwState& S, const FVector& At, const FVector& Dir, float Amount, float GroundZ)
	{
		const int32 Count = FMath::Clamp(FMath::RoundToInt(10.0f + Amount * 26.0f), 6, 44);
		const FVector Forward = Dir.GetSafeNormal();
		for (int32 i = 0; i < Count && S.Droplets.Num() < CwMaxDroplets; ++i)
		{
			FCwDroplet Drop;
			Drop.P = At + S.Random.GetUnitVector() * 1.5f;
			// Most go with the blow, in a cone, some thrown up and back off the blade, a few just fall.
			const float Kind = S.Random.FRand();
			FVector V;
			if (Kind < 0.7f)
			{
				V = S.Random.VRandCone(Forward, FMath::DegreesToRadians(32.0f)) * S.Random.FRandRange(140.0f, 480.0f) * (0.6f + Amount * 0.5f);
				V.Z += S.Random.FRandRange(20.0f, 140.0f);
			}
			else if (Kind < 0.9f)
			{
				V = S.Random.VRandCone(-Forward + FVector::UpVector, FMath::DegreesToRadians(40.0f)) * S.Random.FRandRange(80.0f, 220.0f);
			}
			else
			{
				V = S.Random.GetUnitVector() * 30.0f;
			}
			Drop.V = V;
			Drop.Size = S.Random.FRandRange(0.45f, 1.4f) * (Kind < 0.9f ? 1.0f : 1.4f);
			Drop.GroundZ = GroundZ;
			S.Droplets.Add(Drop);
		}
	}

	/** A stain on the body at the hit (and a run of blood below it), on the nearest bone so it moves with the body. */
	void CwStain(FCwState& S, FCwPerson& P, const FVector& At, const FVector& Dir, float Amount)
	{
		USkeletalMeshComponent* Mesh = P.Mesh.Get();
		if (!Mesh)
		{
			return;
		}
		const FName Bone = Mesh->FindClosestBone(At);
		const FVector Into = Dir.GetSafeNormal();
		// A blow near an older stain makes it bigger and fresh again.
		for (FCwStain& Old : P.Stains)
		{
			UDecalComponent* Decal = Old.Decal.Get();
			if (Decal && !Old.bStreak && Decal->GetAttachSocketName() == Bone && FVector::Dist(Decal->GetComponentLocation() + Into * 4.0f, At) < Old.Half)
			{
				Old.Half = FMath::Min(Old.Half * 1.25f, 13.0f);
				Old.Age = 0.0f;
				Decal->DecalSize = FVector(10.0f, Old.Half, Old.Half);
				Decal->MarkRenderStateDirty();
				return;
			}
		}
		const float Half = FMath::Clamp(4.5f + Amount * 4.0f, 4.0f, 10.0f);
		for (int32 Part = 0; Part < 2; ++Part)
		{
			UMaterialInstanceDynamic* Mid = CwBloodMid(S, Part == 0 ? 1.0f : 0.85f);
			if (!Mid)
			{
				return;
			}
			CwBloodAge(Mid, 0.0f, 45.0f);
			// The run: narrow and long, starting at the wound and running down (decal z is world up for a level blow).
			const FVector Where = Part == 0 ? At - Into * 4.0f : At - Into * 4.0f - FVector(0, 0, Half * 1.4f);
			const FVector Size = Part == 0 ? FVector(10.0f, Half, Half) : FVector(10.0f, Half * 0.35f, Half * 1.6f);
			FVector Flat = Into;
			Flat.Z = 0.0;
			const FRotator Rotation = (Flat.IsNearlyZero() ? Into : Flat.GetSafeNormal()).Rotation();
			UDecalComponent* Decal = UGameplayStatics::SpawnDecalAttached(Mid, Size, Mesh, Bone, Where, Rotation, EAttachLocation::KeepWorldPosition, 0.0f);
			if (!Decal)
			{
				return;
			}
			Decal->SetFadeScreenSize(0.0f);
			Decal->SetSortOrder(S.DecalOrder++);
			FCwStain Stain;
			Stain.Decal = Decal;
			Stain.Mid = Mid;
			Stain.Half = Half;
			Stain.bStreak = Part == 1;
			P.Stains.Add(Stain);
			if (Amount < 0.5f)
			{
				break;  // a light blow: no run
			}
		}
		while (P.Stains.Num() > CwMaxBodyStains)
		{
			CwDestroyDecal(P.Stains[0].Decal);
			P.Stains.RemoveAt(0);
		}
	}

	void CwBleed(FCwState& S, UWorld* World, FCwPerson& Victim, FCwPerson* Other, const FVector& At, const FVector& Dir, float Amount)
	{
		if (!S.bBlood)
		{
			return;
		}
		CwEnsureEffects(S, World);
		const float GroundZ = CwGroundZ(World, At, &Victim, Other);
		CwSpray(S, At, Dir, Amount, GroundZ);
		CwStain(S, Victim, At, Dir, Amount);
		UE_LOG(LogCreatorWeapons, Display, TEXT("Bleed %.2f at %s (ground %.0f): %d droplets, %d stains, %d spots, splat %s, droplet mesh %s"), Amount, *At.ToString(), GroundZ,
			S.Droplets.Num(), Victim.Stains.Num(), S.Spots.Num(), S.SplatBase.IsValid() ? *S.SplatBase->GetName() : TEXT("none"), S.DropletInstances.IsValid() ? TEXT("yes") : TEXT("no"));
		Victim.Blood += Amount;
		Victim.DripIn = FMath::Min(Victim.DripIn, 0.4f);
	}

	void CwCleanUp(FCwState& S)
	{
		for (FCwSpot& Spot : S.Spots)
		{
			CwDestroyDecal(Spot.Decal);
		}
		S.Spots.Reset();
		S.Droplets.Reset();
		for (FCwPerson& P : S.People)
		{
			for (FCwStain& Stain : P.Stains)
			{
				CwDestroyDecal(Stain.Decal);
			}
			P.Stains.Reset();
			CwDestroyDecal(P.Pool);
			P.PoolRadius = 0.0f;
			P.Blood = 0.0f;
			P.Hits = 0;
		}
		if (S.DropletInstances.IsValid())
		{
			S.DropletInstances->ClearInstances();
		}
	}

	void CwUpdateBlood(FCwState& S, UWorld* World, float Dt)
	{
		// Droplets fly, fall and leave a spot where they land (most of them).
		TArray<FTransform> Instances;
		Instances.Reserve(S.Droplets.Num());
		for (int32 i = S.Droplets.Num() - 1; i >= 0; --i)
		{
			FCwDroplet& Drop = S.Droplets[i];
			Drop.Age += Dt;
			Drop.V.Z -= 980.0f * Dt;
			Drop.V *= FMath::Max(0.0f, 1.0f - 0.35f * Dt);
			Drop.P += Drop.V * Dt;
			if (Drop.P.Z <= Drop.GroundZ + 0.3f || Drop.Age > 4.0f)
			{
				if (Drop.Age <= 4.0f && S.Random.FRand() < 0.75f)
				{
					CwFloorSpot(S, World, FVector(Drop.P.X, Drop.P.Y, Drop.GroundZ), FMath::Clamp(Drop.Size * S.Random.FRandRange(1.6f, 3.2f), 0.7f, 4.5f), Drop.V);
				}
				S.Droplets.RemoveAtSwap(i);
				continue;
			}
			const float Speed = static_cast<float>(Drop.V.Size());
			const FRotator Along = Speed > 1.0f ? Drop.V.Rotation() : FRotator::ZeroRotator;
			Instances.Add(FTransform(Along, Drop.P, FVector(Drop.Size * (1.0f + Speed / 140.0f), Drop.Size, Drop.Size)));
		}
		if (UInstancedStaticMeshComponent* Ism = S.DropletInstances.Get())
		{
			if (Ism->GetInstanceCount() > 0 || !Instances.IsEmpty())
			{
				Ism->ClearInstances();
				if (!Instances.IsEmpty())
				{
					Ism->AddInstances(Instances, false, true);
				}
			}
		}
		// Stains and spots dry: darker, duller (a few times a second is enough).
		S.StainUpdate -= Dt;
		const bool bAge = S.StainUpdate <= 0.0f;
		if (bAge)
		{
			S.StainUpdate = 0.25f;
		}
		for (FCwSpot& Spot : S.Spots)
		{
			Spot.Age += Dt;
			if (bAge)
			{
				CwBloodAge(Spot.Mid.Get(), Spot.Age, 50.0f);
			}
		}
		for (FCwPerson& P : S.People)
		{
			for (FCwStain& Stain : P.Stains)
			{
				Stain.Age += Dt;
				if (bAge)
				{
					CwBloodAge(Stain.Mid.Get(), Stain.Age, 45.0f);
				}
			}
			// Fresh wounds drip.
			P.DripIn -= Dt;
			if (P.DripIn <= 0.0f && S.bBlood)
			{
				P.DripIn = S.Random.FRandRange(0.45f, 1.3f);
				for (int32 i = P.Stains.Num() - 1, Done = 0; i >= 0 && Done < 3; --i)
				{
					const FCwStain& Stain = P.Stains[i];
					if (Stain.bStreak || Stain.Age > 25.0f || !Stain.Decal.IsValid() || S.Random.FRand() < 0.4f)
					{
						continue;
					}
					++Done;
					FCwDroplet Drop;
					Drop.P = Stain.Decal->GetComponentLocation() + Stain.Decal->GetForwardVector() * 4.0f - FVector(0, 0, Stain.Half);
					Drop.V = FVector(S.Random.FRandRange(-8.0f, 8.0f), S.Random.FRandRange(-8.0f, 8.0f), -30.0f);
					Drop.Size = S.Random.FRandRange(0.4f, 0.8f);
					Drop.GroundZ = P.Pawn.IsValid() ? static_cast<float>(P.Pawn->GetActorLocation().Z) : static_cast<float>(Drop.P.Z) - 100.0f;
					if (S.Droplets.Num() < CwMaxDroplets)
					{
						S.Droplets.Add(Drop);
					}
				}
			}
			// A pool under someone badly hurt (or dying), growing slowly to a size by how much they have bled.
			const bool bDown = P.Role == ECwRole::Dying || P.Role == ECwRole::Dead;
			const float Want = bDown ? 70.0f : (P.Blood >= 3.0f ? FMath::Min(10.0f + P.Blood * 3.0f, 32.0f) : 0.0f);
			USkeletalMeshComponent* Mesh = P.Mesh.Get();
			if (Want > 0.0f && Mesh && S.bBlood)
			{
				if (!P.Pool.IsValid())
				{
					CwEnsureEffects(S, World);
					UMaterialInstanceDynamic* Mid = CwBloodMid(S, 1.0f);
					if (Mid)
					{
						CwBloodAge(Mid, 0.0f, 80.0f);
						FVector Feet = Mesh->GetComponentLocation();
						if (CwHasBone(Mesh, TEXT("pelvis")))
						{
							const FVector Pelvis = CwBone(Mesh, TEXT("pelvis"));
							Feet.X = Pelvis.X;
							Feet.Y = Pelvis.Y;
						}
						Feet.Z = P.Pawn.IsValid() ? P.Pawn->GetActorLocation().Z : Feet.Z;
						P.PoolRadius = 6.0f;
						UDecalComponent* Pool = UGameplayStatics::SpawnDecalAtLocation(World, Mid, FVector(10.0f, P.PoolRadius, P.PoolRadius), Feet + FVector(0, 0, 2),
							FRotator(-90.0f, S.Random.FRandRange(0.0f, 360.0f), 0.0f), 0.0f);
						if (Pool)
						{
							Pool->SetFadeScreenSize(0.0f);
							Pool->SetSortOrder(1);
							P.Pool = Pool;
							P.PoolMid = Mid;
						}
					}
				}
				if (UDecalComponent* Pool = P.Pool.Get(); Pool && P.PoolRadius < Want)
				{
					P.PoolRadius = FMath::Min(Want, P.PoolRadius + Dt * (bDown ? 4.0f : 1.2f));
					Pool->DecalSize = FVector(10.0f, P.PoolRadius, P.PoolRadius * 1.15f);
					Pool->MarkRenderStateDirty();
				}
			}
		}
	}

	// ---------------------------------------------------------------------------------------------------------------
	// The blade test: the attacker's blades from last frame's place to this frame's, against the defender's shield,
	// blades and body capsules, and the world.

	enum class ECwContact : uint8 { None, Shield, Blade, Body, World };

	struct FCwHit
	{
		ECwContact Kind = ECwContact::None;
		FVector At = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;  // the blade's at the contact, cm/s
		float Fraction = 1.0f;                   // how far between last frame and this one
	};

	struct FCaps
	{
		FVector A, B;
		float R;
		bool bHead = false;
	};
	using FCwCaps = TArray<FCaps, TInlineAllocator<16>>;

	/** A body as capsules, from its pose now. */
	void CwBodyCapsules(const USkeletalMeshComponent* Mesh, FCwCaps& Caps)
	{
		Caps.Reset();
		if (!Mesh)
		{
			return;
		}
		for (const FCwCapsule& C : CwCapsules)
		{
			if (CwHasBone(Mesh, C.A) && CwHasBone(Mesh, C.B))
			{
				Caps.Add({CwBone(Mesh, C.A), CwBone(Mesh, C.B), C.Radius});
			}
		}
		if (CwHasBone(Mesh, TEXT("head")) && CwHasBone(Mesh, TEXT("neck_01")))
		{
			const FVector Head = CwBone(Mesh, TEXT("head"));
			const FVector Up = (Head - CwBone(Mesh, TEXT("neck_01"))).GetSafeNormal();
			Caps.Add({Head + Up * 4.0f, Head + Up * 13.0f, 10.0f, true});
		}
	}

	/**
	 * Whether the attacker's blades, where they are now, are in anything: the defender's body, shield or blades, or the
	 * world. A still test (no sweep), for finding the last moment before a contact.
	 */
	bool CwTouching(UWorld* World, const FCwPerson& Attacker, const FCwBlade Now[2], const FCwPerson& Defender, const FCwBlade DefBlades[2], const FCwCaps& Caps)
	{
		for (int32 Hand = 0; Hand < 2; ++Hand)
		{
			const FCwBlade& Blade = Now[Hand];
			if (!Blade.bValid || Blade.bShield)
			{
				continue;
			}
			for (const FCaps& C : Caps)
			{
				FVector P1, P2;
				FMath::SegmentDistToSegmentSafe(Blade.Hilt, Blade.Tip, C.A, C.B, P1, P2);
				// The head's capsule is rounder than the skull it stands for: a blade put back to it showed a little inside.
				if (FVector::Dist(P1, P2) < C.R + (C.bHead ? 2.0 : 0.5))
				{
					return true;
				}
			}
			for (int32 d = 0; d < 2; ++d)
			{
				const FCwBlade& Def = DefBlades[d];
				if (!Def.bValid)
				{
					continue;
				}
				if (Def.bShield)
				{
					const double DHilt = FVector::DotProduct(Blade.Hilt - Def.Centre, Def.Normal);
					const double DTip = FVector::DotProduct(Blade.Tip - Def.Centre, Def.Normal);
					for (int32 k = 0; k <= 8; ++k)
					{
						const FVector Pt = FMath::Lerp(Blade.Hilt, Blade.Tip, k / 8.0f);
						const double Dz = FVector::DotProduct(Pt - Def.Centre, Def.Normal);
						if (FMath::Abs(Dz) < 2.0 && ((Pt - Def.Centre) - Def.Normal * Dz).Size() < Def.Radius)
						{
							return true;
						}
					}
					if (DHilt * DTip < 0.0)
					{
						const FVector Cross = FMath::Lerp(Blade.Hilt, Blade.Tip, static_cast<float>(DHilt / (DHilt - DTip)));
						if (FVector::Dist(Cross, Def.Centre) < Def.Radius)
						{
							return true;
						}
					}
				}
				else
				{
					FVector P1, P2;
					FMath::SegmentDistToSegmentSafe(Blade.Hilt, Blade.Tip, Def.Hilt, Def.Tip, P1, P2);
					if (FVector::Dist(P1, P2) < 3.0f)
					{
						return true;
					}
				}
			}
			if (World)
			{
				FCollisionQueryParams Params(SCENE_QUERY_STAT(CreatorWeaponsBladeStill), false);
				Params.AddIgnoredActor(Attacker.Pawn.Get());
				Params.AddIgnoredActor(Defender.Pawn.Get());
				FHitResult Hit;
				if (World->LineTraceSingleByChannel(Hit, Blade.Hilt, Blade.Tip, ECC_Visibility, Params))
				{
					return true;
				}
			}
			if (Attacker.Pawn.IsValid() && Blade.Tip.Z < Attacker.Pawn->GetActorLocation().Z + 0.5)
			{
				return true;
			}
		}
		return false;
	}

	FCwHit CwSweep(UWorld* World, const FCwPerson& Attacker, const FCwBlade Now[2], const FCwPerson& Defender, const FCwBlade DefBlades[2], float Dt)
	{
		FCwHit Best;
		const USkeletalMeshComponent* DefMesh = Defender.Mesh.Get();
		if (!Attacker.bPrevValid || !DefMesh)
		{
			return Best;
		}
		// The defender's body as capsules, once.
		FCwCaps Caps;
		CwBodyCapsules(DefMesh, Caps);
		constexpr int32 Steps = 6;
		for (int32 Hand = 0; Hand < 2 && Best.Kind == ECwContact::None; ++Hand)
		{
			const FCwBlade& Blade = Now[Hand];
			if (!Blade.bValid || Blade.bShield)
			{
				continue;
			}
			const FVector H0 = Attacker.PrevHilt[Hand], T0 = Attacker.PrevTip[Hand];
			for (int32 Step = 1; Step <= Steps && Best.Kind == ECwContact::None; ++Step)
			{
				const float F = static_cast<float>(Step) / Steps;
				const FVector H = FMath::Lerp(H0, Blade.Hilt, F);
				const FVector T = FMath::Lerp(T0, Blade.Tip, F);
				const FVector Hp = FMath::Lerp(H0, Blade.Hilt, F - 1.0f / Steps);
				const FVector Tp = FMath::Lerp(T0, Blade.Tip, F - 1.0f / Steps);
				const FVector Velocity = (Blade.Tip - T0) / FMath::Max(Dt, 0.001f);
				// The shield: a disc; the blade's points close to its face, or crossing it, within its radius.
				for (int32 d = 0; d < 2 && Best.Kind == ECwContact::None; ++d)
				{
					const FCwBlade& Def = DefBlades[d];
					if (!Def.bValid)
					{
						continue;
					}
					if (Def.bShield)
					{
						for (int32 k = 0; k <= 8; ++k)
						{
							const float A = k / 8.0f;
							const FVector Pt = FMath::Lerp(H, T, A);
							const FVector Prev = FMath::Lerp(Hp, Tp, A);
							const double Dz = FVector::DotProduct(Pt - Def.Centre, Def.Normal);
							const double DzPrev = FVector::DotProduct(Prev - Def.Centre, Def.Normal);
							const FVector Radial = (Pt - Def.Centre) - Def.Normal * Dz;
							if ((FMath::Abs(Dz) < 3.0 || Dz * DzPrev < 0.0) && Radial.Size() < Def.Radius)
							{
								Best = {ECwContact::Shield, Def.Centre + Radial + Def.Normal * 2.0f, Velocity, F};
								break;
							}
						}
					}
					else
					{
						FVector P1, P2;
						FMath::SegmentDistToSegmentSafe(H, T, Def.Hilt, Def.Tip, P1, P2);
						if (FVector::Dist(P1, P2) < 4.0f)
						{
							Best = {ECwContact::Blade, (P1 + P2) * 0.5f, Velocity, F};
						}
					}
				}
				if (Best.Kind != ECwContact::None)
				{
					break;
				}
				for (const FCaps& C : Caps)
				{
					FVector P1, P2;
					FMath::SegmentDistToSegmentSafe(H, T, C.A, C.B, P1, P2);
					const double Dist = FVector::Dist(P1, P2);
					if (Dist < C.R + 1.5)
					{
						// On the skin, where the blade meets it.
						const FVector Out = (P1 - P2).GetSafeNormal();
						Best = {ECwContact::Body, P2 + (Out.IsNearlyZero() ? -Velocity.GetSafeNormal() : Out) * C.R, Velocity, F};
						break;
					}
				}
			}
			// The world (the ground, walls, anything solid), tip path and blade.
			if (Best.Kind == ECwContact::None && World)
			{
				FCollisionQueryParams Params(SCENE_QUERY_STAT(CreatorWeaponsBlade), false);
				Params.AddIgnoredActor(Attacker.Pawn.Get());
				Params.AddIgnoredActor(Defender.Pawn.Get());
				FHitResult Hit;
				if (World->LineTraceSingleByChannel(Hit, T0, Blade.Tip, ECC_Visibility, Params) || World->LineTraceSingleByChannel(Hit, Blade.Hilt, Blade.Tip, ECC_Visibility, Params))
				{
					Best = {ECwContact::World, Hit.ImpactPoint, (Blade.Tip - T0) / FMath::Max(Dt, 0.001f), FMath::Clamp(Hit.Time, 0.0f, 1.0f)};
				}
				else if (Attacker.Pawn.IsValid() && Blade.Tip.Z < Attacker.Pawn->GetActorLocation().Z + 1.0)
				{
					Best = {ECwContact::World, FVector(Blade.Tip.X, Blade.Tip.Y, Attacker.Pawn->GetActorLocation().Z), (Blade.Tip - T0) / FMath::Max(Dt, 0.001f), 1.0f};
				}
			}
		}
		return Best;
	}

	/** Which side a blow came from, for the defender: 0 front, 1 right, 2 back, 3 left. */
	int32 CwSide(const FCwPerson& Defender, const FVector& From)
	{
		const ASimPawnActor* Pawn = Defender.Pawn.Get();
		if (!Pawn)
		{
			return 0;
		}
		// Where the body faces: the demo's facing while it stands them (it turns the mesh, not the pawn), else the pawn's.
		const FVector Fwd = Defender.bMoved ? Defender.Facing : Pawn->GetActorForwardVector();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Fwd);
		FVector Dir = From;
		Dir.Z = 0.0;
		Dir.Normalize();
		const double F = FVector::DotProduct(Dir, Fwd);
		const double R = FVector::DotProduct(Dir, Right);
		if (FMath::Abs(F) >= FMath::Abs(R))
		{
			return F >= 0.0 ? 0 : 2;
		}
		return R >= 0.0 ? 1 : 3;
	}
}

namespace
{
	// ---------------------------------------------------------------------------------------------------------------
	// Preview and fight demo.

	struct FCwPending
	{
		ECwContact Kind = ECwContact::None;
		int32 Side = 0;
		bool bStrong = false;
		bool bDeath = false;
		bool bStumble = false;  // the heaviest blow: StumbleBackwards (down) whatever the side, then getting up
	};
	FCwPending GCwPending;
	bool GCwPreviewDirty = true;

	bool CwIsParticipant(const FCwState& S, int32 i)
	{
		return S.Demo != ECwDemo::Off ? (i == S.DemoA || i == S.DemoB) : i == S.Selected;
	}

	void CwPlayStance(FCwState& S, FCwPerson& P)
	{
		if (P.HeldStyle < 0)
		{
			return;
		}
		CwPlay(S, P, CwStyles[P.HeldStyle].Stance, 1.0f, true, 0.3f, 0.3f);
		P.Role = ECwRole::Stance;
		P.RoleTime = 0.0f;
	}

	/** Stands a demo fighter at its spot facing the other: the visible mesh moves and turns, the pawn stays. */
	void CwStand(FCwPerson& P)
	{
		USkeletalMeshComponent* Mesh = P.Mesh.Get();
		ASimPawnActor* Pawn = P.Pawn.Get();
		if (!Mesh || !Pawn)
		{
			return;
		}
		if (!P.bMoved)
		{
			P.SavedRelative = Mesh->GetRelativeTransform();
			P.bMoved = true;
		}
		const USceneComponent* Parent = Mesh->GetAttachParent();
		const FTransform Base = P.SavedRelative * (Parent ? Parent->GetComponentTransform() : FTransform::Identity);
		const float Delta = static_cast<float>(P.Facing.Rotation().Yaw - Pawn->GetActorRotation().Yaw);
		const FQuat Rotation = FQuat(FVector::UpVector, FMath::DegreesToRadians(Delta)) * Base.GetRotation();
		FVector Shift = P.Spot - Pawn->GetActorLocation();
		Shift.Z = 0.0;
		Mesh->SetWorldLocationAndRotation(Base.GetLocation() + Shift, Rotation);
	}

	void CwUnstand(FCwPerson& P)
	{
		if (P.bMoved && P.Mesh.IsValid())
		{
			P.Mesh->SetRelativeTransform(P.SavedRelative);
		}
		P.bMoved = false;
	}

	void CwStopDemo(FCwState& S)
	{
		if (S.Demo == ECwDemo::Off)
		{
			return;
		}
		S.Demo = ECwDemo::Off;
		for (FCwPerson& P : S.People)
		{
			CwUnstand(P);
			CwStopMontage(P, 0.4f);
			P.Role = ECwRole::Idle;
		}
		GCwPreviewDirty = true;
	}

	bool CwStartDemo(FCwState& S)
	{
		const int32 Count = S.People.Num();
		if (Count < 2 || !S.People.IsValidIndex(S.Selected))
		{
			S.LastEvent = TEXT("The demo needs two people.");
			return false;
		}
		S.DemoA = S.Selected;
		S.DemoB = S.Selected + 1 < Count ? S.Selected + 1 : S.Selected - 1;
		if (!S.People[S.DemoA].Pawn.IsValid() || !S.People[S.DemoB].Pawn.IsValid())
		{
			S.LastEvent = TEXT("The demo needs two people.");
			return false;
		}
		for (FCwPerson& P : S.People)
		{
			CwStopMontage(P, 0.3f);
			P.Role = ECwRole::Idle;
			P.Hits = 0;
			P.GiveGround = 0.0f;
			P.bFloored = false;
			P.AnchorT = 0.0f;
			P.bPelvisAlong = false;
		}
		S.RawGap = -1.0f;
		S.GapRate = 0.0f;
		S.SwingMinGap = 1.0e6f;
		S.Demo = ECwDemo::Pause;
		S.DemoTime = 1.2f;
		S.Attacker = 0;
		S.bFinish = false;
		S.Swings = S.BodyHits = S.Blocks = S.WorldHits = S.Misses = 0;
		S.LastEvent = TEXT("Squaring up.");
		return true;
	}

	/** The demo spots each frame: either side of the middle between the two pawns, facing each other. */
	void CwPlaceFighters(FCwState& S, float Dt)
	{
		FCwPerson& A = S.People[S.DemoA];
		FCwPerson& B = S.People[S.DemoB];
		const FVector PA = A.Pawn->GetActorLocation();
		const FVector PB = B.Pawn->GetActorLocation();
		FVector Dir = PB - PA;
		Dir.Z = 0.0;
		Dir = Dir.IsNearlyZero() ? FVector(0.0, 1.0, 0.0) : Dir.GetSafeNormal();
		const FVector Mid = (PA + PB) * 0.5;
		// A lunge can carry one body into the other (the clips step forward). The defender gives ground: its spot
		// slides back before they meet, from how fast the pelvises close (where they will be in a quarter second), so
		// they keep at least 55 cm apart along the line; it drifts back afterwards. The attacker's spot stays.
		FCwPerson& Def = S.Attacker == 0 ? B : A;
		constexpr float MinGap = 55.0f;
		if (A.Mesh.IsValid() && B.Mesh.IsValid() && CwHasBone(A.Mesh.Get(), TEXT("pelvis")) && CwHasBone(B.Mesh.Get(), TEXT("pelvis")) && Dt > 0.0f)
		{
			// Getting up starts lying face down with the pelvis at the clip's origin, while the stumble left it a metre or
			// more back: for a moment after such a change the spot moves to keep the pelvis where it was (else the body
			// slid into the other one as the clips blended).
			for (FCwPerson* P : {&A, &B})
			{
				const float Along = static_cast<float>(FVector::DotProduct(CwBone(P->Mesh.Get(), TEXT("pelvis")) - P->Spot, P->Facing));
				if (P->AnchorT > 0.0f && P->bPelvisAlong)
				{
					const float Moved = Along - P->PelvisAlong;
					P->GiveGround = FMath::Clamp(P->GiveGround + Moved, 0.0f, 250.0f);
					if (S.RawGap >= 0.0f)
					{
						S.RawGap -= Moved;  // our own sliding, not the bodies closing
					}
				}
				P->AnchorT = FMath::Max(0.0f, P->AnchorT - Dt);
				P->PelvisAlong = Along;
				P->bPelvisAlong = true;
			}
			const float Gap = static_cast<float>(FVector::DotProduct(CwBone(B.Mesh.Get(), TEXT("pelvis")) - CwBone(A.Mesh.Get(), TEXT("pelvis")), Dir));
			// The bodies' own closing, without our sliding (the spots move the pelvises by exactly the give-ground).
			const float Raw = Gap - A.GiveGround - B.GiveGround;
			if (S.RawGap >= 0.0f)
			{
				S.GapRate = FMath::FInterpTo(S.GapRate, (Raw - S.RawGap) / Dt, Dt, 12.0f);
			}
			S.RawGap = Raw;
			S.SwingMinGap = FMath::Min(S.SwingMinGap, Gap);
			const float Ahead = Gap + FMath::Min(S.GapRate, 0.0f) * 0.25f;
			const float Want = FMath::Max(0.0f, MinGap - FMath::Min(Gap, Ahead));
			if (Gap < MinGap)
			{
				Def.GiveGround += MinGap - Gap;  // already too close: at once
			}
			else if (Want > 0.0f)
			{
				Def.GiveGround += FMath::Min(Want, 400.0f * Dt);  // stepping back ahead of the lunge
			}
			Def.GiveGround = FMath::Min(Def.GiveGround, 250.0f);
			// Drift back (a step or two in, about 70 cm a second) while there is room to spare, nothing is closing and
			// that one is not reacting, down or getting up (a body lying face down slid back across the grass): given ground
			// left at 25 cm a second kept them 3 m apart and swings missed.
			for (FCwPerson* P : {&A, &B})
			{
				if (Want <= 0.0f && Gap > MinGap + 10.0f && S.GapRate > -20.0f && P->GiveGround > 0.0f && P->AnchorT <= 0.0f && !P->bFloored &&
					P->Role != ECwRole::React && P->Role != ECwRole::Dying && P->Role != ECwRole::Dead)
				{
					P->GiveGround = FMath::Max(0.0f, P->GiveGround - FMath::Min(70.0f * Dt, Gap - MinGap - 10.0f));
				}
			}
			const float Total = A.GiveGround + B.GiveGround;
			if (Total > S.MostApart + 5.0f)
			{
				S.MostApart = Total;
				UE_LOG(LogCreatorWeapons, Display, TEXT("Giving ground: pelvises %.0f cm apart, closing %.0f cm/s, %.0f cm in a quarter second; spots %.0f cm further apart"), Gap,
					-S.GapRate, Ahead, Total);
			}
			S.ExtraApart = Total;
		}
		const float Half = S.Distance * 0.5f;
		A.Spot = Mid - Dir * (Half + A.GiveGround);
		B.Spot = Mid + Dir * (Half + B.GiveGround);
		A.Facing = Dir;
		B.Facing = -Dir;
		CwStand(A);
		CwStand(B);
	}

	int32 CwPickAttack(FCwState& S, int32 Style)
	{
		const FCwStyleDef& Def = CwStyles[Style];
		if (S.bPickedOnly && S.Attack >= 0 && S.Attack < Def.AttackCount && !Def.Attacks[S.Attack].bBlock)
		{
			return S.Attack;
		}
		for (int32 Try = 0; Try < 16; ++Try)
		{
			const int32 i = S.Random.RandRange(0, Def.AttackCount - 1);
			if (!Def.Attacks[i].bBlock)
			{
				return i;
			}
		}
		return 0;
	}

	/**
	 * Poses a fighter again now, in the middle of the frame: its hidden mannequin (where the clips play) and then the
	 * visible body that copies it. The new pose is the one drawn this frame.
	 */
	void CwEvaluateNow(FCwPerson& P)
	{
		ASimPawnActor* Pawn = P.Pawn.Get();
		USkeletalMeshComponent* Body = Pawn ? Pawn->GetBody() : nullptr;
		USkeletalMeshComponent* Visible = P.Mesh.Get();
		if (Body)
		{
			Body->TickAnimation(0.0f, false);
			Body->RefreshBoneTransforms();
		}
		if (Visible && Visible != Body)
		{
			Visible->TickAnimation(0.0f, false);
			Visible->RefreshBoneTransforms();
		}
	}

	/**
	 * After the animation of a frame in which the attacker's blade went into something: its swing is put back to the
	 * last moment the blade was clear (a few halvings between last frame's place and this one) and posed again, so
	 * this very frame shows the blade at the surface. Returns how many poses it tried.
	 */
	int32 CwRewindToContact(FCwState& S, UWorld* World, FCwPerson& Att, const FCwPerson& Def, const FCwBlade DefBlades[2], float Fraction, FCwBlade OutBlades[2])
	{
		const float Now = CwMontagePos(Att);
		float Clear = Att.PrevMontagePos <= Now ? FMath::Max(0.0f, Att.PrevMontagePos) : FMath::Max(0.0f, Now - 0.05f);
		float Inside = Now;
		FCwCaps Caps;
		CwBodyCapsules(Def.Mesh.Get(), Caps);
		int32 Tries = 0;
		float Try = Clear + (Inside - Clear) * FMath::Clamp(Fraction, 0.0f, 1.0f);
		for (int32 Step = 0; Step < 6 && Inside - Clear > 0.0005f; ++Step)
		{
			CwSetPos(Att, Try);
			CwEvaluateNow(Att);
			CwPlaceWeapons(S, Att, OutBlades);
			++Tries;
			if (CwTouching(World, Att, OutBlades, Def, DefBlades, Caps))
			{
				Inside = Try;
			}
			else
			{
				Clear = Try;
			}
			Try = (Clear + Inside) * 0.5f;
		}
		// End on the clear pose (the last one tried may have been inside).
		if (!FMath::IsNearlyEqual(CwMontagePos(Att), Clear, 0.0001f) || Tries == 0)
		{
			CwSetPos(Att, Clear);
			CwEvaluateNow(Att);
			CwPlaceWeapons(S, Att, OutBlades);
			++Tries;
		}
		return Tries;
	}

	void CwResolve(FCwState& S, UWorld* World, FCwPerson& Att, FCwPerson& Def, const FCwBlade DefBlades[2], FCwHit Hit, FCwBlade* AttBlades = nullptr)
	{
		const FCwStyleDef& Style = CwStyles[Att.HeldStyle];
		const float Heavy = Style.Attacks[S.SwingAttack].Heavy;
		UFightSubsystem* Fight = World->GetSubsystem<UFightSubsystem>();
		const FVector Travel = Hit.Velocity.IsNearlyZero() ? (Def.Spot - Att.Spot).GetSafeNormal() : Hit.Velocity.GetSafeNormal();
		// Guarding from the front: a blow that got to the body is taken on the weapon instead.
		if (Hit.Kind == ECwContact::Body && S.bBlocking && !S.bForceStagger && CwSide(Def, -Travel) == 0)
		{
			for (int32 d = 0; d < 2; ++d)
			{
				const FCwBlade& B = DefBlades[d];
				if (B.bValid)
				{
					Hit.Kind = B.bShield ? ECwContact::Shield : ECwContact::Blade;
					Hit.At = B.bShield ? B.Centre + B.Normal * 2.0f : FMath::ClosestPointOnSegment(Hit.At, B.Hilt, B.Tip);
					break;
				}
			}
		}
		// Back to where the blade touched, posed again this frame, so it never shows inside what it hit.
		CwSetRate(Att, 0.0f);
		CwSetRate(Def, 0.0f);
		if (AttBlades)
		{
			const int32 Tries = CwRewindToContact(S, World, Att, Def, DefBlades, Hit.Fraction, AttBlades);
			if (Hit.Kind == ECwContact::Body)
			{
				// The wound where the blade now rests: the nearest point of the body to it, on the skin.
				FCwCaps Caps;
				CwBodyCapsules(Def.Mesh.Get(), Caps);
				double Nearest = TNumericLimits<double>::Max();
				for (int32 Hand = 0; Hand < 2; ++Hand)
				{
					if (!AttBlades[Hand].bValid || AttBlades[Hand].bShield)
					{
						continue;
					}
					for (const FCaps& C : Caps)
					{
						FVector P1, P2;
						FMath::SegmentDistToSegmentSafe(AttBlades[Hand].Hilt, AttBlades[Hand].Tip, C.A, C.B, P1, P2);
						const double Gap = FVector::Dist(P1, P2) - C.R;
						if (Gap < Nearest)
						{
							Nearest = Gap;
							const FVector Out = (P1 - P2).GetSafeNormal();
							Hit.At = Out.IsNearlyZero() ? P1 : P2 + Out * C.R;
						}
					}
				}
			}
			UE_LOG(LogCreatorWeapons, Display, TEXT("Contact %d: swing put back to %.3f s, posed again %d times this frame"), static_cast<int32>(Hit.Kind), CwMontagePos(Att), Tries);
		}
		GCwPending = FCwPending();
		GCwPending.Kind = Hit.Kind;
		GCwPending.Side = CwSide(Def, -Travel);
		S.DemoTime = CwHitStopSeconds;
		switch (Hit.Kind)
		{
			case ECwContact::Shield:
			case ECwContact::Blade:
				++S.Blocks;
				S.LastEvent = Hit.Kind == ECwContact::Shield ? TEXT("Taken on the shield.") : TEXT("Blade on blade.");
				if (Fight)
				{
					// Steel on steel or iron-rimmed wood: sparks (and chips off a wooden club or board).
					const bool bWood = Att.HeldStyle == static_cast<int32>(ECwStyle::Club) || Hit.Kind == ECwContact::Shield;
					Fight->SpawnMeleeImpact(bWood ? colony::ProjectileHitKind::Building : colony::ProjectileHitKind::Armor, colony::kNoEntity, colony::kNoEntity, Hit.At, Travel, -1, 10.0f);
				}
				break;
			case ECwContact::World:
				++S.WorldHits;
				S.LastEvent = TEXT("Struck the ground.");
				if (Fight)
				{
					Fight->SpawnMeleeImpact(colony::ProjectileHitKind::Ground, colony::kNoEntity, colony::kNoEntity, Hit.At, Travel, -1, 10.0f);
				}
				break;
			case ECwContact::Body:
			{
				++S.BodyHits;
				++Def.Hits;
				const bool bClub = Att.HeldStyle == static_cast<int32>(ECwStyle::Club);
				const float Amount = (0.45f + Heavy * 0.75f) * (bClub ? 0.55f : 1.0f) * S.Random.FRandRange(0.8f, 1.2f);
				CwBleed(S, World, Def, &Att, Hit.At, Travel, Amount);
				GCwPending.bDeath = S.bFinish;
				GCwPending.bStrong = Heavy * S.Random.FRandRange(0.6f, 1.3f) > 0.75f || (Def.Hits >= 4 && S.Random.FRand() < 0.5f);
				// The heaviest blows (a heavy attack that staggers, now and then) knock the defender down.
				GCwPending.bStumble = GCwPending.bStrong && !GCwPending.bDeath && Heavy >= 0.85f && S.Random.FRand() < 0.35f;
				if (S.bForceStagger && !GCwPending.bDeath)
				{
					S.bForceStagger = false;
					GCwPending.bStrong = true;
					GCwPending.bStumble = true;
					S.StaggerT = 0.0f;
					S.StaggerShots = 0;
					UE_LOG(LogCreatorWeapons, Display, TEXT("Heavy stagger: StumbleBackwards (fighters pushed %.0f cm further apart so far at most)"), S.MostApart);
				}
				S.DemoTime = GCwPending.bStrong ? 0.12f : CwHitStopSeconds;
				if (S.ScriptT >= 0.0f && S.ScriptHitShots < 6)
				{
					S.PendingShot = FString::Printf(TEXT("e_hit_%d"), ++S.ScriptHitShots);
				}
				S.LastEvent = GCwPending.bDeath ? TEXT("A finishing blow.") : (GCwPending.bStrong ? TEXT("A heavy hit: staggered.") : TEXT("A hit."));
				break;
			}
			default:
				break;
		}
		Att.Role = ECwRole::HitStop;
		S.Demo = ECwDemo::Impact;
	}

	void CwDefenderReacts(FCwState& S, FCwPerson& Def)
	{
		const FCwStyleDef& Style = CwStyles[FMath::Max(Def.HeldStyle, 0)];
		switch (GCwPending.Kind)
		{
			case ECwContact::Shield:
			case ECwContact::Blade:
				if (CwPlay(S, Def, Style.BlockReact, 1.0f, false, 0.08f, 0.25f))
				{
					Def.Role = ECwRole::React;
				}
				else
				{
					CwSetRate(Def, 1.0f);
				}
				break;
			case ECwContact::Body:
			{
				const int32 Side = GCwPending.Side;
				const TCHAR* Clip = nullptr;
				if (GCwPending.bDeath)
				{
					Clip = Side == 0 ? CwPick(S, Style.Deaths, 2) : CwDeathBySide[Side];
					if (CwPlay(S, Def, Clip, 1.0f, false, 0.1f, 0.5f))
					{
						Def.Role = ECwRole::Dying;
						Def.RoleTime = 0.0f;
						return;
					}
				}
				if (GCwPending.bStumble && CwPlay(S, Def, CwStumble, 1.0f, false, 0.06f, 0.3f))
				{
					Def.Role = ECwRole::React;
					Def.bFloored = true;
					break;
				}
				if (Side == 0)
				{
					Clip = GCwPending.bStrong && S.Random.FRand() < 0.6f ? CwReactLarge[0] : CwPick(S, Style.Impacts, 5);
				}
				else
				{
					Clip = GCwPending.bStrong ? CwReactLarge[Side] : CwReactSmall[Side];
				}
				if (CwPlay(S, Def, Clip, 1.0f, false, 0.06f, 0.3f))
				{
					Def.Role = ECwRole::React;
					Def.bFloored = false;
				}
				break;
			}
			default:
				CwSetRate(Def, 1.0f);
				break;
		}
	}

	struct FCwBlades
	{
		FCwBlade H[2];
	};

	void CwUpdateDemo(FCwState& S, UWorld* World, float Dt)
	{
		FCwPerson& A = S.People[S.DemoA];
		FCwPerson& B = S.People[S.DemoB];
		FCwPerson& Att = S.Attacker == 0 ? A : B;
		FCwPerson& Def = S.Attacker == 0 ? B : A;
		const int32 AttIndex = S.Attacker == 0 ? S.DemoA : S.DemoB;
		if (Att.HeldStyle < 0 || Def.HeldStyle < 0)
		{
			return;  // a body without hands (not a MakeHuman or mannequin mesh)
		}
		S.DemoTime -= Dt;
		// Whoever finished a reaction or a guard goes back to the stance.
		for (FCwPerson* P : {&A, &B})
		{
			P->RoleTime += Dt;
			if (P->Role == ECwRole::React && P->bFloored && CwMontageFraction(*P) >= 0.88f)
			{
				// Down: getting up from where the fall left it (blended in), and only then the stance.
				P->bFloored = false;
				if (!CwPlay(S, *P, CwGetUp, 1.0f, false, 0.3f, 0.4f))
				{
					CwPlayStance(S, *P);
				}
				P->AnchorT = 0.6f;
				UE_LOG(LogCreatorWeapons, Display, TEXT("Getting up after the stumble"));
			}
			else if ((P->Role == ECwRole::React && CwMontageFraction(*P) >= 0.92f) || P->Role == ECwRole::Idle)
			{
				CwPlayStance(S, *P);
			}
		}
		switch (S.Demo)
		{
			case ECwDemo::Pause:
				if (S.DemoTime <= 0.0f && Att.Role != ECwRole::React && Def.Role != ECwRole::React)
				{
					S.SwingAttack = CwPickAttack(S, Att.HeldStyle);
					const FCwStyleDef& Style = CwStyles[Att.HeldStyle];
					if (CwPlay(S, Att, Style.Attacks[S.SwingAttack].Clip, S.Speed, false, 0.15f, 0.3f))
					{
						Att.Role = ECwRole::Attack;
						if (S.Swings > 0 && S.SwingMinGap < 1.0e5f)
						{
							UE_LOG(LogCreatorWeapons, Display, TEXT("Swing %d: pelvises came within %.0f cm"), S.Swings, S.SwingMinGap);
						}
						S.SwingMinGap = 1.0e6f;
						++S.Swings;
						const FCwStyleDef& DefStyle = CwStyles[Def.HeldStyle];
						S.bBlocking = !S.bFinish && !S.bForceStagger && DefStyle.Blocks[0] && S.Random.FRand() < 0.35f;
						if (S.bBlocking && CwPlay(S, Def, CwPick(S, DefStyle.Blocks, 3), 1.0f, true, 0.12f, 0.3f))
						{
							Def.Role = ECwRole::Block;
						}
						S.LastEvent = FString::Printf(TEXT("%d attacks: %s%s"), AttIndex + 1, UTF8_TO_TCHAR(Style.Attacks[S.SwingAttack].Label), S.bBlocking ? TEXT(", the other guards") : TEXT(""));
						S.Demo = ECwDemo::Swing;
					}
					else
					{
						S.DemoTime = 1.0f;
						S.LastEvent = TEXT("The attack's clip is missing (MakeHumanArt -Clips).");
					}
				}
				break;
			case ECwDemo::Swing:
				// The blade test runs after the animation (CwSwingAfterAnimation).
				break;
			case ECwDemo::Impact:
				if (S.DemoTime <= 0.0f)
				{
					// The blow bounces back off what it hit instead of following through.
					CwSetRate(Att, -0.8f);
					Att.Role = ECwRole::Recoil;
					CwDefenderReacts(S, Def);
					S.Demo = ECwDemo::Recover;
					S.DemoTime = CwRecoilSeconds;
				}
				break;
			case ECwDemo::Recover:
				if (S.DemoTime <= 0.0f)
				{
					CwStopMontage(Att, 0.3f);
					CwPlayStance(S, Att);
					if (Def.Role == ECwRole::Dying)
					{
						S.Demo = ECwDemo::DeathHold;
						S.DemoTime = 30.0f;
					}
					else
					{
						if (Def.Role == ECwRole::Block)
						{
							CwPlayStance(S, Def);
						}
						S.Attacker ^= 1;
						S.Demo = ECwDemo::Pause;
						S.DemoTime = 0.9f + (GCwPending.bStrong ? 0.6f : 0.0f);
					}
				}
				break;
			case ECwDemo::DeathHold:
				if (Def.Role == ECwRole::Dying && CwMontageFraction(Def) >= 0.95f)
				{
					// Lie still at the end of the fall.
					CwSetRate(Def, 0.0f);
					Def.Role = ECwRole::Dead;
					S.DemoTime = 4.0f;
					S.LastEvent = TEXT("Down.");
				}
				else if (Def.Role == ECwRole::Dead && S.DemoTime <= 0.0f)
				{
					// The demo goes on: up again, blood kept.
					// Getting up from the floor (the stance follows when it ends), not a blend from lying to standing.
					if (CwPlay(S, Def, CwGetUp, 1.0f, false, 0.4f, 0.4f))
					{
						Def.Role = ECwRole::React;
						Def.bFloored = false;
						Def.AnchorT = 0.7f;
					}
					else
					{
						CwStopMontage(Def, 0.9f);
						CwPlayStance(S, Def);
					}
					Def.Hits = 0;
					S.bFinish = false;
					S.Attacker ^= 1;
					S.Demo = ECwDemo::Pause;
					S.DemoTime = 1.8f;
					S.LastEvent = TEXT("Up again.");
				}
				else if (S.DemoTime <= 0.0f)
				{
					CwPlayStance(S, Def);
					S.Demo = ECwDemo::Pause;
					S.DemoTime = 1.0f;
				}
				break;
			default:
				break;
		}
	}

	/**
	 * The swing, after this frame's animation: the attacker's blades from last frame to now against the defender, and
	 * on a contact the stop, posed back to the contact before anything is drawn.
	 */
	void CwSwingAfterAnimation(FCwState& S, UWorld* World, float Dt, TArray<FCwBlades>& Blades)
	{
		FCwPerson& A = S.People[S.DemoA];
		FCwPerson& B = S.People[S.DemoB];
		FCwPerson& Att = S.Attacker == 0 ? A : B;
		FCwPerson& Def = S.Attacker == 0 ? B : A;
		const int32 AttIndex = S.Attacker == 0 ? S.DemoA : S.DemoB;
		const int32 DefIndex = S.Attacker == 0 ? S.DemoB : S.DemoA;
		if (Att.HeldStyle < 0 || Def.HeldStyle < 0)
		{
			return;
		}
		switch (S.Demo)
		{
			case ECwDemo::Swing:
			{
				const float Fraction = CwMontageFraction(Att);
				if (Fraction > 0.1f && Fraction < 0.9f)
				{
					const FCwHit Hit = CwSweep(World, Att, Blades[AttIndex].H, Def, Blades[DefIndex].H, Dt);
					if (Hit.Kind != ECwContact::None)
					{
						CwResolve(S, World, Att, Def, Blades[DefIndex].H, Hit, Blades[AttIndex].H);
						break;
					}
				}
				if (Fraction >= 0.97f)
				{
					if (S.bFinish && Blades[AttIndex].H[0].bValid && Def.Mesh.IsValid() && CwHasBone(Def.Mesh.Get(), TEXT("spine_03")))
					{
						// Finishing: the last blow lands even if this swing fell short.
						FCwHit Forced;
						Forced.Kind = ECwContact::Body;
						Forced.At = CwBone(Def.Mesh.Get(), TEXT("spine_03")) + Def.Facing * 12.0f;
						Forced.Velocity = -Def.Facing * 300.0f;
						Forced.Fraction = 1.0f;
						S.bBlocking = false;
						CwResolve(S, World, Att, Def, Blades[DefIndex].H, Forced);
						break;
					}
					++S.Misses;
					S.LastEvent = TEXT("Missed.");
					CwPlayStance(S, Att);
					if (Def.Role == ECwRole::Block)
					{
						CwPlayStance(S, Def);
					}
					S.Attacker ^= 1;
					S.Demo = ECwDemo::Pause;
					S.DemoTime = 0.8f;
				}
				break;
			}
			default:
				break;
		}
	}

	/**
	 * The left hand on a two-handed hilt: a point on the grip below the right hand, given to the visible body's
	 * animation in the right hand bone's space (so it follows the right hand exactly in the next pose), the elbow out
	 * to the side and down. Eases in and out.
	 */
	void CwUpdateLeftGrip(FCwState& S, FCwPerson& P, float Dt)
	{
		USkeletalMeshComponent* Mesh = P.Mesh.Get();
		UMakeHumanAnimInstance* Anim = Mesh ? Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
		if (!Anim)
		{
			P.LeftGrip = 0.0f;
			return;
		}
		const bool bWant = P.HeldStyle == static_cast<int32>(ECwStyle::GreatSword) && P.Role != ECwRole::Dying && P.Role != ECwRole::Dead &&
			CwHasBone(Mesh, TEXT("upperarm_l")) && CwHasBone(Mesh, TEXT("upperarm_r")) && CwHasBone(Mesh, TEXT("pelvis")) && CwHasBone(Mesh, TEXT("neck_01"));
		P.LeftGrip = FMath::FInterpConstantTo(P.LeftGrip, bWant ? 1.0f : 0.0f, Dt, 4.0f);
		FTransform Right, Left;
		if (P.LeftGrip <= 0.0f || P.HeldStyle < 0 || !CwGripFrame(Mesh, true, Right) || !CwGripFrame(Mesh, false, Left))
		{
			Anim->SetLeftHandGrip(0.0f, FVector::ZeroVector, FVector::ZeroVector);
			return;
		}
		// The hilt point: below the right fist along the grip (the grip tweak included, as the sword is placed).
		const float* T = S.Grip[P.HeldStyle][0];
		const FTransform Held = FTransform(FRotator(T[3], T[4], T[5]), FVector(T[0], T[1], T[2])) * Right;
		const FVector OnHilt = Held.TransformPosition(FVector(-S.LeftHandBelow, 0.0, 0.0));
		// The wrist goes where it puts the left fist's grip point on the hilt.
		const FVector Wrist = OnHilt + (CwBone(Mesh, TEXT("hand_l")) - Left.GetLocation());
		const FVector Offset = Mesh->GetSocketTransform(TEXT("hand_r"), RTS_World).InverseTransformPosition(Wrist);
		const FVector Shoulder = CwBone(Mesh, TEXT("upperarm_l"));
		const FVector Across = (Shoulder - CwBone(Mesh, TEXT("upperarm_r"))).GetSafeNormal();
		const FVector Up = (CwBone(Mesh, TEXT("neck_01")) - CwBone(Mesh, TEXT("pelvis"))).GetSafeNormal();
		const FVector Elbow = (Shoulder + Wrist) * 0.5 + (Across - Up).GetSafeNormal() * 30.0;
		Anim->SetLeftHandGrip(FMath::SmoothStep(0.0f, 1.0f, P.LeftGrip), Offset, Elbow);
	}

	/** Once a frame after all the animation, before the frame is drawn (FCwAfterAnimationTick). */
	void CwAfterAnimation(FCwState& S, UWorld* World, float DeltaSeconds)
	{
		const float Dt = FMath::Clamp(DeltaSeconds, 0.0f, 0.1f);
		TArray<FCwBlades> Blades;
		Blades.SetNum(S.People.Num());
		for (int32 i = 0; i < S.People.Num(); ++i)
		{
			CwPlaceWeapons(S, S.People[i], Blades[i].H);
		}
		if (S.Demo == ECwDemo::Swing && S.People.IsValidIndex(S.DemoA) && S.People.IsValidIndex(S.DemoB))
		{
			CwSwingAfterAnimation(S, World, Dt, Blades);
		}
		for (int32 i = 0; i < S.People.Num(); ++i)
		{
			FCwPerson& P = S.People[i];
			for (int32 Hand = 0; Hand < 2; ++Hand)
			{
				P.PrevHilt[Hand] = Blades[i].H[Hand].Hilt;
				P.PrevTip[Hand] = Blades[i].H[Hand].Tip;
			}
			P.bPrevValid = P.HeldStyle >= 0;
			P.PrevMontagePos = CwMontagePos(P);
			CwUpdateLeftGrip(S, P, Dt);
		}
	}

	/**
	 * Runs CwAfterAnimation in TG_PostUpdateWork, after the fighters' meshes have ticked (prerequisites on them too), so
	 * the blade test sees this frame's pose and a contact is posed back before the frame is drawn.
	 */
	struct FCwAfterAnimationTick : public FTickFunction
	{
		TWeakObjectPtr<UWorld> World;
		TSet<TWeakObjectPtr<USkeletalMeshComponent>> Waits;

		virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent) override
		{
			FCwState& S = CwState();
			if (World.IsValid() && S.World.Get() == World.Get())
			{
				CwAfterAnimation(S, World.Get(), DeltaTime);
			}
		}
		virtual FString DiagnosticMessage() override { return TEXT("CreatorWeapons after animation"); }
		virtual FName DiagnosticContext(bool bDetailed) override { return FName(TEXT("CreatorWeapons")); }
	};

	FCwAfterAnimationTick& CwAfterTick()
	{
		// Never destroyed: a tick function unregisters itself on destruction, and at exit its level is long gone.
		static FCwAfterAnimationTick* Tick = new FCwAfterAnimationTick();
		return *Tick;
	}

	/** Registers the after-animation tick in this world and makes it wait for every person's meshes. */
	void CwEnsureAfterTick(FCwState& S, UWorld* World)
	{
		FCwAfterAnimationTick& Tick = CwAfterTick();
		if (Tick.World.Get() != World || !Tick.IsTickFunctionRegistered())
		{
			if (Tick.IsTickFunctionRegistered())
			{
				Tick.UnRegisterTickFunction();
			}
			Tick.Waits.Reset();
			Tick.World = World;
			Tick.bCanEverTick = true;
			Tick.bTickEvenWhenPaused = true;
			Tick.bStartWithTickEnabled = true;
			Tick.TickGroup = TG_PostUpdateWork;
			Tick.EndTickGroup = TG_PostUpdateWork;
			if (World->PersistentLevel)
			{
				Tick.RegisterTickFunction(World->PersistentLevel);
				Tick.SetTickFunctionEnable(true);
			}
		}
		for (const FCwPerson& P : S.People)
		{
			ASimPawnActor* Pawn = P.Pawn.Get();
			for (USkeletalMeshComponent* Mesh : {Pawn ? Pawn->GetBody() : nullptr, P.Mesh.Get()})
			{
				if (Mesh && !Tick.Waits.Contains(Mesh))
				{
					Tick.AddPrerequisite(Mesh, Mesh->PrimaryComponentTick);
					Tick.Waits.Add(Mesh);
				}
			}
		}
	}

	void CwUpdatePreview(FCwState& S, FCwPerson& P, float Dt)
	{
		if (P.HeldStyle < 0)
		{
			return;
		}
		const FCwStyleDef& Style = CwStyles[P.HeldStyle];
		const bool bAttack = S.Attack >= 0 && S.Attack < Style.AttackCount;
		if (GCwPreviewDirty)
		{
			GCwPreviewDirty = false;
			if (!bAttack)
			{
				CwPlayStance(S, P);
			}
			else if (Style.Attacks[S.Attack].bBlock)
			{
				CwPlay(S, P, Style.Attacks[S.Attack].Clip, 1.0f, true, 0.2f, 0.3f);
				P.Role = ECwRole::Block;
			}
			else if (CwPlay(S, P, Style.Attacks[S.Attack].Clip, S.Speed, false, 0.15f, 0.3f))
			{
				P.Role = ECwRole::Attack;
			}
			return;
		}
		if (!bAttack || Style.Attacks[S.Attack].bBlock)
		{
			return;
		}
		// The swing again and again, with a short pause in the stance between.
		if (P.Role == ECwRole::Attack && CwMontageFraction(P) >= 0.93f)
		{
			CwPlayStance(S, P);
			S.PreviewPause = 0.7f;
		}
		else if (P.Role == ECwRole::Stance)
		{
			S.PreviewPause -= Dt;
			if (S.PreviewPause <= 0.0f && CwPlay(S, P, Style.Attacks[S.Attack].Clip, S.Speed, false, 0.15f, 0.3f))
			{
				P.Role = ECwRole::Attack;
			}
		}
	}
	/** -CwScript: a fixed sequence for pictures. */
	void CwRunScript(FCwState& S, float Dt)
	{
		if (S.ScriptT < 0.0f)
		{
			if (!S.People.IsValidIndex(S.Selected) || !S.People[S.Selected].Pawn.IsValid() || !S.People[S.Selected].Pawn->GetMakeHumanMesh())
			{
				return;
			}
			S.ScriptT = 0.0f;
			S.ScriptStep = 0;
			S.bEquip = false;
		}
		S.ScriptT += Dt;
		struct FStep
		{
			float At;
			int32 Action;  // 0 style+attack, 1 shot, 2 start the demo, 3 finish, 4 stop the demo, 5 the next body blow staggers
			int32 Style;
			int32 Attack;
			const TCHAR* Shot;
		};
		static const FStep Steps[] = {
			{9.0f, 0, 1, 0, nullptr}, {9.7f, 1, 0, 0, TEXT("c_great1")}, {10.1f, 1, 0, 0, TEXT("c_great2")},
			{11.5f, 0, 3, 0, nullptr}, {12.2f, 1, 0, 0, TEXT("d_dual1")}, {12.8f, 1, 0, 0, TEXT("d_dual2")},
			{14.0f, 0, 1, -1, nullptr}, {14.5f, 2, 0, 0, nullptr}, {45.0f, 3, 0, 0, nullptr}, {56.0f, 1, 0, 0, TEXT("f_after1")},
			// Sword and shield: the shield, the push-apart in lunges, and one heavy stagger (StumbleBackwards).
			{58.0f, 4, 0, 0, nullptr}, {58.5f, 0, 2, -1, nullptr}, {59.5f, 2, 0, 0, nullptr}, {63.0f, 1, 0, 0, TEXT("g_shield1")},
			{68.0f, 1, 0, 0, TEXT("g_shield2")}, {70.0f, 5, 0, 0, nullptr},
		};
		// The forced stagger: the blow, the stumble twice, then getting up twice.
		if (S.StaggerT >= 0.0f)
		{
			S.StaggerT += Dt;
			static const float ShotAt[5] = {0.0f, 0.4f, 0.9f, 2.4f, 3.6f};
			if (S.StaggerShots < 5 && S.StaggerT >= ShotAt[S.StaggerShots])
			{
				S.PendingShot = FString::Printf(TEXT("h_stagger%d"), ++S.StaggerShots);
			}
		}
		// -CwRecord=<seconds>: from 6 s into the demo, every frame at a fixed 30 a second (Saved/Weapons/rec_0000.png ...).
		// -CwRecordAt=<from>:<seconds>,<from>:<seconds>...: instead, those spans of the script's clock (numbered on).
		static TArray<FVector2f> Spans = []
		{
			TArray<FVector2f> Out;
			FString List;
			float Seconds = 0.0f;
			if (FParse::Value(FCommandLine::Get(), TEXT("CwRecordAt="), List, false))
			{
				TArray<FString> Parts;
				List.ParseIntoArray(Parts, TEXT(","));
				for (const FString& Part : Parts)
				{
					FString From, Length;
					if (Part.Split(TEXT(":"), &From, &Length))
					{
						Out.Emplace(FCString::Atof(*From), FCString::Atof(*Length));
					}
				}
			}
			else if (FParse::Value(FCommandLine::Get(), TEXT("CwRecord="), Seconds))
			{
				Out.Emplace(20.5f, Seconds);
			}
			return Out;
		}();
		bool bRecordNow = false;
		for (const FVector2f& Span : Spans)
		{
			bRecordNow |= S.ScriptT >= Span.X && S.ScriptT < Span.X + Span.Y;
		}
		if (bRecordNow && S.Demo != ECwDemo::Off)
		{
			if (S.Recorded == 0)
			{
				FApp::SetUseFixedTimeStep(true);
				FApp::SetFixedDeltaTime(1.0 / 30.0);
			}
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Weapons") / FString::Printf(TEXT("rec_%04d.png"), S.Recorded++), false, false);
		}
		while (S.ScriptStep < static_cast<int32>(UE_ARRAY_COUNT(Steps)) && S.ScriptT >= Steps[S.ScriptStep].At)
		{
			const FStep& Step = Steps[S.ScriptStep++];
			switch (Step.Action)
			{
				case 0:
					S.bEquip = true;
					S.Style = Step.Style;
					S.Attack = Step.Attack;
					GCwPreviewDirty = true;
					break;
				case 1:
					S.PendingShot = Step.Shot;
					break;
				case 2:
					CwStartDemo(S);
					break;
				case 3:
					S.bFinish = true;
					break;
				case 4:
					CwStopDemo(S);
					break;
				case 5:
					S.bForceStagger = true;
					break;
				default:
					break;
			}
		}
	}
}

// -------------------------------------------------------------------------------------------------------------------

void CreatorWeapons::Tick(UWorld* World, const TArray<ASimPawnActor*>& People, int32 Selected, float DeltaSeconds, bool& bInOutWalk)
{
	FCwState& S = CwState();
	if (S.World.Get() != World)
	{
		// A new world (or the first): everything from the last one is gone with it.
		S = FCwState();
		S.World = World;
		GCwPreviewDirty = true;
	}
	if (!World)
	{
		return;
	}
	const float Dt = FMath::Clamp(DeltaSeconds, 0.0f, 0.1f);
	if (S.People.Num() != People.Num())
	{
		CwStopDemo(S);
		S.People.SetNum(People.Num());
	}
	for (int32 i = 0; i < People.Num(); ++i)
	{
		if (S.People[i].Pawn.Get() != People[i])
		{
			CwDropWeapons(S.People[i]);
			S.People[i] = FCwPerson();
			S.People[i].Pawn = People[i];
		}
	}
	if (Selected != S.Selected)
	{
		S.Selected = Selected;
		GCwPreviewDirty = true;
	}
	if (S.Demo != ECwDemo::Off && (!S.People.IsValidIndex(S.DemoA) || !S.People.IsValidIndex(S.DemoB) || !S.People[S.DemoA].Pawn.IsValid() || !S.People[S.DemoB].Pawn.IsValid()))
	{
		CwStopDemo(S);
	}
	static const bool bCwScript = FParse::Param(FCommandLine::Get(), TEXT("CwScript"));
	if (bCwScript)
	{
		CwRunScript(S, Dt);
	}
	if (S.bEquip || S.Demo != ECwDemo::Off)
	{
		CwEnsureEffects(S, World);
	}
	// Who holds what: the picked person (or both fighters) the style, everyone else nothing.
	for (int32 i = 0; i < S.People.Num(); ++i)
	{
		FCwPerson& P = S.People[i];
		const int32 Want = (S.bEquip || S.Demo != ECwDemo::Off) && CwIsParticipant(S, i) ? S.Style : -1;
		const int32 Before = P.HeldStyle;
		CwHold(S, P, Want);
		if (Want < 0 && Before >= 0)
		{
			CwStopMontage(P, 0.3f);
			P.Role = ECwRole::Idle;
		}
		else if (Want >= 0 && Before != Want)
		{
			GCwPreviewDirty = true;
		}
	}
	if (S.Demo != ECwDemo::Off)
	{
		bInOutWalk = false;
		CwPlaceFighters(S, Dt);
	}
	// The preview or the demo. The weapons go to the hands, and the blade test runs, after the animation
	// (CwAfterAnimation, in TG_PostUpdateWork).
	CwEnsureAfterTick(S, World);
	if (S.Demo != ECwDemo::Off)
	{
		CwUpdateDemo(S, World, Dt);
	}
	else if (S.bEquip && S.People.IsValidIndex(S.Selected))
	{
		CwUpdatePreview(S, S.People[S.Selected], Dt);
	}
	CwUpdateBlood(S, World, Dt);
	if (!S.PendingShot.IsEmpty())
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Weapons") / S.PendingShot + TEXT(".png"), true, false);
		S.PendingShot.Reset();
	}
}

void CreatorWeapons::DrawWindow(float Scale)
{
	FCwState& S = CwState();
	if (!S.World.IsValid())
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	// Bottom middle, clear of the creator panel (left) and the animations panel (right).
	// Placed again whenever the view changes size (the creator sets 1920x1080 after the first frames, which left a
	// once-placed window off the picture).
	static ImVec2 CwLastViewport(0.0f, 0.0f);
	const bool bResized = CwLastViewport.x != Viewport->Size.x || CwLastViewport.y != Viewport->Size.y;
	CwLastViewport = Viewport->Size;
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y - 16.0f * Scale), bResized ? ImGuiCond_Always : ImGuiCond_Once, ImVec2(0.5f, 1.0f));
	ImGui::SetNextWindowBgAlpha(0.88f);
	if (ImGui::Begin("WEAPONS", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
	{
		const bool bDemo = S.Demo != ECwDemo::Off;
		if (ImGui::Checkbox("Hold a weapon", &S.bEquip))
		{
			GCwPreviewDirty = true;
		}
		ImGui::SeparatorText("Fighting style");
		for (int32 i = 0; i < CwStyleCount; ++i)
		{
			if (ImGui::RadioButton(CwStyles[i].Label, S.Style == i))
			{
				if (S.Style != i)
				{
					S.Style = i;
					S.Attack = -1;
					GCwPreviewDirty = true;
					if (bDemo)
					{
						// Both change weapons: start the exchange again.
						CwStopDemo(S);
						CwStartDemo(S);
					}
				}
			}
		}
		const FCwStyleDef& Style = CwStyles[S.Style];
		if (Style.Note)
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 300.0f * Scale);
			ImGui::TextDisabled("%s", Style.Note);
			ImGui::PopTextWrapPos();
		}
		ImGui::SeparatorText("Attack (plays at once, again and again)");
		ImGui::BeginChild("##attacks", ImVec2(300.0f * Scale, 200.0f * Scale), ImGuiChildFlags_Borders);
		if (ImGui::Selectable("Stance (idle)", S.Attack < 0))
		{
			S.Attack = -1;
			S.bEquip = true;
			GCwPreviewDirty = true;
		}
		for (int32 a = 0; a < Style.AttackCount; ++a)
		{
			if (ImGui::Selectable(Style.Attacks[a].Label, S.Attack == a))
			{
				S.Attack = a;
				S.bEquip = true;
				GCwPreviewDirty = true;
			}
		}
		ImGui::EndChild();
		ImGui::SliderFloat("Speed##cw", &S.Speed, 0.25f, 1.5f, "%.2f");

		ImGui::SeparatorText("Fight demo");
		if (!bDemo)
		{
			if (ImGui::Button("Start the fight"))
			{
				CwStartDemo(S);
			}
			ImGui::SameLine();
			ImGui::TextDisabled("the picked person and the next one");
		}
		else
		{
			if (ImGui::Button("Stop"))
			{
				CwStopDemo(S);
			}
			ImGui::SameLine();
			const bool bCanFinish = !S.bFinish && S.People.IsValidIndex(S.DemoA) && S.People.IsValidIndex(S.DemoB) &&
				(S.People[S.DemoA].Hits >= 3 || S.People[S.DemoB].Hits >= 3);
			ImGui::BeginDisabled(!bCanFinish);
			if (ImGui::Button("Finish"))
			{
				S.bFinish = true;
			}
			ImGui::EndDisabled();
			if (!bCanFinish && !S.bFinish)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(after 3 hits)");
			}
		}
		ImGui::SliderFloat("Apart (cm)", &S.Distance, 110.0f, 260.0f, "%.0f");
		ImGui::Checkbox("Only the picked attack", &S.bPickedOnly);
		if (bDemo && S.People.IsValidIndex(S.DemoA) && S.People.IsValidIndex(S.DemoB))
		{
			ImGui::Text("Hits taken: %d / %d   swings %d, blocked %d, missed %d", S.People[S.DemoA].Hits, S.People[S.DemoB].Hits, S.Swings, S.Blocks, S.Misses);
		}
		if (!S.LastEvent.IsEmpty())
		{
			ImGui::TextDisabled("%s", TCHAR_TO_UTF8(*S.LastEvent));
		}

		ImGui::SeparatorText("Blood");
		ImGui::Checkbox("Blood", &S.bBlood);
		ImGui::SameLine();
		if (ImGui::Button("Clean up"))
		{
			CwCleanUp(S);
		}
		int32 Stains = 0;
		for (const FCwPerson& P : S.People)
		{
			Stains += P.Stains.Num();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%d on the floor, %d on bodies", S.Spots.Num(), Stains);
		if (S.bSplatIsWound)
		{
			ImGui::TextDisabled("Plain blood spots: run the FightMaterials commandlet for splats.");
		}

		if (ImGui::CollapsingHeader("Grip (fine tuning)"))
		{
			if (S.Style == static_cast<int32>(ECwStyle::GreatSword))
			{
				ImGui::SliderFloat("Left hand below the right (cm)", &S.LeftHandBelow, 4.0f, 25.0f, "%.1f");
			}
			for (int32 Hand = 0; Hand < 2; ++Hand)
			{
				const char* Def = Hand == 0 ? Style.RightDef : Style.LeftDef;
				if (!Def)
				{
					continue;
				}
				ImGui::PushID(Hand);
				float* T = S.Grip[S.Style][Hand];
				ImGui::Text("%s hand (%s)", Hand == 0 ? "Right" : "Left", Def);
				ImGui::SliderFloat3("Move (cm)", T, -15.0f, 15.0f, "%.1f");
				ImGui::SliderFloat3("Turn (deg)", T + 3, -180.0f, 180.0f, "%.0f");
				if (ImGui::Button("Reset"))
				{
					FMemory::Memzero(T, sizeof(float) * 6);
				}
				ImGui::PopID();
			}
		}
	}
	ImGui::End();
}
