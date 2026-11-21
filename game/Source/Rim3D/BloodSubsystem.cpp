#include "BloodSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DecalComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"
#include "SkinBlood.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Filth.h"
#include "ColonySim/Map.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Wounds.h"

#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogBlood, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GBloodEnabled(TEXT("Blood.Enabled"), 1,
		TEXT("1 draws the full blood (spray, drops, stains, bleeding wounds, filth); 0 leaves the fights' old simple blood."), ECVF_Default);
	TAutoConsoleVariable<int32> GBloodQuality(TEXT("Blood.Quality"), 1, TEXT("0 low (fewer drops), 1 normal, 2 high."), ECVF_Default);
	TAutoConsoleVariable<int32> GBloodMaxDrops(TEXT("Blood.MaxDrops"), 3000, TEXT("Most drops in the air at once; hits make fewer as it fills."), ECVF_Default);
	TAutoConsoleVariable<int32> GBloodMaxStains(TEXT("Blood.MaxStains"), 900, TEXT("Most stains on the world at once (oldest go first); bodies keep 32 each, 400 in all."), ECVF_Default);
	TAutoConsoleVariable<float> GBloodFadeStep(TEXT("Blood.FadeStep"), 0.03f,
		TEXT("Share of a blood stain's strength lost every 10 seconds (game speed) from when it was put down (sneed: 3%, gone in about 5.5 minutes). 0: stains stay."), ECVF_Default);
	TAutoConsoleVariable<float> GBloodDryMinutes(TEXT("Blood.DryMinutes"), 5.0f, TEXT("Real minutes at 1x for fresh blood to dry dark."), ECVF_Default);
	TAutoConsoleVariable<int32> GBloodFilth(TEXT("Blood.Filth"), 1, TEXT("1 draws the sim's filth (blood, dirt, vomit, ash) as stains on its cells."), ECVF_Default);
	TAutoConsoleVariable<float> GBloodDropScale(TEXT("Blood.DropScale"), 1.6f, TEXT("Drawn size of flying drops against their real size (they read better a little big)."), ECVF_Default);
	TAutoConsoleVariable<float> GBloodSplatScale(TEXT("Blood.SplatScale"), 1.9f,
		TEXT("Drawn size of blood splats on the ground and walls (the round shapes fill less of their square than the old spiky ones, and read small from 10 m)."), ECVF_Default);
	TAutoConsoleVariable<float> GBloodGroundShade(TEXT("Blood.GroundShade"), 0.6f,
		TEXT("How bright blood on the ground and walls is against blood on bodies (green and blue go down further, so it stays deep red, not pink)."), ECVF_Default);
	TAutoConsoleVariable<int32> GBloodFlipU(TEXT("Blood.FlipU"), 0, TEXT("1 mirrors the stain pictures left-right (if spatter tails point the wrong way)."), ECVF_Default);
	TAutoConsoleVariable<int32> GBloodFlipV(TEXT("Blood.FlipV"), 0, TEXT("1 mirrors the stain pictures top-bottom (if runs go up)."), ECVF_Default);
	TAutoConsoleVariable<int32> GBloodSwapUV(TEXT("Blood.SwapUV"), 0, TEXT("1 turns the stain pictures a quarter (if runs go sideways)."), ECVF_Default);
	TAutoConsoleVariable<int32> GBloodBleeding(TEXT("Blood.Bleeding"), 0,
		TEXT("1: wounds keep bleeding while the sim says so (drips, runs, heartbeat spurts, pools). 0 (sneed, 2026-11-16, for now): blood only on the hit."), ECVF_Default);
	TAutoConsoleVariable<int32> GBloodBits(TEXT("Blood.Bits"), 0, TEXT("1 throws small bits of tissue on heavy hits (off: sneed read them as low-poly chunks)."), ECVF_Default);
	TAutoConsoleVariable<int32> GBloodDebug(TEXT("Blood.Debug"), 0, TEXT("1 logs every hit and prints counts on screen."), ECVF_Default);

	const TCHAR* const BloodSplatterPath = TEXT("/Game/Rim3D/Generated/Blood/M_BloodSplatter.M_BloodSplatter");
	const TCHAR* const BloodDropPath = TEXT("/Game/Rim3D/Generated/Blood/M_BloodDrop.M_BloodDrop");
	const TCHAR* const BloodMistPath = TEXT("/Game/Rim3D/Generated/Blood/M_BloodMist.M_BloodMist");
	const TCHAR* const BloodSphereMeshPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
	const TCHAR* const BloodPlaneMeshPath = TEXT("/Engine/BasicShapes/Plane.Plane");

	// The splat atlas (BloodArtCommandlet): 4 x 4 pictures.
	constexpr int32 BloodCellRound = 0;        // 0-3: a drop that hit square on: a round blot, bulges, droplets
	constexpr int32 BloodCellDirectional = 4;  // 4-7: a drop that hit at a slant, travelling to +U: body and tail
	constexpr int32 BloodCellSpecks = 8;       // 8-9: fine spatter
	constexpr int32 BloodCellPool = 10;        // 10-11: a pool
	constexpr int32 BloodCellRun = 12;         // a run, top to bottom (+V down)
	constexpr int32 BloodCellSmear = 13;       // a smear along U
	constexpr int32 BloodCellHole = 14;        // a bullet hole or stab
	/**
	 * The colour of blood on a body against the same blood on the ground: brighter, and with more of the two weaker
	 * channels, because skin and cloth are mostly seen in open shade, where a pure red is black (PC look, 2026-11-18).
	 */
	FLinearColor BloodOnBody(const FLinearColor& Color)
	{
		const float Top = 1.7f * FMath::Max3(Color.R, Color.G, Color.B);
		return FLinearColor(FMath::Max(Color.R * 1.7f, 0.3f * Top), FMath::Max(Color.G * 1.7f, 0.3f * Top), FMath::Max(Color.B * 1.7f, 0.3f * Top), Color.A);
	}
	constexpr int32 BloodCellGash = 15;        // a torn exit wound or a cut along U
	constexpr int32 BloodWorldCells = 16;      // CellMaterials: the same 16 again, tinted for the ground and walls

	/** How big (decal half size, cm) a pool under a bleeding body grows. The pool picture fills only about the middle 60%
	 *  of its square, so 90 is a puddle about a metre across: a body lying on it still leaves it showing round the
	 *  edges (PC look, 2026-11-20: at 26-52 it hid under the body and nothing read from 12 m). */
	float BloodPoolMaxRadius(float BloodLoss)
	{
		return 35.0f + 55.0f * FMath::Clamp(BloodLoss + 0.3f, 0.0f, 1.0f);
	}

	constexpr int32 BloodTracesPerFrame = 320;
	constexpr int32 BloodMaxBodyStainsEach = 32;
	constexpr int32 BloodMaxBodyStains = 400;
	constexpr int32 BloodMaxFilthDecals = 1500;
	constexpr float BloodGravity = 980.0f;
	// Rain washes a fresh stain off in this many game-speed seconds at full rain (ours: thin spatter goes in minutes).
	constexpr float BloodRainWashSeconds = 240.0f;
	/** How much of a stain is left `Age` game-speed seconds after it was put down: 3% less every 10 s (Blood.FadeStep). */
	float BloodAgeFade(float Age)
	{
		const float Step = FMath::Max(0.0f, GBloodFadeStep.GetValueOnGameThread());
		return FMath::Clamp(1.0f - Step * FMath::FloorToFloat(FMath::Max(0.0f, Age) / 10.0f), 0.0f, 1.0f);
	}
	// A stain on skin in the rain.
	constexpr float BloodBodyRainWashSeconds = 300.0f;
	// A stain on skin under water while swimming (the sim's GrimeRules swimWashSeconds is 600 game seconds: 20 s at 1x).
	constexpr float BloodBodySwimWashSeconds = 20.0f;
	constexpr float BloodGoingSeconds = 1.5f;

	FVector BloodAnyPerpendicular(const FVector& V, FRandomStream& Random)
	{
		FVector P = FVector::CrossProduct(V, FMath::Abs(V.Z) < 0.9 ? FVector::UpVector : FVector::RightVector).GetSafeNormal();
		const float Turn = Random.FRandRange(0.0f, 2.0f * PI);
		return P.RotateAngleAxisRad(Turn, V.GetSafeNormal());
	}

	FVector BloodSimToWorldUnits(const USimSubsystem& Bridge, float X, float Y, float Z)
	{
		return Bridge.GetWorldOrigin() + FVector(X * 100.0, Y * 100.0, Z * 100.0);
	}

	float BloodQualityScale()
	{
		switch (GBloodQuality.GetValueOnGameThread())
		{
			case 0: return 0.4f;
			case 2: return 1.8f;
			default: return 1.0f;
		}
	}

	bool BloodIsWoundBleeding(const colony::Snapshot& Snapshot, const colony::ThingView& View, int32 Part, float& OutShare)
	{
		float Total = 0.0f;
		float Mine = 0.0f;
		bool bAny = false;
		bool bMine = false;
		for (uint32 I = 0; I < View.woundCount; ++I)
		{
			const uint32 Index = View.firstWound + I;
			if (Index >= Snapshot.wounds.size())
			{
				break;
			}
			const colony::WoundView& Wound = Snapshot.wounds[Index];
			if (!Wound.bleeding)
			{
				continue;
			}
			bAny = true;
			Total += FMath::Max(0.05f, Wound.severity);
			if (Part < 0 || static_cast<int32>(Wound.part) == Part)
			{
				bMine = true;
				Mine += FMath::Max(0.05f, Wound.severity);
			}
		}
		if (!bMine && bAny && Part >= 0)
		{
			// The hit part's wound may be on an organ or bone under it (left out of the view): count its share as the rest's.
			Mine = Total / FMath::Max(1.0f, static_cast<float>(View.woundCount));
			bMine = View.bleedPerDay > 0.0f;
		}
		OutShare = Total > 0.0f ? Mine / Total : 0.0f;
		return bMine;
	}
}

bool UBloodSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UBloodSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBloodSubsystem, STATGROUP_Tickables);
}

void UBloodSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		BridgeSource = Bridge;
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &UBloodSubsystem::OnSimEvent);
	}
	Collection.InitializeDependency<USimPawnSubsystem>();
	SkinBlood = NewObject<USkinBlood>(this);

	// -BloodSplatScale=2 -BloodGroundShade=0.5 on the command line (PC look, 2026-11-20: Blood.SplatScale set through
	// -ini was only "deferred" and the splats stayed the same size). The console (Blood.SplatScale 2) works too.
	float FromCommandLine = 0.0f;
	if (FParse::Value(FCommandLine::Get(), TEXT("BloodSplatScale="), FromCommandLine) && FromCommandLine > 0.0f)
	{
		GBloodSplatScale.AsVariable()->Set(FromCommandLine, ECVF_SetByCommandline);
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("BloodGroundShade="), FromCommandLine) && FromCommandLine > 0.0f)
	{
		GBloodGroundShade.AsVariable()->Set(FromCommandLine, ECVF_SetByCommandline);
	}
	UE_LOG(LogBlood, Display, TEXT("Blood.SplatScale %.2f, Blood.GroundShade %.2f"), GBloodSplatScale.GetValueOnGameThread(), GBloodGroundShade.GetValueOnGameThread());
}

int32 UBloodSubsystem::GetSkinPaints() const
{
	return SkinBlood ? SkinBlood->GetPaints() : 0;
}

USkinBlood* UBloodSubsystem::GetSkinBlood() const
{
	return SkinBlood.Get();
}

int32 UBloodSubsystem::GetSkinMasks() const
{
	return SkinBlood ? SkinBlood->GetMasks() : 0;
}

void UBloodSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = BridgeSource.Get())
	{
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------
// Fluids, kinds and force.

bool UBloodSubsystem::FluidForFilthDef(const colony::Sim* Sim, colony::DefId FilthDef, FBloodFluid& Out, bool* bOutFluid)
{
	if (!Sim || !FilthDef.valid())
	{
		return false;
	}
	const colony::Def& Def = Sim->defs().get(FilthDef);
	if (Def.count("look.wet") < 3)
	{
		return false;
	}
	Out.Wet = FLinearColor(Def.numberAt("look.wet", 0), Def.numberAt("look.wet", 1), Def.numberAt("look.wet", 2));
	Out.Dry = Def.count("look.dry") >= 3 ? FLinearColor(Def.numberAt("look.dry", 0), Def.numberAt("look.dry", 1), Def.numberAt("look.dry", 2)) : Out.Wet;
	Out.Filth = FilthDef;
	if (bOutFluid)
	{
		*bOutFluid = Def.flag("look.fluid", false);
	}
	return true;
}

FBloodFluid UBloodSubsystem::FluidForPawnDef(const colony::Sim* Sim, colony::DefId PawnDef)
{
	FBloodFluid Fluid;
	if (!Sim)
	{
		return Fluid;
	}
	const int32 Kind = Sim->filth().bloodKindOf(PawnDef);
	if (Kind >= 0 && Kind < static_cast<int32>(Sim->filth().kinds().size()))
	{
		FluidForFilthDef(Sim, Sim->filth().kinds()[Kind].def, Fluid);
	}
	return Fluid;
}

float UBloodSubsystem::ForceFor(EBloodWound Kind, float Damage)
{
	float Reference = 12.0f;
	switch (Kind)
	{
		case EBloodWound::Gunshot: Reference = 12.0f; break;
		case EBloodWound::Cut: Reference = 11.0f; break;
		case EBloodWound::Stab: Reference = 11.0f; break;
		case EBloodWound::Blunt: Reference = 13.0f; break;
		case EBloodWound::Bite: Reference = 9.0f; break;
		case EBloodWound::Blast: Reference = 15.0f; break;
		default: break;
	}
	return FMath::Clamp(FMath::Sqrt(FMath::Max(0.0f, Damage) / Reference) * 1.0f, 0.2f, 2.5f);
}

EBloodWound UBloodSubsystem::KindForInjury(const FString& HediffName)
{
	if (HediffName == TEXT("Gunshot")) return EBloodWound::Gunshot;
	if (HediffName == TEXT("Cut") || HediffName == TEXT("Scratch")) return EBloodWound::Cut;
	if (HediffName == TEXT("Stab")) return EBloodWound::Stab;
	if (HediffName == TEXT("Bite") || HediffName == TEXT("Shredded")) return EBloodWound::Bite;
	if (HediffName == TEXT("Burn") || HediffName == TEXT("Frostbite")) return EBloodWound::Burn;
	if (HediffName == TEXT("Crush") || HediffName == TEXT("Bruise") || HediffName == TEXT("Crack")) return EBloodWound::Blunt;
	return EBloodWound::Auto;
}

FLinearColor UBloodSubsystem::ColorAt(const FBloodFluid& Fluid, float Age) const
{
	const float DrySeconds = FMath::Max(10.0f, GBloodDryMinutes.GetValueOnGameThread() * 60.0f);
	// Drying is quick at first (the surface skins over), then slow.
	const float T = FMath::Clamp(1.0f - FMath::Exp(-2.6f * Age / DrySeconds), 0.0f, 1.0f) / (1.0f - FMath::Exp(-2.6f));
	FLinearColor Color = FMath::Lerp(Fluid.Wet, Fluid.Dry, FMath::Clamp(T, 0.0f, 1.0f));
	// A pure red goes black in the blue light of open shade (PC look, 2026-11-18: wounds on a shaded face were black
	// smudges): keep a little of the other two channels.
	const float Floor = 0.12f * FMath::Max3(Color.R, Color.G, Color.B);  // (not GetMax: that counts alpha, and made it pink)
	Color.R = FMath::Max(Color.R, Floor);
	Color.G = FMath::Max(Color.G, Floor);
	Color.B = FMath::Max(Color.B, Floor);
	return Color;
}

FLinearColor UBloodSubsystem::DecalColorFor(const FBloodFluid& Fluid, float Age, float Opacity) const
{
	const float DrySeconds = FMath::Max(10.0f, GBloodDryMinutes.GetValueOnGameThread() * 60.0f);
	const float Wet = FMath::Clamp(1.0f - Age / DrySeconds, 0.0f, 1.0f);
	FLinearColor Color = ColorAt(Fluid, Age);
	// Alpha: floor(alpha) / 62 is the wetness (31 steps, even numbers), frac(alpha) the opacity (M_BloodSplatter).
	Color.A = FMath::RoundToFloat(Wet * 31.0f) * 2.0f + FMath::Clamp(Opacity, 0.0f, 0.995f);
	return Color;
}

// ---------------------------------------------------------------------------
// Assets.

bool UBloodSubsystem::IsReady()
{
	return GBloodEnabled.GetValueOnGameThread() != 0 && EnsureAssets();
}

UInstancedStaticMeshComponent* UBloodSubsystem::MakeInstanced(UStaticMesh* Mesh, UMaterialInterface* Material, int32 CustomFloats)
{
	UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(Holder);
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, Material);
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetGenerateOverlapEvents(false);
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(false);
	Component->bReceivesDecals = false;
	Component->bUseAsOccluder = false;
	Component->SetNumCustomDataFloats(CustomFloats);
	Component->SetupAttachment(Holder->GetRootComponent());
	Component->RegisterComponent();
	return Component;
}

bool UBloodSubsystem::EnsureAssets()
{
	if (Holder && SplatterBase && DropInstances && MistInstances)
	{
		return true;
	}
	if (bAssetsTried)
	{
		return false;
	}
	bAssetsTried = true;
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	SplatterBase = LoadObject<UMaterialInterface>(nullptr, BloodSplatterPath);
	UMaterialInterface* DropMaterial = LoadObject<UMaterialInterface>(nullptr, BloodDropPath);
	UMaterialInterface* MistMaterial = LoadObject<UMaterialInterface>(nullptr, BloodMistPath);
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, BloodSphereMeshPath);
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, BloodPlaneMeshPath);
	if (!SplatterBase || !DropMaterial || !MistMaterial || !Sphere || !Plane)
	{
		bAssetsMissing = true;
		UE_LOG(LogBlood, Warning, TEXT("Blood assets are missing (UnrealEditor-Cmd.exe Rim3D.uproject -run=BloodArt): the fights draw their old simple blood"));
		return false;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("Root"));
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();
	DropInstances = MakeInstanced(Sphere, DropMaterial, 4);
	ChunkInstances = MakeInstanced(Sphere, DropMaterial, 4);
	MistInstances = MakeInstanced(Plane, MistMaterial, 4);
	CellMaterials.Reset();
	// 0-15 on bodies, 16-31 (BloodWorldCells on) on the ground and walls, which take WorldTint.
	for (int32 Cell = 0; Cell < 32; ++Cell)
	{
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(SplatterBase, this);
		Mid->SetScalarParameterValue(TEXT("CellX"), static_cast<float>(Cell % 4));
		Mid->SetScalarParameterValue(TEXT("CellY"), static_cast<float>((Cell % 16) / 4));
		CellMaterials.Add(Mid);
	}
	ApplyCvarsToMaterials();
	return true;
}

void UBloodSubsystem::ApplyCvarsToMaterials()
{
	const float Shade = FMath::Clamp(GBloodGroundShade.GetValueOnGameThread(), 0.1f, 2.0f);
	for (int32 Index = 0; Index < CellMaterials.Num(); ++Index)
	{
		if (UMaterialInstanceDynamic* Mid = CellMaterials[Index])
		{
			Mid->SetVectorParameterValue(TEXT("WorldTint"), Index >= BloodWorldCells ? FLinearColor(Shade, Shade * 0.55f, Shade * 0.55f) : FLinearColor::White);
			Mid->SetScalarParameterValue(TEXT("FlipU"), GBloodFlipU.GetValueOnGameThread() != 0 ? 1.0f : 0.0f);
			Mid->SetScalarParameterValue(TEXT("FlipV"), GBloodFlipV.GetValueOnGameThread() != 0 ? 1.0f : 0.0f);
			Mid->SetScalarParameterValue(TEXT("SwapUV"), GBloodSwapUV.GetValueOnGameThread() != 0 ? 1.0f : 0.0f);
		}
	}
}

void UBloodSubsystem::ResetAll()
{
	Drops.Reset();
	Mist.Reset();
	for (FStain& Stain : Stains)
	{
		if (Stain.Decal.IsValid())
		{
			Stain.Decal->DestroyComponent();
		}
	}
	Stains.Reset();
	for (FBodyStain& Stain : BodyStains)
	{
		if (Stain.Decal.IsValid())
		{
			Stain.Decal->DestroyComponent();
		}
	}
	BodyStains.Reset();
	for (FBleed& Bleed : Bleeds)
	{
		if (Bleed.Run.IsValid())
		{
			Bleed.Run->DestroyComponent();
		}
	}
	Bleeds.Reset();
	Pools.Reset();
	Pending.Reset();
	Injuries.Reset();
	if (SkinBlood)
	{
		SkinBlood->Reset();
	}
	SimHitSteps.Reset();
	for (TPair<uint64, FFilthStain>& Pair : FilthDecals)
	{
		if (Pair.Value.Decal.IsValid())
		{
			Pair.Value.Decal->DestroyComponent();
		}
	}
	FilthDecals.Reset();
	LastFilthChanges = ~0ull;
	bFilthSeenOnce = false;
	for (UInstancedStaticMeshComponent* Component : {DropInstances.Get(), ChunkInstances.Get(), MistInstances.Get()})
	{
		if (Component)
		{
			Component->ClearInstances();
		}
	}
}

void UBloodSubsystem::CleanAll()
{
	ResetAll();
}

// ---------------------------------------------------------------------------
// Sim events: the injury kind of each hit, blasts, and hits that came with no bullet or swing (traps, dev damage).

void UBloodSubsystem::OnSimEvent(const colony::Event& Event)
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	const int64 Step = Bridge->GetSteps();
	if (Event.type == colony::EventType::PawnInjured && Event.def.valid())
	{
		const colony::Def& Def = Bridge->GetSim()->defs().get(Event.def);
		FInjury& Injury = Injuries.FindOrAdd(entt::to_integral(Event.thing));
		if (Injury.Step != Step || Event.amount > Injury.Amount)
		{
			Injury.Hediff = UTF8_TO_TCHAR(Def.defName().c_str());
			Injury.Amount = Event.amount;
			Injury.Step = Step;
			Injury.Part = Event.part;
		}
	}
	else if ((Event.type == colony::EventType::ProjectileHit || Event.type == colony::EventType::MeleeHit) && Event.hit == colony::ProjectileHitKind::Pawn)
	{
		SimHitSteps.Add(entt::to_integral(Event.target), Step);
	}
	else if (Event.type == colony::EventType::Explosion)
	{
		LastBlastCentre = BloodSimToWorldUnits(*Bridge, Event.x, Event.y, Event.z);
		LastBlastStep = Step;
	}
}

AActor* UBloodSubsystem::ActorOf(colony::Entity Pawn) const
{
	const USimPawnSubsystem* Pawns = GetWorld() ? GetWorld()->GetSubsystem<USimPawnSubsystem>() : nullptr;
	return Pawns && Pawn != colony::kNoEntity ? Pawns->FindPawn(Pawn) : nullptr;
}

// ---------------------------------------------------------------------------
// Hits.

void UBloodSubsystem::AddHit(const FBloodHit& Hit)
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	FPendingHit Entry;
	Entry.Hit = Hit;
	Entry.Step = Bridge ? Bridge->GetSteps() : 0;
	Pending.Add(Entry);
}

float UBloodSubsystem::GroundBelow(const FVector& At, AActor* Ignore) const
{
	FHitResult Result;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(BloodGround), false);
	if (Ignore)
	{
		Params.AddIgnoredActor(Ignore);
	}
	if (GetWorld()->LineTraceSingleByChannel(Result, At + FVector(0, 0, 30), At - FVector(0, 0, 400), ECC_Visibility, Params))
	{
		return static_cast<float>(Result.ImpactPoint.Z);
	}
	return static_cast<float>(At.Z - 150.0);
}

void UBloodSubsystem::DrawHit(const FBloodHit& Hit)
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	const colony::Sim* Sim = Bridge ? Bridge->GetSim() : nullptr;
	const colony::ThingView* View = Sim && Hit.Victim != colony::kNoEntity ? Bridge->GetCurrentSnapshot().find(Hit.Victim) : nullptr;

	EBloodWound Kind = Hit.Kind;
	if (Kind == EBloodWound::Auto)
	{
		if (const FInjury* Injury = Injuries.Find(entt::to_integral(Hit.Victim)); Injury && Bridge && Bridge->GetSteps() - Injury->Step <= 150)
		{
			Kind = KindForInjury(Injury->Hediff);
		}
		if (Kind == EBloodWound::Auto)
		{
			Kind = Hit.Fallback == EBloodWound::Auto ? EBloodWound::Blunt : Hit.Fallback;
		}
	}
	++Hits;
	++HitsByKind[static_cast<int32>(Kind)];
	if (Kind == EBloodWound::Burn)
	{
		return;  // burns char; they do not bleed
	}
	FBloodFluid Fluid = Hit.Fluid;
	if (!Hit.bHasFluid && View)
	{
		Fluid = FluidForPawnDef(Sim, View->def);
	}
	HitsByFluid.FindOrAdd(Fluid.Filth.value) += 1;

	float Force = Hit.Force >= 0.0f ? FMath::Clamp(Hit.Force, 0.2f, 2.5f) : ForceFor(Kind, Hit.Damage);
	if (Kind == EBloodWound::Blunt && Hit.Damage < 5.0f && Hit.Force < 0.0f)
	{
		Force *= 0.4f;  // a light blow bruises: a little blood at most
	}
	const int32 MaxDrops = FMath::Max(100, GBloodMaxDrops.GetValueOnGameThread());
	const float Room = FMath::Clamp(static_cast<float>(MaxDrops - Drops.Num()) / static_cast<float>(MaxDrops) * 1.5f, 0.12f, 1.0f);
	const float S = BloodQualityScale() * Room;
	const auto N = [S](float Base) { return FMath::Max(1, FMath::RoundToInt(Base * S)); };

	const FVector Dir = Hit.Direction.IsNearlyZero() ? -FVector::UpVector : Hit.Direction.GetSafeNormal();
	const FVector Back = -Dir;
	const FVector Up = FVector::UpVector;
	USkeletalMeshComponent* Mesh = Hit.Mesh.Get();
	AActor* Ignore = Mesh ? Mesh->GetOwner() : ActorOf(Hit.Victim);
	const float GroundZ = GroundBelow(Hit.At, Ignore);

	// How thick the body is along the shot: the head, the trunk or a limb by the hit's height, bigger animals thicker.
	float Half = Hit.BodyHalfThicknessCm;
	if (Half <= 0.0f)
	{
		const float Height = static_cast<float>(Hit.At.Z) - GroundZ;
		Half = Height > 150.0f ? 9.0f : (Height > 95.0f ? 12.0f : 6.0f);
		if (View && View->bodySize > 0.0f)
		{
			Half = FMath::Max(4.0f, Half * FMath::Sqrt(FMath::Max(0.2f, View->bodySize)));
		}
	}

	FVector ExitAt = Hit.At;
	bool bExit = false;
	switch (Kind)
	{
		case EBloodWound::Gunshot:
		{
			// The way in: a small back-spatter toward the shooter and a puff.
			SprayCone(Hit.At, (Back + Up * 0.2f).GetSafeNormal(), 35.0f, N(14.0f * Force), 120.0f, 380.0f, 0.08f, 0.25f, Fluid, Ignore, GroundZ);
			SprayMist(Hit.At, Back, 30.0f, N(5.0f), 160.0f, 3.5f, 0.4f, 0.5f, Fluid);
			bExit = Force >= 0.55f || Random.FRand() < Force;
			if (bExit)
			{
				// Through and through: the exit is bigger and throws a cone of drops, a jet and a cloud out of the far side.
				ExitAt = Hit.At + Dir * (2.0f * Half);
				const float Speed = FMath::Sqrt(Force);
				SprayCone(ExitAt, (Dir + Up * 0.08f).GetSafeNormal(), 22.0f, N(55.0f * Force), 350.0f * Speed, 1100.0f * Speed, 0.10f, 0.55f, Fluid, Ignore, GroundZ);
				SprayCone(ExitAt, Dir, 8.0f, N(10.0f), 600.0f * Speed, 900.0f * Speed, 0.30f, 0.60f, Fluid, Ignore, GroundZ);
				// A fine pinpoint mist: many small puffs fanning out, and a slower cloud that hangs.
				SprayMist(ExitAt, Dir, 18.0f, N(14.0f * Force), 420.0f, 5.0f, 0.55f, 0.6f, Fluid);
				SprayMist(ExitAt, Dir, 35.0f, N(5.0f * Force), 150.0f, 11.0f, 0.9f, 0.4f, Fluid);
				if (Force > 0.9f && GBloodBits.GetValueOnGameThread() != 0)
				{
					for (int32 I = 0; I < FMath::RoundToInt(2.0f * Force); ++I)
					{
						const FVector V = FMath::VRandCone(Dir, FMath::DegreesToRadians(20.0f)) * Random.FRandRange(300.0f, 600.0f);
						SpawnDrop(ExitAt, V, Random.FRandRange(0.8f, 1.4f), Fluid, Ignore, GroundZ, EDropKind::Chunk);
					}
				}
				++ExitWounds;
			}
			else
			{
				SprayCone(Hit.At, (Back + Up * 0.3f).GetSafeNormal(), 45.0f, N(20.0f * Force), 150.0f, 450.0f, 0.10f, 0.40f, Fluid, Ignore, GroundZ);
				SprayMist(Hit.At, Back, 35.0f, N(8.0f * Force), 220.0f, 4.5f, 0.5f, 0.55f, Fluid);
				SprayMist(Hit.At, Back, 45.0f, N(3.0f), 90.0f, 9.0f, 0.8f, 0.35f, Fluid);
			}
			break;
		}
		case EBloodWound::Cut:
		{
			// Cast-off: a fan along the blade's path, and a ribbon of drops in a tight line.
			const FVector Along = (Dir + Up * 0.25f).GetSafeNormal();
			SprayCone(Hit.At, Along, 30.0f, N(35.0f * Force), 200.0f, 650.0f, 0.10f, 0.45f, Fluid, Ignore, GroundZ);
			SprayCone(Hit.At, Along, 5.0f, N(10.0f * Force), 300.0f, 700.0f, 0.15f, 0.40f, Fluid, Ignore, GroundZ);
			SprayMist(Hit.At, Along, 25.0f, N(6.0f * Force), 260.0f, 4.0f, 0.45f, 0.5f, Fluid);
			break;
		}
		case EBloodWound::Stab:
			SprayCone(Hit.At, (Back + Up * 0.2f).GetSafeNormal(), 40.0f, N(16.0f * Force), 100.0f, 300.0f, 0.12f, 0.45f, Fluid, Ignore, GroundZ);
			SprayMist(Hit.At, Back, 40.0f, N(1.0f), 80.0f, 6.0f, 0.4f, 0.3f, Fluid);
			break;
		case EBloodWound::Bite:
			SprayCone(Hit.At, (Back + Up * 0.3f).GetSafeNormal(), 60.0f, N(24.0f * Force), 120.0f, 400.0f, 0.10f, 0.40f, Fluid, Ignore, GroundZ);
			SprayMist(Hit.At, Back, 50.0f, N(2.0f), 100.0f, 8.0f, 0.45f, 0.35f, Fluid);
			break;
		case EBloodWound::Blast:
		{
			FVector Away = Dir;
			if (Bridge && Bridge->GetSteps() - LastBlastStep <= 3)
			{
				Away = (Hit.At - LastBlastCentre).GetSafeNormal();
			}
			Away = (Away + Up * 0.4f).GetSafeNormal();
			SprayCone(Hit.At, Away, 75.0f, N(60.0f * Force), 300.0f, 900.0f, 0.10f, 0.55f, Fluid, Ignore, GroundZ);
			SprayMist(Hit.At, Away, 70.0f, N(8.0f * Force), 300.0f, 16.0f, 0.8f, 0.5f, Fluid);
			for (int32 I = 0; I < (GBloodBits.GetValueOnGameThread() != 0 ? FMath::RoundToInt(3.0f * Force) : 0); ++I)
			{
				SpawnDrop(Hit.At, FMath::VRandCone(Away, FMath::DegreesToRadians(60.0f)) * Random.FRandRange(300.0f, 700.0f), Random.FRandRange(0.8f, 1.5f), Fluid,
					Ignore, GroundZ, EDropKind::Chunk);
			}
			break;
		}
		case EBloodWound::Blunt:
		default:
			SprayCone(Hit.At, (Back + Up * 0.5f).GetSafeNormal(), 70.0f, N(14.0f * Force), 100.0f, 350.0f, 0.10f, 0.35f, Fluid, Ignore, GroundZ);
			SprayMist(Hit.At, Back, 60.0f, N(2.0f * Force), 90.0f, 7.0f, 0.4f, 0.3f, Fluid);
			break;
	}

	// After-combat splatter on the ground (sneed: like Baldur's Gate 3): every hit that draws blood leaves a readable
	// pattern there and then, not only what the flying drops happen to make. A spatter fan thrown the way the blow went
	// (out of the exit for a shot through), a few slanted drops further out in a line, and a patch under the body.
	// Big enough to read from across a fight, and never culled by distance.
	if (Kind != EBloodWound::Blunt || Hit.Damage >= 8.0f)
	{
		const FVector Flat = FVector(Dir.X, Dir.Y, 0.0f).GetSafeNormal().IsNearlyZero() ? BloodAnyPerpendicular(Up, Random) : FVector(Dir.X, Dir.Y, 0.0f).GetSafeNormal();
		const FVector Throw = Kind == EBloodWound::Gunshot && !bExit ? -Flat : Flat;  // no exit: the blood comes back out of the entry
		const FLinearColor Fresh = DecalColorFor(Fluid, 0.0f, 0.97f);
		const auto Splat = [&](int32 Cell, const FVector& Where, const FVector& Along, float HalfLength, float HalfWidth)
		{
			const FVector Ground(Where.X, Where.Y, GroundBelow(FVector(Where.X, Where.Y, Hit.At.Z), Ignore));
			if (UDecalComponent* Decal = MakeWorldDecal(Cell, Ground, Up, Along, HalfLength, HalfWidth, Fresh))
			{
				Decal->SetFadeScreenSize(0.0f);
				Decal->DecalSize.X = 12.0f;  // reaches over bumps and grass roots (20 also painted dropped items)
				AddStain(Decal, Ground, Up, FMath::Sqrt(HalfLength * HalfWidth), Fluid, false);
			}
		};
		const float Reach = (Kind == EBloodWound::Gunshot ? 55.0f : 35.0f) * FMath::Sqrt(Force);
		const FVector Foot(Hit.At.X, Hit.At.Y, Hit.At.Z);
		// The main fan.
		// (PC look, 2026-11-17: the first sizes read as a few small blotches from the firing line, so everything here is
		// about twice as big and the fan is filled with crowned drops.)
		const float FanHalf = FMath::Clamp(30.0f + 30.0f * Force, 28.0f, 95.0f);
		const FVector FanAt = Foot + Throw * (Reach + FanHalf * 0.6f);
		Splat(BloodCellSpecks + Random.RandRange(0, 1), FanAt, Throw, FanHalf * 1.25f, FanHalf);
		Splat(BloodCellSpecks + Random.RandRange(0, 1), FanAt + Throw * FanHalf * 0.5f, Throw, FanHalf * 0.8f, FanHalf * 0.65f);
		const FVector Across(-Throw.Y, Throw.X, 0.0f);
		// (PC look, 2026-11-18: the fine spatter alone did not show from 9 m. The fan is now carried by solid shapes:
		// two or three irregular splashes where most of it landed, then crowned drops, big near the body and small far
		// out, widening as they go.)
		for (int32 I = 0; I < (Force >= 0.6f ? 3 : 2); ++I)
		{
			const float W = Random.FRandRange(12.0f, 21.0f) * (0.7f + 0.3f * Force);
			const float T = Random.FRandRange(0.0f, 0.6f);
			Splat(BloodCellPool + Random.RandRange(0, 1), Foot + Throw * (Reach * 0.6f + T * FanHalf * 1.6f) + Across * Random.FRandRange(-0.35f, 0.35f) * FanHalf * (0.3f + T),
				FMath::VRandCone(Throw, FMath::DegreesToRadians(25.0f)), W * Random.FRandRange(1.2f, 1.9f), W);
		}
		for (int32 I = 0; I < FMath::Clamp(FMath::RoundToInt((9.0f + 8.0f * Force) * BloodQualityScale()), 5, 26); ++I)
		{
			const float T = FMath::Square(Random.FRand());  // 0 at the body, 1 at the far end; more of them near
			const float W = FMath::Lerp(16.0f, 4.5f, T) * Random.FRandRange(0.6f, 1.2f) * (0.8f + 0.2f * Force);
			const FVector Where = Foot + Throw * (Reach * 0.4f + T * FanHalf * 2.6f) + Across * Random.FRandRange(-1.0f, 1.0f) * FanHalf * (0.25f + 0.75f * T);
			if (T > 0.35f && Random.FRand() < 0.5f)
			{
				Splat(BloodCellDirectional + Random.RandRange(0, 3), Where, (Where - Foot).GetSafeNormal2D(), W * Random.FRandRange(1.8f, 2.6f), W);
			}
			else
			{
				Splat(BloodCellRound + Random.RandRange(0, 3), Where, Throw, W, W);
			}
		}
		// Slanted drops trailing further out.
		const int32 Trail = FMath::Clamp(FMath::RoundToInt(2.0f + 3.0f * Force * BloodQualityScale()), 1, 8);
		for (int32 I = 0; I < Trail; ++I)
		{
			const FVector Spread = FMath::VRandCone(Throw, FMath::DegreesToRadians(22.0f));
			const FVector Along = FVector(Spread.X, Spread.Y, 0.0f).GetSafeNormal();
			const float Out = Reach + FanHalf + Random.FRandRange(20.0f, 160.0f) * FMath::Sqrt(Force);
			const float W = Random.FRandRange(4.0f, 9.0f) * (0.7f + 0.3f * Force);
			Splat(BloodCellDirectional + Random.RandRange(0, 3), Foot + Along * Out, Along, W * Random.FRandRange(1.8f, 3.0f), W);
		}
		// A few round drops close by.
		for (int32 I = 0; I < FMath::Clamp(FMath::RoundToInt(2.0f * Force), 1, 4); ++I)
		{
			const FVector Off = FMath::VRandCone(Up, FMath::DegreesToRadians(89.0f));
			const float W = Random.FRandRange(2.5f, 6.0f);
			Splat(BloodCellRound + Random.RandRange(0, 3), Foot + FVector(Off.X, Off.Y, 0.0f) * Random.FRandRange(10.0f, 45.0f), Throw, W, W);
		}
		// A patch where the body stands or lies: more for bad wounds.
		// Not again where one already lies (more hits on a downed body stacked them into one smooth sheet).
		const int32 PatchesHere = Stains.FilterByPredicate([&Foot](const FStain& Stain) { return Stain.RadiusCm >= 9.0f && FVector::DistSquared2D(Stain.Position, Foot) < 50.0f * 50.0f; }).Num();
		if (PatchesHere < 3 && (Force >= 0.6f || (View && (View->downed || View->dead))))
		{
			// Two or three overlapping splashes with drops round the rim, not one smooth sheet.
			const float PatchHalf = FMath::Clamp(12.0f + 10.0f * Force, 12.0f, 34.0f);
			for (int32 I = 0; I < 3; ++I)
			{
				const FVector Off = FMath::VRandCone(Up, FMath::DegreesToRadians(89.0f)).GetSafeNormal2D();
				const float W = PatchHalf * Random.FRandRange(0.55f, 1.0f);
				Splat(BloodCellPool + Random.RandRange(0, 1), Foot + Off * (I == 0 ? 0.0f : PatchHalf * Random.FRandRange(0.5f, 0.9f)), Off, W, W * Random.FRandRange(0.6f, 0.85f));
			}
			for (int32 I = 0; I < 6; ++I)
			{
				const FVector Off = FMath::VRandCone(Up, FMath::DegreesToRadians(89.0f)).GetSafeNormal2D();
				const float W = Random.FRandRange(2.5f, 7.0f);
				Splat(BloodCellRound + Random.RandRange(0, 3), Foot + Off * PatchHalf * Random.FRandRange(1.0f, 1.9f), Off, W, W);
			}
		}
	}

	// The marks on the body: painted into the skin where the body can take it (USkinBlood: it moves and bends with the
	// skin and never slides), else decals stuck to the nearest bone.
	USkinBlood* Skin = SkinBlood && Mesh && SkinBlood->CanPaint(Mesh) ? SkinBlood.Get() : nullptr;
	if (Skin)
	{
		const FName Bone = Mesh->FindClosestBone(Hit.At);
		const FVector BoneAt = Bone != NAME_None ? Mesh->GetSocketLocation(Bone) : Mesh->Bounds.Origin;
		FVector Into = Kind == EBloodWound::Gunshot ? Dir : (BoneAt - Hit.At).GetSafeNormal();
		if (Into.IsNearlyZero())
		{
			Into = Dir;
		}
		FSkinBloodPaint Mark;
		Mark.At = Hit.At;
		Mark.To = Hit.At;
		Mark.Inward = Into;
		bool bPaint = true;
		switch (Kind)
		{
			case EBloodWound::Gunshot:
				Mark.Core = 1.3f + 0.6f * Force;
				Mark.Soak = 4.5f + 3.0f * Force;
				Mark.Spatter = 8.0f + 4.0f * Force;
				Mark.Run = 6.0f + 6.0f * Force;
				Mark.Hole = 1.0f;
				break;
			case EBloodWound::Cut:
			{
				FVector Along = FVector::VectorPlaneProject(Dir, Into).GetSafeNormal();
				if (Along.IsNearlyZero())
				{
					Along = BloodAnyPerpendicular(Into, Random);
				}
				const float HalfLength = 3.0f + 3.0f * Force;
				Mark.At = Hit.At - Along * HalfLength;
				Mark.To = Hit.At + Along * HalfLength;
				Mark.Core = 0.8f + 0.4f * Force;
				Mark.Soak = 3.5f + 2.5f * Force;
				Mark.Spatter = 7.0f + 3.0f * Force;
				Mark.Run = 6.0f + 6.0f * Force;
				Mark.Hole = 0.8f;
				break;
			}
			case EBloodWound::Stab:
				Mark.Core = 1.5f + 0.5f * Force;
				Mark.Soak = 4.0f + 2.5f * Force;
				Mark.Spatter = 5.0f;
				Mark.Run = 6.0f + 6.0f * Force;
				Mark.Hole = 1.0f;
				break;
			case EBloodWound::Bite:
				Mark.Core = 2.5f + 1.0f * Force;
				Mark.Soak = 5.0f + 2.0f * Force;
				Mark.Spatter = 6.0f;
				Mark.Run = 5.0f + 5.0f * Force;
				Mark.Hole = 0.6f;
				break;
			case EBloodWound::Blast:
				Mark.Core = 2.0f;
				Mark.Soak = 6.0f + 3.0f * Force;
				Mark.Spatter = 14.0f;
				Mark.Run = 6.0f;
				Mark.Hole = 0.5f;
				break;
			case EBloodWound::Blunt:
				bPaint = Hit.Damage >= 8.0f;  // a lighter blow only bruises
				Mark.Core = 1.6f + 0.8f * Force;
				Mark.Soak = 3.5f + 2.0f * Force;
				Mark.Spatter = 4.0f;
				Mark.Run = 4.0f + 4.0f * Force;
				Mark.Hole = 0.3f;
				break;
			default:
				Mark.Core = 1.5f;
				Mark.Soak = 4.0f;
				Mark.Spatter = 6.0f;
				Mark.Run = 6.0f;
				break;
		}
		if (bPaint && Skin->Paint(Mesh, Mark, Fluid, Clock))
		{
			++WoundMarks;
		}
		if (bPaint && bExit)
		{
			// The torn way out: bigger, more blood, more spray over the skin round it. Where it is is a guess along the
			// shot (the body's thickness), so take skin a good way along the line.
			FSkinBloodPaint Exit;
			Exit.At = ExitAt;
			Exit.To = ExitAt;
			Exit.Inward = -Dir;
			Exit.Depth = FMath::Max(6.0f, Half * 0.75f);
			Exit.Core = 2.4f + 1.2f * Force;
			Exit.Soak = 6.0f + 4.0f * Force;
			Exit.Spatter = 12.0f + 6.0f * Force;
			Exit.Run = 10.0f + 8.0f * Force;
			Exit.Hole = 1.0f;
			Skin->Paint(Mesh, Exit, Fluid, Clock);
		}
	}
	else if (Mesh)
	{
		const FName Bone = Mesh->FindClosestBone(Hit.At);
		const FVector BoneAt = Bone != NAME_None ? Mesh->GetSocketLocation(Bone) : Mesh->Bounds.Origin;
		FVector Into = Kind == EBloodWound::Gunshot ? Dir : (BoneAt - Hit.At).GetSafeNormal();
		if (Into.IsNearlyZero())
		{
			Into = Dir;
		}
		const float Age = 0.0f;
		const FLinearColor Fresh = DecalColorFor(Fluid, Age, 1.0f);
		const FVector Side = BloodAnyPerpendicular(Into, Random);
		switch (Kind)
		{
			case EBloodWound::Gunshot:
				AddBodyStain(MakeBodyDecal(Mesh, BloodCellHole, Hit.At, Into, Side, 1.2f + 0.8f * Force, 1.2f + 0.8f * Force, Fresh), Mesh, Hit.Victim, Fluid, true);
				if (bExit)
				{
					AddBodyStain(MakeBodyDecal(Mesh, BloodCellGash, ExitAt, -Dir, BloodAnyPerpendicular(Dir, Random), 3.0f + 3.0f * Force, 2.5f + 2.0f * Force, Fresh), Mesh,
						Hit.Victim, Fluid, true);
				}
				break;
			case EBloodWound::Cut:
			{
				const FVector Along = FVector::VectorPlaneProject(Dir, Into).GetSafeNormal();
				AddBodyStain(MakeBodyDecal(Mesh, BloodCellGash, Hit.At, Into, Along.IsNearlyZero() ? Side : Along, 4.0f + 4.0f * Force, 0.9f + 0.6f * Force, Fresh), Mesh,
					Hit.Victim, Fluid, true);
				break;
			}
			case EBloodWound::Stab:
				AddBodyStain(MakeBodyDecal(Mesh, BloodCellHole, Hit.At, Into, Side, 1.6f + 0.6f * Force, 1.0f + 0.4f * Force, Fresh), Mesh, Hit.Victim, Fluid, true);
				break;
			case EBloodWound::Bite:
				AddBodyStain(MakeBodyDecal(Mesh, BloodCellGash, Hit.At, Into, Side, 3.0f + 1.5f * Force, 2.5f + 1.0f * Force, Fresh), Mesh, Hit.Victim, Fluid, true);
				break;
			case EBloodWound::Blunt:
				if (Hit.Damage >= 8.0f)
				{
					AddBodyStain(MakeBodyDecal(Mesh, BloodCellSmear, Hit.At, Into, Side, 2.5f + 1.5f * Force, 1.2f, Fresh), Mesh, Hit.Victim, Fluid, true);
				}
				break;
			default:
				break;
		}
		if (Kind != EBloodWound::Blunt || Hit.Damage >= 8.0f)
		{
			// Blood soaking out round the wound (into clothes where they cover it: they take the decal too).
			// (PC look, 2026-11-18: at 6 + 7 x force these covered a whole head and read as cow-print blotches.)
			const float Soak = 5.5f + 4.0f * Force;
			AddBodyStain(MakeBodyDecal(Mesh, BloodCellPool + Random.RandRange(0, 1), Hit.At, Into, Side, Soak, Soak * 0.85f, DecalColorFor(Fluid, 0.0f, 0.95f)), Mesh,
				Hit.Victim, Fluid, false);
			if (bExit)
			{
				const float ExitSoak = Soak * 1.3f;
				AddBodyStain(MakeBodyDecal(Mesh, BloodCellPool + Random.RandRange(0, 1), ExitAt, -Dir, BloodAnyPerpendicular(Dir, Random), ExitSoak, ExitSoak * 0.85f,
					DecalColorFor(Fluid, 0.0f, 0.95f)), Mesh, Hit.Victim, Fluid, false);
			}
		}
		if (Kind != EBloodWound::Blunt || Hit.Damage >= 8.0f)
		{
			// Blood splashed over the skin and clothes round the wound: a spatter pattern and a couple of drops with tails
			// running off it, so a hit body reads as bloodied at a glance, not tinted.
			const float Size = 6.0f + 4.0f * Force;
			AddBodyStain(MakeBodyDecal(Mesh, BloodCellSpecks + Random.RandRange(0, 1), Hit.At + Side * Random.FRandRange(0.0f, 4.0f), Into, Side, Size, Size, Fresh), Mesh,
				Hit.Victim, Fluid, false);
			for (int32 I = 0; I < (Force >= 0.6f ? 2 : 1); ++I)
			{
				const FVector Off = FMath::VRandCone(Side, FMath::DegreesToRadians(60.0f));
				const float W = 1.5f + 1.2f * Force;
				AddBodyStain(MakeBodyDecal(Mesh, BloodCellDirectional + Random.RandRange(0, 3), Hit.At + Off * Random.FRandRange(4.0f, 10.0f), Into, Off, W * 2.4f, W, Fresh),
					Mesh, Hit.Victim, Fluid, false);
			}
		}
		++WoundMarks;
	}

	// Wounds that bleed on.
	const bool bBleeds = GBloodBleeding.GetValueOnGameThread() != 0 && (Kind != EBloodWound::Blunt || Hit.Damage >= 8.0f);
	if (bBleeds && Mesh)
	{
		AddBleed(Hit, Kind, Force, Fluid, Hit.At, Kind == EBloodWound::Gunshot ? Back : -(Mesh->GetSocketLocation(Mesh->FindClosestBone(Hit.At)) - Hit.At).GetSafeNormal());
		if (bExit)
		{
			FBloodHit ExitHit = Hit;
			AddBleed(ExitHit, Kind, Force * 1.2f, Fluid, ExitAt, Dir);
		}
	}
	if (GBloodDebug.GetValueOnGameThread() != 0)
	{
		UE_LOG(LogBlood, Display, TEXT("Hit kind %d force %.2f damage %.1f exit %d drops %d stains %d bleeds %d"), static_cast<int32>(Kind), Force, Hit.Damage, bExit ? 1 : 0,
			Drops.Num(), Stains.Num(), Bleeds.Num());
	}
}

void UBloodSubsystem::SprayCone(const FVector& At, const FVector& Axis, float ConeDegrees, int32 Count, float MinSpeed, float MaxSpeed, float MinRadius,
	float MaxRadius, const FBloodFluid& Fluid, AActor* Ignore, float GroundZ)
{
	const float Cone = FMath::DegreesToRadians(ConeDegrees);
	for (int32 I = 0; I < Count; ++I)
	{
		// Most drops are small; a few are big (a power law), and the big ones are slower.
		const float R = FMath::Lerp(MinRadius, MaxRadius, FMath::Pow(Random.FRand(), 2.2f));
		const float SizeShare = (R - MinRadius) / FMath::Max(0.01f, MaxRadius - MinRadius);
		const float Speed = FMath::Lerp(MaxSpeed, MinSpeed, FMath::Clamp(Random.FRand() * 0.7f + SizeShare * 0.3f, 0.0f, 1.0f));
		const FVector V = FMath::VRandCone(Axis, Cone) * Speed;
		SpawnDrop(At + FMath::VRand() * Random.FRandRange(0.0f, 1.5f), V, R, Fluid, Ignore, GroundZ);
	}
}

void UBloodSubsystem::SprayMist(const FVector& At, const FVector& Axis, float ConeDegrees, int32 Count, float Speed, float RadiusCm, float Life, float Opacity,
	const FBloodFluid& Fluid)
{
	for (int32 I = 0; I < Count; ++I)
	{
		FMistPuff Puff;
		Puff.Position = At;
		Puff.Velocity = FMath::VRandCone(Axis, FMath::DegreesToRadians(ConeDegrees)) * Speed * Random.FRandRange(0.5f, 1.2f);
		Puff.RadiusCm = RadiusCm * Random.FRandRange(0.7f, 1.3f);
		Puff.Grow = Random.FRandRange(2.0f, 3.2f);
		Puff.Life = Life * Random.FRandRange(0.8f, 1.25f);
		Puff.Opacity = Opacity;
		Puff.Color = Fluid.Wet;
		Mist.Add(Puff);
		++MistSpawned;
	}
}

void UBloodSubsystem::SpawnDrop(const FVector& At, const FVector& Velocity, float RadiusCm, const FBloodFluid& Fluid, AActor* Ignore, float GroundZ, EDropKind Kind)
{
	if (Drops.Num() >= FMath::Max(100, GBloodMaxDrops.GetValueOnGameThread()))
	{
		return;
	}
	FDrop Drop;
	Drop.Position = At;
	Drop.Velocity = Velocity;
	Drop.RadiusCm = RadiusCm;
	Drop.Life = 4.0f;
	Drop.GroundZ = GroundZ;
	Drop.Fluid = Fluid;
	Drop.Color = Kind == EDropKind::Chunk ? Fluid.Wet * 0.55f : Fluid.Wet;
	Drop.Ignore = Ignore;
	Drop.Kind = Kind;
	Drop.bTraced = RadiusCm >= 0.18f || Kind == EDropKind::Chunk;
	Drop.bSplats = RadiusCm >= 0.09f;
	Drops.Add(Drop);
	++DropsSpawned;
}

void UBloodSubsystem::AddBleed(const FBloodHit& Hit, EBloodWound Kind, float Force, const FBloodFluid& Fluid, const FVector& At, const FVector& Outward)
{
	USkeletalMeshComponent* Mesh = Hit.Mesh.Get();
	if (!Mesh)
	{
		return;
	}
	// One bleeding wound per part and pawn is enough for the drips; a worse hit makes it worse.
	for (FBleed& Bleed : Bleeds)
	{
		if (Bleed.Mesh.Get() == Mesh && Bleed.Part == Hit.Part && Hit.Part >= 0 && FVector::Dist(Bleed.Mesh->GetSocketTransform(Bleed.Bone).TransformPosition(Bleed.LocalPosition), At) < 12.0f)
		{
			Bleed.Force = FMath::Max(Bleed.Force, Force);
			return;
		}
	}
	FBleed Bleed;
	Bleed.Victim = Hit.Victim;
	Bleed.Mesh = Mesh;
	Bleed.Bone = Mesh->FindClosestBone(At);
	const FTransform BoneTransform = Mesh->GetSocketTransform(Bleed.Bone);
	Bleed.LocalPosition = BoneTransform.InverseTransformPosition(At);
	Bleed.LocalOutward = BoneTransform.InverseTransformVectorNoScale(Outward.GetSafeNormal());
	Bleed.Part = Hit.Part;
	Bleed.Kind = Kind;
	Bleed.Force = Force;
	Bleed.Fluid = Fluid;
	Bleed.DripIn = Random.FRandRange(0.2f, 0.8f);
	Bleed.bArterial = Force >= 1.3f && (Kind == EBloodWound::Gunshot || Kind == EBloodWound::Cut || Kind == EBloodWound::Stab) && Random.FRand() < 0.5f;
	Bleed.SpurtIn = Random.FRandRange(0.2f, 0.6f);
	const USimSubsystem* Bridge = BridgeSource.Get();
	const bool bSimPawn = Bridge && Bridge->GetSim() && Hit.Victim != colony::kNoEntity;
	Bleed.NoSimSeconds = bSimPawn ? 0.0f : (Hit.BleedSecondsWithoutSim > 0.0f ? Hit.BleedSecondsWithoutSim : 45.0f);
	Bleeds.Add(Bleed);
}

// ---------------------------------------------------------------------------
// Decals.

UDecalComponent* UBloodSubsystem::MakeWorldDecal(int32 Cell, const FVector& At, const FVector& Normal, const FVector& Along, float HalfLength, float HalfWidth,
	const FLinearColor& Color, bool bScaled)
{
	if (!CellMaterials.IsValidIndex(BloodWorldCells + Cell))
	{
		return nullptr;
	}
	const FVector Into = -Normal.GetSafeNormal();
	FVector Y = FVector::VectorPlaneProject(Along, Into).GetSafeNormal();
	if (Y.IsNearlyZero())
	{
		Y = BloodAnyPerpendicular(Into, Random);
	}
	const FRotator Rotation = FRotationMatrix::MakeFromXY(Into, Y).Rotator();
	const float Scale = bScaled ? FMath::Clamp(GBloodSplatScale.GetValueOnGameThread(), 0.3f, 4.0f) : 1.0f;
	UDecalComponent* Decal = UGameplayStatics::SpawnDecalAtLocation(GetWorld(), CellMaterials[BloodWorldCells + Cell], FVector(5.0f, HalfLength * Scale, HalfWidth * Scale), At,
		Rotation, 0.0f);
	if (Decal)
	{
		Decal->SetDecalColor(Color);
		Decal->SetFadeScreenSize(0.0015f);
		Decal->SetSortOrder(Cell >= BloodCellPool && Cell < BloodCellRun ? 0 : 1);
	}
	return Decal;
}

UDecalComponent* UBloodSubsystem::MakeBodyDecal(USkeletalMeshComponent* Mesh, int32 Cell, const FVector& At, const FVector& Into, const FVector& Along, float HalfLength,
	float HalfWidth, const FLinearColor& Color)
{
	if (!Mesh || !CellMaterials.IsValidIndex(Cell))
	{
		return nullptr;
	}
	const FVector X = Into.GetSafeNormal();
	FVector Y = FVector::VectorPlaneProject(Along, X).GetSafeNormal();
	if (Y.IsNearlyZero())
	{
		Y = BloodAnyPerpendicular(X, Random);
	}
	const FName Bone = Mesh->FindClosestBone(At);
	// The box straddles the skin, its X axis into the body.
	UDecalComponent* Decal = UGameplayStatics::SpawnDecalAttached(CellMaterials[Cell], FVector(7.0f, HalfLength, HalfWidth), Mesh, Bone, At - X * 3.0f,
		FRotationMatrix::MakeFromXY(X, Y).Rotator(), EAttachLocation::KeepWorldPosition, 0.0f);
	if (Decal)
	{
		Decal->SetDecalColor(BloodOnBody(Color));
		Decal->SetFadeScreenSize(0.0f);
		Decal->SetSortOrder(Cell == BloodCellHole || Cell == BloodCellGash ? 3 : 2);
	}
	return Decal;
}

int32 UBloodSubsystem::AddStain(UDecalComponent* Decal, const FVector& At, const FVector& Normal, float RadiusCm, const FBloodFluid& Fluid, bool bPool)
{
	if (!Decal)
	{
		return -1;
	}
	FStain Stain;
	Stain.Decal = Decal;
	Stain.Position = At;
	Stain.Normal = Normal;
	Stain.RadiusCm = RadiusCm;
	Stain.Fluid = Fluid;
	Stain.Cell = CellOf(At);
	Stain.bOutdoor = IsOutdoor(Stain.Cell);
	Stain.bPool = bPool;
	Stain.Serial = NextSerial++;
	Stain.WetSince = Clock;
	Stain.LastVisit = Clock;
	const USimSubsystem* Bridge = BridgeSource.Get();
	Stain.LastVisitStep = Bridge ? Bridge->GetSteps() : 0;
	Stain.LastColorAge = 0.0f;
	Stains.Add(Stain);
	++SplatsMade;
	TrimStains();
	return Stain.Serial;
}

void UBloodSubsystem::AddBodyStain(UDecalComponent* Decal, USkeletalMeshComponent* Mesh, colony::Entity Owner, const FBloodFluid& Fluid, bool bWound)
{
	if (!Decal)
	{
		return;
	}
	FBodyStain Stain;
	Stain.Decal = Decal;
	Stain.Mesh = Mesh;
	// Skin and cloth are mostly seen in softer light than the ground: blood on them is drawn brighter so it reads as red.
	FBloodFluid Bright = Fluid;
	Bright.Wet = BloodOnBody(Fluid.Wet);
	Bright.Dry = BloodOnBody(Fluid.Dry);
	Stain.Owner = Owner;
	Stain.Fluid = Bright;
	Stain.bWound = bWound;
	Stain.WetSince = Clock;
	Stain.LastVisit = Clock;
	Stain.LastColorAge = 0.0f;
	BodyStains.Add(Stain);
	++BodyStainsMade;
	// Each body keeps its newest 32 (wounds before splashes), all bodies 400.
	int32 Mine = 0;
	for (const FBodyStain& Other : BodyStains)
	{
		Mine += Other.Mesh.Get() == Mesh ? 1 : 0;
	}
	for (int32 Index = 0; Index < BodyStains.Num() && (Mine > BloodMaxBodyStainsEach || BodyStains.Num() > BloodMaxBodyStains); )
	{
		FBodyStain& Old = BodyStains[Index];
		const bool bMine = Old.Mesh.Get() == Mesh;
		if ((BodyStains.Num() > BloodMaxBodyStains || bMine) && !(Old.bWound && Mine <= BloodMaxBodyStainsEach + 4 && BodyStains.Num() <= BloodMaxBodyStains))
		{
			if (Old.Decal.IsValid())
			{
				Old.Decal->DestroyComponent();
			}
			Mine -= bMine ? 1 : 0;
			BodyStains.RemoveAt(Index);
			continue;
		}
		++Index;
	}
}

void UBloodSubsystem::TrimStains()
{
	const int32 Max = FMath::Max(50, GBloodMaxStains.GetValueOnGameThread());
	int32 Remove = Stains.Num() - Max;
	if (Remove <= 0)
	{
		return;
	}
	// The oldest go first, small spatter before pools.
	for (int32 Pass = 0; Pass < 2 && Remove > 0; ++Pass)
	{
		for (int32 Index = 0; Index < Stains.Num() && Remove > 0; )
		{
			FStain& Stain = Stains[Index];
			if (Pass == 1 || (!Stain.bPool && Stain.RadiusCm < 12.0f))
			{
				if (Stain.Decal.IsValid())
				{
					Stain.Decal->DestroyComponent();
				}
				Stains.RemoveAt(Index);
				--Remove;
				continue;
			}
			++Index;
		}
	}
}

void UBloodSubsystem::Land(const FDrop& Drop, const FVector& At, const FVector& Normal, UPrimitiveComponent* What)
{
	if (!Drop.bSplats)
	{
		return;
	}
	const FVector N = Normal.GetSafeNormal().IsNearlyZero() ? FVector::UpVector : Normal.GetSafeNormal();
	const float Speed = static_cast<float>(Drop.Velocity.Size());
	const FVector V = Speed > 1.0f ? Drop.Velocity / Speed : -N;
	// The angle it came in at stretches the splat along its path (a forensic spatter's width / length = sin of the angle).
	const float Sine = FMath::Clamp(static_cast<float>(FMath::Abs(FVector::DotProduct(V, N))), 0.28f, 1.0f);
	const float Elongation = 1.0f / Sine;
	const FVector Along = FVector::VectorPlaneProject(V, N).GetSafeNormal();
	const float Scale = Drop.Kind == EDropKind::Chunk ? 1.2f : 1.0f;
	// Splats spread well past the drop (a 3 mm drop leaves a 1-2 cm splat with a crown); never so small that it is lost at range.
	const float HalfWidth = FMath::Clamp(Drop.RadiusCm * (7.0f + Speed * 0.008f) * Scale, 1.2f, 16.0f);
	const float HalfLength = HalfWidth * Elongation;
	const FLinearColor Fresh = DecalColorFor(Drop.Fluid, 0.0f, 1.0f);

	// On a body: a splash that moves with it.
	if (USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(What))
	{
		if (SkinBlood && SkinBlood->CanPaint(Mesh))
		{
			if (Drop.RadiusCm < 0.25f)
			{
				return;  // fine spray: as dots all over a body it read as freckles (PC look, 2026-11-18)
			}
			// Into the skin: a small splash, longer the flatter it came in, with a short run if it was a big one.
			FSkinBloodPaint Mark;
			Mark.At = At - Along * (HalfLength - HalfWidth) * 0.5f;
			Mark.To = At + Along * (HalfLength - HalfWidth) * 0.5f;
			Mark.Inward = -N;
			Mark.Depth = 4.0f;
			Mark.Core = FMath::Clamp(HalfWidth * 0.35f, 0.3f, 2.5f);
			Mark.Soak = 0.0f;
			Mark.Spatter = 0.0f;
			Mark.Run = Drop.RadiusCm > 0.35f ? FMath::Clamp(HalfWidth * 1.5f, 0.0f, 8.0f) : 0.0f;
			Mark.Strength = 0.9f;
			SkinBlood->Paint(Mesh, Mark, Drop.Fluid, Clock);
			return;
		}
		const int32 Cell = Elongation > 1.6f ? BloodCellDirectional + Random.RandRange(0, 3) : BloodCellSpecks + Random.RandRange(0, 1);
		colony::Entity Owner = colony::kNoEntity;
		if (const ASimPawnActor* Pawn = Cast<ASimPawnActor>(Mesh->GetOwner()))
		{
			Owner = Pawn->GetThing();
		}
		AddBodyStain(MakeBodyDecal(Mesh, Cell, At, -N, Along, HalfLength, HalfWidth, Fresh), Mesh, Owner, Drop.Fluid, false);
		return;
	}

	// Near an earlier small splat: they run together (a puddle under drips) instead of a new decal.
	const float NewR = FMath::Sqrt(HalfLength * HalfWidth);
	if (Elongation < 1.6f && NewR < 4.0f)
	{
		const int32 From = FMath::Max(0, Stains.Num() - 160);
		for (int32 Index = Stains.Num() - 1; Index >= From; --Index)
		{
			FStain& Other = Stains[Index];
			if (Other.bGoing || !Other.Decal.IsValid() || Other.RadiusCm > 22.0f || Other.Fluid.Filth != Drop.Fluid.Filth)
			{
				continue;
			}
			if (FVector::DistSquared(Other.Position, At) < FMath::Square((Other.RadiusCm + NewR) * 0.55f) && FVector::DotProduct(Other.Normal, N) > 0.8f)
			{
				const float Grown = FMath::Sqrt(FMath::Square(Other.RadiusCm) + 0.6f * FMath::Square(NewR));
				const float Ratio = Grown / FMath::Max(0.1f, Other.RadiusCm);
				Other.RadiusCm = Grown;
				Other.Decal->DecalSize = FVector(Other.Decal->DecalSize.X, Other.Decal->DecalSize.Y * Ratio, Other.Decal->DecalSize.Z * Ratio);
				Other.Decal->MarkRenderStateDirty();
				Other.WetSince = FMath::Lerp(Other.WetSince, Clock, 0.6f);
				Other.WashLeft = 1.0f;
				Other.LastColorAge = -100.0f;
				++SplatsMerged;
				return;
			}
		}
	}

	int32 Cell;
	if (Drop.Kind == EDropKind::Chunk)
	{
		Cell = BloodCellSmear;
	}
	else if (Elongation > 1.6f)
	{
		Cell = BloodCellDirectional + Random.RandRange(0, 3);
	}
	else if (Drop.RadiusCm < 0.16f)
	{
		Cell = BloodCellSpecks + Random.RandRange(0, 1);
	}
	else
	{
		Cell = BloodCellRound + Random.RandRange(0, 3);
	}
	const bool bWall = N.Z < 0.6f;
	AddStain(MakeWorldDecal(Cell, At, N, Along, HalfLength, HalfWidth, Fresh), At, N, NewR, Drop.Fluid, false);
	if (bWall)
	{
		++WallSplats;
		// A big drop on a wall runs down it.
		if (Drop.RadiusCm >= 0.32f && Random.FRand() < 0.45f)
		{
			const float RunLength = Random.FRandRange(6.0f, 22.0f) * Drop.RadiusCm / 0.4f;
			const FVector Down = FVector::VectorPlaneProject(-FVector::UpVector, N).GetSafeNormal();
			if (!Down.IsNearlyZero())
			{
				// The run picture goes top to bottom along +V; the decal's Z is the picture's up.
				const FVector Into = -N;
				const FRotator Rotation = FRotationMatrix::MakeFromXZ(Into, -Down).Rotator();
				const FVector Middle = At + Down * RunLength * 0.9f;
				if (UDecalComponent* Run = UGameplayStatics::SpawnDecalAtLocation(GetWorld(), CellMaterials[BloodWorldCells + BloodCellRun], FVector(5.0f, HalfWidth * 0.8f, RunLength), Middle, Rotation, 0.0f))
				{
					Run->SetDecalColor(Fresh);
					Run->SetFadeScreenSize(0.0015f);
					Run->SetSortOrder(1);
					AddStain(Run, Middle, N, RunLength * 0.5f, Drop.Fluid, false);
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Per frame.

void UBloodSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = BridgeSource.Get();
	const bool bSim = Bridge && Bridge->GetSim();
	if (bSim && Bridge->GetSimGeneration() != LastGeneration)
	{
		LastGeneration = Bridge->GetSimGeneration();
		ResetAll();
	}
	if (!IsReady())
	{
		Pending.Reset();
		return;
	}
	const int32 Flips = (GBloodFlipU.GetValueOnGameThread() != 0 ? 1 : 0) | (GBloodFlipV.GetValueOnGameThread() != 0 ? 2 : 0) | (GBloodSwapUV.GetValueOnGameThread() != 0 ? 4 : 0) |
		(FMath::RoundToInt(FMath::Clamp(GBloodGroundShade.GetValueOnGameThread(), 0.1f, 2.0f) * 100.0f) << 3);  // (so Blood.GroundShade shows at once)
	if (Flips != LastFlips)
	{
		LastFlips = Flips;
		ApplyCvarsToMaterials();
	}
	const float Speed = bSim ? static_cast<float>(Bridge->GetSpeed()) : 1.0f;
	const float Dt = FMath::Min(DeltaTime, 0.1f) * Speed;
	Clock += Dt;
	const int64 Steps = bSim ? Bridge->GetSteps() : 0;
	const float GameSeconds = LastStep >= 0 ? static_cast<float>(FMath::Max<int64>(0, Steps - LastStep)) : 0.0f;
	LastStep = Steps;

	// Hits whose injury events are in now.
	TArray<FPendingHit> Ready = MoveTemp(Pending);
	Pending.Reset();
	for (const FPendingHit& Entry : Ready)
	{
		DrawHit(Entry.Hit);
	}
	// Injuries with no bullet or blow drawn for them (a trap, a blast, dev damage): blood from the middle of the body.
	if (bSim)
	{
		for (auto It = Injuries.CreateIterator(); It; ++It)
		{
			FInjury& Injury = It.Value();
			if (Injury.Step < 0 || Steps - Injury.Step > 600)
			{
				It.RemoveCurrent();
				continue;
			}
			if (Injury.Step != Steps || Injury.Amount <= 0.0f)
			{
				continue;
			}
			const int64* HitStep = SimHitSteps.Find(It.Key());
			if (HitStep && *HitStep == Injury.Step)
			{
				continue;
			}
			const colony::Entity Pawn = static_cast<colony::Entity>(It.Key());
			ASimPawnActor* Actor = Cast<ASimPawnActor>(ActorOf(Pawn));
			USkeletalMeshComponent* Mesh = Actor ? Actor->GetDecalMesh() : nullptr;
			if (!Mesh)
			{
				continue;
			}
			FBloodHit Hit;
			Hit.Victim = Pawn;
			Hit.Mesh = Mesh;
			Hit.Part = Injury.Part;
			Hit.Damage = Injury.Amount;
			Hit.At = Mesh->Bounds.Origin + FVector(Random.FRandRange(-8.0f, 8.0f), Random.FRandRange(-8.0f, 8.0f), Random.FRandRange(-10.0f, 15.0f));
			const bool bBlast = Steps - LastBlastStep <= 1;
			Hit.Kind = bBlast && KindForInjury(Injury.Hediff) != EBloodWound::Burn ? EBloodWound::Blast : KindForInjury(Injury.Hediff);
			if (Hit.Kind == EBloodWound::Auto)
			{
				Hit.Kind = EBloodWound::Blunt;
			}
			Hit.Direction = bBlast ? (Hit.At - LastBlastCentre).GetSafeNormal() : FMath::VRand();
			DrawHit(Hit);
			Injury.Step = -2;  // drawn
		}
	}

	TracesThisFrame = 0;
	UpdateDrops(Dt);
	UpdateMist(Dt);
	UpdateBleeding(Dt, GameSeconds);
	UpdateStains(Dt, GameSeconds);
	if (SkinBlood)
	{
		const colony::Snapshot* Snapshot = bSim ? &Bridge->GetCurrentSnapshot() : nullptr;
		SkinBlood->Tick(Clock, [Snapshot](const USkeletalMeshComponent* Mesh)
		{
			const ASimPawnActor* Pawn = Snapshot && Mesh ? Cast<ASimPawnActor>(Mesh->GetOwner()) : nullptr;
			const colony::ThingView* View = Pawn ? Snapshot->find(Pawn->GetThing()) : nullptr;
			return View ? View->blood : -1.0f;
		});
	}
	if (bSim)
	{
		SyncFilth();
	}
	DrawInstances();

	if (GBloodDebug.GetValueOnGameThread() != 0 && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(0x0B100D, 0.0f, FColor::Red,
			FString::Printf(TEXT("Blood: drops %d mist %d stains %d (merged %d, walls %d) body %d bleeding %d pools %d filth %d washed %d"), Drops.Num(), Mist.Num(),
				Stains.Num(), SplatsMerged, WallSplats, BodyStains.Num(), Bleeds.Num(), Pools.Num(), FilthDecals.Num(), Washed));
	}
}

void UBloodSubsystem::UpdateDrops(float Dt)
{
	if (Dt <= 0.0f)
	{
		return;
	}
	UWorld* World = GetWorld();
	for (int32 Index = Drops.Num() - 1; Index >= 0; --Index)
	{
		FDrop& Drop = Drops[Index];
		Drop.Age += Dt;
		if (Drop.Age >= Drop.Life)
		{
			Drops.RemoveAtSwap(Index);
			continue;
		}
		// Air slows small drops much more than big ones; gravity pulls them all.
		const float Drag = 0.15f + 0.6f / FMath::Max(0.05f, Drop.RadiusCm);
		const FVector From = Drop.Position;
		Drop.Velocity *= FMath::Exp(-Drag * Dt * (Drop.Kind == EDropKind::Chunk ? 0.3f : 1.0f));
		Drop.Velocity.Z -= BloodGravity * Dt;
		Drop.Position += Drop.Velocity * Dt;

		bool bLanded = false;
		if (Drop.bTraced && TracesThisFrame < BloodTracesPerFrame)
		{
			++TracesThisFrame;
			FHitResult Result;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(BloodDrop), false);
			if (Drop.Ignore.IsValid())
			{
				Params.AddIgnoredActor(Drop.Ignore.Get());
			}
			if (Holder)
			{
				Params.AddIgnoredActor(Holder);
			}
			if (World->LineTraceSingleByChannel(Result, From, Drop.Position, ECC_Visibility, Params))
			{
				Land(Drop, Result.ImpactPoint, Result.ImpactNormal, Result.GetComponent());
				bLanded = true;
			}
		}
		if (!bLanded && Drop.Position.Z <= Drop.GroundZ && Drop.Velocity.Z < 0.0f)
		{
			Land(Drop, FVector(Drop.Position.X, Drop.Position.Y, Drop.GroundZ), FVector::UpVector, nullptr);
			bLanded = true;
		}
		if (bLanded)
		{
			Drops.RemoveAtSwap(Index);
		}
	}
}

void UBloodSubsystem::UpdateMist(float Dt)
{
	for (int32 Index = Mist.Num() - 1; Index >= 0; --Index)
	{
		FMistPuff& Puff = Mist[Index];
		Puff.Age += Dt;
		if (Puff.Age >= Puff.Life)
		{
			Mist.RemoveAtSwap(Index);
			continue;
		}
		Puff.Velocity *= FMath::Exp(-6.0f * Dt);
		Puff.Velocity.Z -= 60.0f * Dt;  // the finest drops settle slowly
		Puff.Position += Puff.Velocity * Dt;
	}
}

void UBloodSubsystem::UpdateBleeding(float Dt, float GameSeconds)
{
	(void)GameSeconds;
	const USimSubsystem* Bridge = BridgeSource.Get();
	const colony::Sim* Sim = Bridge ? Bridge->GetSim() : nullptr;
	const colony::Snapshot* Snapshot = Sim ? &Bridge->GetCurrentSnapshot() : nullptr;
	for (int32 Index = Bleeds.Num() - 1; Index >= 0; --Index)
	{
		FBleed& Bleed = Bleeds[Index];
		USkeletalMeshComponent* Mesh = Bleed.Mesh.Get();
		if (!Mesh)
		{
			Bleeds.RemoveAtSwap(Index);
			continue;
		}
		Bleed.Age += Dt;
		bool bDown = false;
		bool bDead = false;
		float BleedPerDay = 0.0f;
		float BloodLoss = 0.0f;
		float Target = 0.0f;
		if (Bleed.NoSimSeconds > 0.0f)
		{
			// Outside the sim: bleeds a set time, slowing.
			const float T = Bleed.Age / Bleed.NoSimSeconds;
			Target = T < 1.0f ? (0.8f + 2.0f * Bleed.Force) * (1.0f - T) : 0.0f;
			BleedPerDay = Target * 0.3f;
		}
		else if (Snapshot)
		{
			const colony::ThingView* View = Snapshot->find(Bleed.Victim);
			if (!View)
			{
				Bleeds.RemoveAtSwap(Index);
				continue;
			}
			bDown = View->downed;
			bDead = View->dead;
			BleedPerDay = View->bleedPerDay;
			BloodLoss = View->bloodLoss;
			float Share = 0.0f;
			const bool bBleeding = BloodIsWoundBleeding(*Snapshot, *View, Bleed.Part, Share);
			if (bDead)
			{
				// The heart has stopped: what is there runs out and slows to nothing in half a minute.
				Target = Bleed.Rate * FMath::Exp(-Dt / 8.0f);
				Bleed.bArterial = false;
			}
			else if (bBleeding)
			{
				Target = FMath::Clamp(0.6f + 4.0f * Share * FMath::Max(0.2f, View->bleedPerDay), 0.4f, 7.0f);
			}
		}
		Bleed.Rate = Target;
		if (Bleed.Rate < 0.08f)
		{
			Bleed.NotBleedingFor += Dt;
			if (Bleed.NotBleedingFor > 2.0f)
			{
				// Stopped (tended, clotted or run dry): the run stays as a stain on the body.
				if (Bleed.Run.IsValid())
				{
					AddBodyStain(Bleed.Run.Get(), Mesh, Bleed.Victim, Bleed.Fluid, false);
				}
				Bleeds.RemoveAtSwap(Index);
				continue;
			}
			continue;
		}
		Bleed.NotBleedingFor = 0.0f;
		const FTransform BoneTransform = Mesh->GetSocketTransform(Bleed.Bone);
		const FVector At = BoneTransform.TransformPosition(Bleed.LocalPosition);
		const FVector Outward = BoneTransform.TransformVectorNoScale(Bleed.LocalOutward).GetSafeNormal();
		AActor* Ignore = Mesh->GetOwner();

		// Drips.
		Bleed.DripIn -= Dt;
		if (Bleed.DripIn <= 0.0f)
		{
			Bleed.DripIn = 1.0f / Bleed.Rate * Random.FRandRange(0.7f, 1.3f);
			const FVector V = Outward * Random.FRandRange(5.0f, 25.0f) + FVector(Random.FRandRange(-5.0f, 5.0f), Random.FRandRange(-5.0f, 5.0f), -10.0f);
			SpawnDrop(At + Outward * 0.8f, V, Random.FRandRange(0.22f, 0.42f), Bleed.Fluid, Ignore, GroundBelow(At, Ignore));
			++Drips;
		}
		// Spurts with the heartbeat from a cut artery.
		if (Bleed.bArterial && !bDead)
		{
			Bleed.SpurtIn -= Dt;
			if (Bleed.SpurtIn <= 0.0f)
			{
				Bleed.SpurtIn = Random.FRandRange(0.7f, 0.85f) * (bDown ? 1.25f : 1.0f);
				const float Weaken = FMath::Clamp(1.0f - BloodLoss, 0.25f, 1.0f);
				const int32 Count = FMath::RoundToInt(Random.FRandRange(8.0f, 14.0f) * Weaken * BloodQualityScale());
				SprayCone(At, (Outward + FVector::UpVector * 0.3f).GetSafeNormal(), 10.0f, FMath::Max(2, Count), 180.0f * Weaken, 320.0f * Weaken, 0.18f, 0.45f, Bleed.Fluid,
					Ignore, GroundBelow(At, Ignore));
				++Spurts;
			}
		}
		// A run down the skin while upright: painted into the skin, longer as it bleeds on, or a decal.
		const bool bSkinRun = SkinBlood && SkinBlood->CanPaint(Mesh);
		if (!bDown && !bDead && bSkinRun)
		{
			const float MaxRun = 35.0f + Bleed.Force * 15.0f;
			Bleed.RunLengthCm = FMath::Min(MaxRun, Bleed.RunLengthCm + Dt * (1.5f + Bleed.Rate));
			if (Bleed.RunLengthCm - Bleed.SkinRunPainted >= 3.0f)
			{
				if (Bleed.SkinSeed < 0.0f)
				{
					Bleed.SkinSeed = Random.FRandRange(0.0f, 97.0f);
				}
				FSkinBloodPaint Mark;
				Mark.At = At;
				Mark.To = At;
				Mark.Inward = -Outward;
				Mark.Depth = 5.0f;
				Mark.Core = 0.7f + 0.3f * Bleed.Force;
				Mark.Run = Bleed.RunLengthCm;
				Mark.Strength = 0.9f;
				Mark.Seed = Bleed.SkinSeed;
				SkinBlood->Paint(Mesh, Mark, Bleed.Fluid, Clock);
				Bleed.SkinRunPainted = Bleed.RunLengthCm;
			}
		}
		if (!bDown && !bDead && !bSkinRun)
		{
			const float MaxRun = 35.0f + Bleed.Force * 15.0f;
			const float Before = Bleed.RunLengthCm;
			Bleed.RunLengthCm = FMath::Min(MaxRun, Bleed.RunLengthCm + Dt * (1.5f + Bleed.Rate));
			if (!Bleed.Run.IsValid() && Bleed.RunLengthCm >= 3.0f && CellMaterials.IsValidIndex(BloodCellRun))
			{
				const FVector Into = -Outward;
				FVector Up = FVector::VectorPlaneProject(FVector::UpVector, Into).GetSafeNormal();
				if (Up.IsNearlyZero())
				{
					Up = BloodAnyPerpendicular(Into, Random);
				}
				const FName Bone = Bleed.Bone;
				UDecalComponent* Run = UGameplayStatics::SpawnDecalAttached(CellMaterials[BloodCellRun], FVector(7.0f, 1.4f + 0.4f * Bleed.Force, Bleed.RunLengthCm * 0.5f), Mesh, Bone,
					At - Up * Bleed.RunLengthCm * 0.5f - Into * 3.0f, FRotationMatrix::MakeFromXZ(Into, Up).Rotator(), EAttachLocation::KeepWorldPosition, 0.0f);
				if (Run)
				{
					Run->SetDecalColor(DecalColorFor(Bleed.Fluid, 0.0f, 1.0f));
					Run->SetSortOrder(2);
					Bleed.Run = Run;
					++BodyStainsMade;
				}
			}
			else if (Bleed.Run.IsValid() && Bleed.RunLengthCm - Before > 0.0f)
			{
				// Longer, and the middle moves down by half of what it grew.
				UDecalComponent* Run = Bleed.Run.Get();
				const float Grew = Bleed.RunLengthCm * 0.5f - Run->DecalSize.Z;
				Run->DecalSize.Z = Bleed.RunLengthCm * 0.5f;
				Run->AddWorldOffset(-Run->GetUpVector() * Grew);
				Run->SetDecalColor(DecalColorFor(Bleed.Fluid, 0.0f, 1.0f));
				Run->MarkRenderStateDirty();
			}
		}
		// A pool under a body on the ground.
		if ((bDown || bDead) && Bleed.Victim != colony::kNoEntity)
		{
			FPool* Pool = Pools.FindByPredicate([&Bleed](const FPool& P) { return P.Victim == Bleed.Victim && !P.bLeft; });
			FVector Centre = Mesh->Bounds.Origin;
			if (const ASimPawnActor* Pawn = Cast<ASimPawnActor>(Mesh->GetOwner()))
			{
				FVector Pelvis;
				if (Pawn->GetRagdollPelvis(Pelvis))
				{
					Centre = Pelvis;
				}
			}
			// A downed body crawling away leaves a smeared trail, and a new pool where it stops (PC look, 2026-11-20:
			// she crawled 2 m off her pool, and from 12 m only that one patch showed).
			if (Pool && !bDead)
			{
				const FVector Moved = FVector(Centre.X - Pool->LastTrail.X, Centre.Y - Pool->LastTrail.Y, 0.0f);
				if (Moved.SizeSquared() > FMath::Square(30.0f))
				{
					const FVector Middle = Pool->LastTrail + Moved * 0.5f;
					const FVector OnGround(Middle.X, Middle.Y, GroundBelow(FVector(Middle.X, Middle.Y, Centre.Z + 40.0f), Ignore));
					const float Width = Random.FRandRange(5.0f, 9.0f) * (0.7f + 0.3f * Bleed.Force);
					if (UDecalComponent* Smear = MakeWorldDecal(BloodCellSmear, OnGround, FVector::UpVector, Moved.GetSafeNormal(), Moved.Size() * 0.65f, Width,
						DecalColorFor(Bleed.Fluid, 0.0f, 0.9f)))
					{
						Smear->SetFadeScreenSize(0.0f);
						Smear->SetSortOrder(0);
						AddStain(Smear, OnGround, FVector::UpVector, Width, Bleed.Fluid, false);
						++CrawlSmears;
					}
					Pool->LastTrail = Centre;
				}
				if (FVector::DistSquared2D(Centre, Pool->Centre) > FMath::Square(Pool->RadiusCm * 0.6f + 60.0f))
				{
					Pool->bLeft = true;
					Pool = nullptr;
				}
			}
			if (!Pool && CellMaterials.IsValidIndex(BloodCellPool))
			{
				const FVector Ground(Centre.X, Centre.Y, GroundBelow(Centre + FVector(0, 0, 40), Ignore));
				const FLinearColor Fresh = DecalColorFor(Bleed.Fluid, 0.0f, 1.0f);
				UDecalComponent* Decal = MakeWorldDecal(BloodCellPool + Random.RandRange(0, 1), Ground, FVector::UpVector, FMath::VRand(), 6.0f, 6.0f, Fresh);
				if (Decal)
				{
					Decal->SetSortOrder(0);
					FPool NewPool;
					NewPool.Victim = Bleed.Victim;
					NewPool.Centre = Ground;
					NewPool.LastTrail = Ground;
					NewPool.Stain = AddStain(Decal, Ground, FVector::UpVector, 6.0f, Bleed.Fluid, true);
					NewPool.RadiusCm = 6.0f;
					// Drops and a second lobe round it, so its edge is broken (it read as a smooth sheet of paint).
					for (int32 I = 0; I < 7; ++I)
					{
						const FVector Off = FMath::VRandCone(FVector::UpVector, FMath::DegreesToRadians(89.0f)).GetSafeNormal2D();
						const float W = I == 0 ? Random.FRandRange(14.0f, 22.0f) : Random.FRandRange(3.0f, 8.0f);
						const FVector Where = Ground + Off * (I == 0 ? Random.FRandRange(18.0f, 30.0f) : Random.FRandRange(30.0f, 75.0f));
						const FVector OnGround(Where.X, Where.Y, GroundBelow(Where + FVector(0, 0, 40), Ignore));
						if (UDecalComponent* Rim = MakeWorldDecal(I == 0 ? BloodCellPool + Random.RandRange(0, 1) : BloodCellRound + Random.RandRange(0, 3), OnGround, FVector::UpVector, Off,
							W, W * Random.FRandRange(0.7f, 1.0f), Fresh))
						{
							Rim->SetFadeScreenSize(0.0f);
							AddStain(Rim, OnGround, FVector::UpVector, W, Bleed.Fluid, false);
						}
					}
					NewPool.MaxRadiusCm = BloodPoolMaxRadius(BloodLoss);
					Pools.Add(NewPool);
					++PoolsMade;
				}
			}
			else if (Pool)
			{
				Pool->MaxRadiusCm = FMath::Max(Pool->MaxRadiusCm, BloodPoolMaxRadius(BloodLoss));
				const float Grown = FMath::Min(Pool->MaxRadiusCm, Pool->RadiusCm + Dt * (3.0f + 4.0f * Bleed.Rate) / FMath::Max(1.0f, Pool->RadiusCm / 25.0f));
				if (Grown > Pool->RadiusCm + 0.05f)
				{
					if (FStain* Stain = Stains.FindByPredicate([Pool](const FStain& S) { return S.Serial == Pool->Stain; }); Stain && Stain->Decal.IsValid())
					{
						const float Ratio = Grown / FMath::Max(0.1f, Pool->RadiusCm);
						Stain->RadiusCm = Grown;
						Stain->Decal->DecalSize = FVector(Stain->Decal->DecalSize.X, Stain->Decal->DecalSize.Y * Ratio, Stain->Decal->DecalSize.Z * Ratio);
						Stain->Decal->MarkRenderStateDirty();
						Stain->WetSince = Clock;  // still fed: stays wet
					}
					Pool->RadiusCm = Grown;
				}
			}
		}
	}
	Pools.RemoveAll([this](const FPool& Pool) { return !Stains.ContainsByPredicate([&Pool](const FStain& S) { return S.Serial == Pool.Stain; }); });
}

int32 UBloodSubsystem::CellOf(const FVector& World) const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetSim())
	{
		return -1;
	}
	const FVector Local = (World - Bridge->GetWorldOrigin()) / 100.0;
	const colony::CellPos Cell{FMath::FloorToInt(Local.X), FMath::FloorToInt(Local.Y), 0};
	const colony::Map& Map = Bridge->GetSim()->map();
	if (!Map.onGround(Cell))
	{
		return -1;
	}
	return Map.index(Cell);
}

bool UBloodSubsystem::IsOutdoor(int32 Cell) const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (Cell < 0 || !Bridge || !Bridge->GetSim())
	{
		return Cell < 0;
	}
	const colony::Map& Map = Bridge->GetSim()->map();
	return Map.roof(Map.cellAt(Cell)) == colony::Roof::None;
}

float UBloodSubsystem::RainRate() const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	return Bridge && Bridge->GetSim() ? Bridge->GetCurrentSnapshot().weather.rainRate : 0.0f;
}

void UBloodSubsystem::WashCell(int32 Cell, int32 Radius)
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (Cell < 0 || !Bridge || !Bridge->GetSim())
	{
		return;
	}
	const colony::Map& Map = Bridge->GetSim()->map();
	const colony::CellPos Centre = Map.cellAt(Cell);
	for (FStain& Stain : Stains)
	{
		if (Stain.bGoing || Stain.Cell < 0)
		{
			continue;
		}
		const colony::CellPos At = Map.cellAt(Stain.Cell);
		if (FMath::Abs(At.x - Centre.x) <= Radius && FMath::Abs(At.y - Centre.y) <= Radius)
		{
			Stain.bGoing = true;
			++Washed;
		}
	}
}

void UBloodSubsystem::UpdateStains(float Dt, float GameSeconds)
{
	(void)GameSeconds;
	const USimSubsystem* Bridge = BridgeSource.Get();
	const colony::Sim* Sim = Bridge ? Bridge->GetSim() : nullptr;
	const float Rain = RainRate();
	const int64 Steps = Sim ? Bridge->GetSteps() : 0;
	// Under the open sky without rain a stain lasts as long as the sim's blood filth does (Filth def's disappearsInDays).
	float DryWeatherSeconds = 144.0f * 3600.0f;
	if (Sim)
	{
		const int32 Blood = Sim->filth().kindIndex("Blood");
		if (Blood >= 0 && Sim->filth().kinds()[Blood].fadeHours > 0.0f)
		{
			DryWeatherSeconds = Sim->filth().kinds()[Blood].fadeHours * 3600.0f;
		}
	}

	// Going ones fade every frame.
	for (int32 Index = Stains.Num() - 1; Index >= 0; --Index)
	{
		FStain& Stain = Stains[Index];
		if (!Stain.Decal.IsValid())
		{
			Stains.RemoveAt(Index);
			continue;
		}
		if (!Stain.bGoing)
		{
			continue;
		}
		Stain.Fade -= Dt / BloodGoingSeconds;
		if (Stain.Fade <= 0.0f)
		{
			Stain.Decal->DestroyComponent();
			Stains.RemoveAt(Index);
			continue;
		}
		Stain.Decal->SetDecalColor(DecalColorFor(Stain.Fluid, Clock - Stain.WetSince, Stain.Fade * BloodAgeFade(Clock - Stain.WetSince) * (0.35f + 0.65f * Stain.WashLeft)));
	}

	// The rest a slice a frame: drying colour, rain.
	const int32 Slice = FMath::Max(40, Stains.Num() / 30);
	for (int32 I = 0; I < Slice && Stains.Num() > 0; ++I)
	{
		StainCursor = (StainCursor + 1) % Stains.Num();
		FStain& Stain = Stains[StainCursor];
		if (Stain.bGoing)
		{
			continue;
		}
		const float Elapsed = Clock - Stain.LastVisit;
		const float ElapsedGame = Stain.LastVisitStep >= 0 ? static_cast<float>(FMath::Max<int64>(0, Steps - Stain.LastVisitStep)) : 0.0f;
		Stain.LastVisit = Clock;
		Stain.LastVisitStep = Steps;
		if (Sim && Stain.bOutdoor)
		{
			if (Rain > 0.001f)
			{
				Stain.WashLeft -= Elapsed * FMath::Clamp(Rain, 0.2f, 1.5f) / BloodRainWashSeconds;
			}
			Stain.WashLeft -= ElapsedGame / DryWeatherSeconds;
			if (Stain.WashLeft <= 0.0f)
			{
				Stain.WashLeft = 0.0f;
				Stain.bGoing = true;
				++Washed;
				continue;
			}
		}
		const float Age = Clock - Stain.WetSince;
		const float Left = BloodAgeFade(Age);
		if (Left <= 0.0f)
		{
			Stain.bGoing = true;
			++Washed;
			continue;
		}
		if (Age - Stain.LastColorAge > 2.0f || Rain > 0.001f)
		{
			Stain.LastColorAge = Age;
			Stain.Decal->SetDecalColor(DecalColorFor(Stain.Fluid, Age, Left * (0.35f + 0.65f * Stain.WashLeft)));
		}
	}

	// Body stains: dry, and wash off a body standing in the rain.
	const colony::Snapshot* Snapshot = Sim ? &Bridge->GetCurrentSnapshot() : nullptr;
	for (int32 Index = BodyStains.Num() - 1; Index >= 0; --Index)
	{
		FBodyStain& Stain = BodyStains[Index];
		if (!Stain.Decal.IsValid())
		{
			BodyStains.RemoveAt(Index);
			continue;
		}
		const float Elapsed = Clock - Stain.LastVisit;
		if (Elapsed < 0.5f && !Stain.bGoing)
		{
			continue;
		}
		Stain.LastVisit = Clock;
		if (Snapshot && Stain.Owner != colony::kNoEntity)
		{
			if (const colony::ThingView* View = Snapshot->find(Stain.Owner))
			{
				const colony::Map& Map = Sim->map();
				const colony::CellPos Cell{FMath::FloorToInt(View->x), FMath::FloorToInt(View->y), View->level};
				if (Rain > 0.001f && Map.inBounds(Cell) && Map.roof(Cell) == colony::Roof::None)
				{
					Stain.WashLeft -= Elapsed * FMath::Clamp(Rain, 0.2f, 1.5f) / BloodBodyRainWashSeconds;
				}
				// Water washes it too (docs/design/grime.md): swimming quickly, waist-deep slower.
				if (View->swimming)
				{
					Stain.WashLeft -= Elapsed / BloodBodySwimWashSeconds;
				}
				else if (View->waterDepth >= 0.6f)
				{
					Stain.WashLeft -= Elapsed / (2.0f * BloodBodySwimWashSeconds);
				}
			}
		}
		if (Stain.WashLeft <= 0.0f || BloodAgeFade(Clock - Stain.WetSince) <= 0.0f)
		{
			Stain.Decal->DestroyComponent();
			BodyStains.RemoveAt(Index);
			++Washed;
			continue;
		}
		const float Age = Clock - Stain.WetSince;
		if (Age - Stain.LastColorAge > 2.0f || Rain > 0.001f)
		{
			Stain.LastColorAge = Age;
			Stain.Decal->SetDecalColor(DecalColorFor(Stain.Fluid, Age, BloodAgeFade(Age) * (0.3f + 0.7f * Stain.WashLeft)));
		}
	}
	// Body stains made before the pawn was known (drops landing on a body): find the owner by actor.
	for (FBodyStain& Stain : BodyStains)
	{
		if (Stain.Owner == colony::kNoEntity && Stain.Mesh.IsValid())
		{
			if (const ASimPawnActor* Pawn = Cast<ASimPawnActor>(Stain.Mesh->GetOwner()))
			{
				Stain.Owner = Pawn->GetThing();
			}
		}
	}
}

void UBloodSubsystem::SyncFilth()
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	const colony::Snapshot& Snapshot = Bridge->GetCurrentSnapshot();
	const bool bOn = GBloodFilth.GetValueOnGameThread() != 0;
	if (!bOn)
	{
		if (FilthDecals.Num() > 0)
		{
			for (TPair<uint64, FFilthStain>& Pair : FilthDecals)
			{
				if (Pair.Value.Decal.IsValid())
				{
					Pair.Value.Decal->DestroyComponent();
				}
			}
			FilthDecals.Reset();
			LastFilthChanges = ~0ull;
		}
		return;
	}
	const colony::Sim* Sim = Bridge->GetSim();
	if (Snapshot.filthChanges != LastFilthChanges)
	{
		LastFilthChanges = Snapshot.filthChanges;
		const colony::Map& Map = Sim->map();
		TSet<uint64> Seen;
		for (const colony::FilthView& Pile : Snapshot.filth)
		{
			if (!Map.inBounds(Pile.cell))
			{
				continue;
			}
			const int32 CellIndex = Map.index(colony::CellPos{Pile.cell.x, Pile.cell.y, 0});
			const uint64 Key = (static_cast<uint64>(static_cast<uint32>(CellIndex)) << 32) | (static_cast<uint64>(Pile.cell.level & 0xFF) << 24) | (Pile.def.value & 0xFFFFFF);
			Seen.Add(Key);
			FFilthStain* Existing = FilthDecals.Find(Key);
			if (Existing && Existing->Thickness == Pile.thickness && Existing->Decal.IsValid())
			{
				continue;
			}
			if (Existing && Existing->Thickness > Pile.thickness && Existing->bFluid)
			{
				WashCell(CellIndex, 1);  // cleaned a layer: the spray round it goes too
			}
			if (!Existing && FilthDecals.Num() >= BloodMaxFilthDecals)
			{
				continue;
			}
			FFilthStain Stain = Existing ? *Existing : FFilthStain();
			if (!Existing)
			{
				FBloodFluid Fluid;
				bool bFluid = false;
				if (!FluidForFilthDef(Sim, Pile.def, Fluid, &bFluid))
				{
					continue;  // no look: firefoam and the like draw themselves
				}
				Stain.Fluid = Fluid;
				Stain.bFluid = bFluid;
				// Its age is the sim's (game seconds; the clock here is real seconds at game speed).
				Stain.WetSince = Clock - Pile.age / static_cast<float>(colony::kStepsPerRealSecondAt1x);
			}
			else if (Pile.thickness > Existing->Thickness)
			{
				Stain.WetSince = Clock;  // fresh blood on it
			}
			Stain.Thickness = Pile.thickness;
			// One decal per pile, placed and turned by the cell so it does not jump when it thickens.
			const uint32 Hash = static_cast<uint32>(CellIndex) * 2654435761u ^ (Pile.def.value * 40503u + 17u);
			FRandomStream CellRandom(static_cast<int32>(Hash));
			const float Radius = (Stain.bFluid ? 16.0f : 30.0f) + (Stain.bFluid ? 7.0f : 9.0f) * static_cast<float>(Pile.thickness);
			const int32 Cell = Stain.bFluid ? (Pile.thickness >= 2 ? BloodCellPool + CellRandom.RandRange(0, 1) : BloodCellRound + CellRandom.RandRange(0, 3))
			                                : (CellRandom.RandRange(0, 2) == 0 ? BloodCellSmear : BloodCellSpecks + CellRandom.RandRange(0, 1));
			const FVector Top = Bridge->SimToWorld(Pile.cell.x + 0.5f + CellRandom.FRandRange(-0.25f, 0.25f), Pile.cell.y + 0.5f + CellRandom.FRandRange(-0.25f, 0.25f), Pile.cell.level);
			const FVector At(Top.X, Top.Y, GroundBelow(Top + FVector(0, 0, 150), nullptr));
			if (Stain.Decal.IsValid())
			{
				Stain.Decal->DestroyComponent();
			}
			UDecalComponent* Decal = MakeWorldDecal(Cell, At, FVector::UpVector, FVector(CellRandom.FRandRange(-1.0f, 1.0f), CellRandom.FRandRange(-1.0f, 1.0f), 0.0f), Radius,
				Radius * CellRandom.FRandRange(0.75f, 1.0f), DecalColorFor(Stain.Fluid, Stain.bFluid ? Clock - Stain.WetSince : 100000.0f, Stain.bFluid ? 0.9f * BloodAgeFade(Clock - Stain.WetSince) : 0.75f),
				false);  // sized to its 1 m cell already
			if (!Decal)
			{
				continue;
			}
			Decal->SetSortOrder(0);
			Stain.Decal = Decal;
			Stain.LastColorAge = Clock - Stain.WetSince;
			FilthDecals.Add(Key, Stain);
		}
		for (auto It = FilthDecals.CreateIterator(); It; ++It)
		{
			if (!Seen.Contains(It.Key()))
			{
				if (It.Value().bFluid)
				{
					WashCell(static_cast<int32>(It.Key() >> 32), 1);
				}
				if (It.Value().Decal.IsValid())
				{
					It.Value().Decal->DestroyComponent();
				}
				It.RemoveCurrent();
			}
		}
		bFilthSeenOnce = true;
	}
	// Drying colour on fluid piles, a few a frame.
	int32 Budget = 30;
	for (TPair<uint64, FFilthStain>& Pair : FilthDecals)
	{
		FFilthStain& Stain = Pair.Value;
		if (!Stain.bFluid || !Stain.Decal.IsValid())
		{
			continue;
		}
		const float Age = Clock - Stain.WetSince;
		if (Age - Stain.LastColorAge > 4.0f)
		{
			Stain.LastColorAge = Age;
			Stain.Decal->SetDecalColor(DecalColorFor(Stain.Fluid, Age, 0.9f * BloodAgeFade(Age)));
			if (--Budget <= 0)
			{
				break;
			}
		}
	}
}

void UBloodSubsystem::DrawInstances()
{
	const float DropScale = FMath::Clamp(GBloodDropScale.GetValueOnGameThread(), 0.2f, 6.0f);
	TArray<FTransform> DropTransforms;
	TArray<FTransform> ChunkTransforms;
	TArray<FLinearColor> DropColors;
	TArray<FLinearColor> ChunkColors;
	DropTransforms.Reserve(Drops.Num());
	DropColors.Reserve(Drops.Num());
	for (const FDrop& Drop : Drops)
	{
		const float Speed = static_cast<float>(Drop.Velocity.Size());
		const FRotator Rotation = Speed > 1.0f ? Drop.Velocity.Rotation() : FRotator::ZeroRotator;
		// The engine sphere is 100 cm across; a drop is stretched along its flight (motion and surface tension).
		const float R = Drop.RadiusCm * DropScale / 50.0f;
		if (Drop.Kind == EDropKind::Chunk)
		{
			ChunkTransforms.Add(FTransform(FRotator(Drop.Age * 500.0f, Drop.Age * 300.0f, 0.0f), Drop.Position, FVector(R * 1.3f, R, R * 0.7f)));
			ChunkColors.Add(Drop.Color);
		}
		else
		{
			const float Stretch = 1.0f + FMath::Min(Speed * 0.004f, 3.0f);
			DropTransforms.Add(FTransform(Rotation, Drop.Position, FVector(R * Stretch, R / FMath::Sqrt(Stretch), R / FMath::Sqrt(Stretch))));
			DropColors.Add(Drop.Color);
		}
	}
	FVector Camera = FVector::ZeroVector;
	bool bCamera = false;
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
	{
		Camera = PC->PlayerCameraManager->GetCameraLocation();
		bCamera = true;
	}
	TArray<FTransform> MistTransforms;
	TArray<FLinearColor> MistColors;
	MistTransforms.Reserve(Mist.Num());
	for (const FMistPuff& Puff : Mist)
	{
		const float T = Puff.Age / Puff.Life;
		const float Radius = Puff.RadiusCm * (1.0f + (Puff.Grow - 1.0f) * FMath::Sqrt(T));
		const FVector Facing = bCamera ? (Camera - Puff.Position).GetSafeNormal() : FVector::UpVector;
		// The engine plane is 100 cm square, facing +Z.
		MistTransforms.Add(FTransform(FRotationMatrix::MakeFromZ(Facing).Rotator(), Puff.Position, FVector(Radius / 50.0f)));
		FLinearColor Color = Puff.Color;
		Color.A = Puff.Opacity * (1.0f - T) * (1.0f - T);
		MistColors.Add(Color);
	}
	const auto Push = [](UInstancedStaticMeshComponent* Component, const TArray<FTransform>& Transforms, const TArray<FLinearColor>& Colors)
	{
		if (!Component)
		{
			return;
		}
		if (Component->GetInstanceCount() != Transforms.Num())
		{
			Component->ClearInstances();
			if (Transforms.Num() > 0)
			{
				Component->AddInstances(Transforms, false, true);
			}
		}
		else if (Transforms.Num() > 0)
		{
			Component->BatchUpdateInstancesTransforms(0, Transforms, true, false, true);
		}
		for (int32 Index = 0; Index < Colors.Num(); ++Index)
		{
			const float Data[4] = {Colors[Index].R, Colors[Index].G, Colors[Index].B, Colors[Index].A};
			Component->SetCustomData(Index, TArrayView<const float>(Data, 4), false);
		}
		Component->MarkRenderStateDirty();
	};
	Push(DropInstances, DropTransforms, DropColors);
	Push(ChunkInstances, ChunkTransforms, ChunkColors);
	Push(MistInstances, MistTransforms, MistColors);
}
