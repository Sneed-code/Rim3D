#include "GoreOrgans.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Math/RandomStream.h"
#include "ProceduralMeshComponent.h"

#include "BloodSubsystem.h"
#include "GoreGibActor.h"
#include "GoreOrganShapes.h"
#include "SimSubsystem.h"

namespace
{
	TAutoConsoleVariable<int32> GGoreSlabs(TEXT("Gore.TorsoSlabs"), 12,
		TEXT("How many slabs of skin and flesh a burst trunk's torso is cut into (the genitals come off as one more); 0 = only small chunks."), ECVF_Default);
	TAutoConsoleVariable<int32> GGoreOrgans(TEXT("Gore.Organs"), 1,
		TEXT("1: a burst trunk throws out its organs, intestines, spine and ribs (body-health.md \"Trunks burst\"); 0: only chunks."), ECVF_Default);
	TAutoConsoleVariable<int32> GGoreRopes(TEXT("Gore.GutRopes"), 4,
		TEXT("How many ropes of intestine a burst trunk spills."), ECVF_Default);
	TAutoConsoleVariable<int32> GGoreRopeMax(TEXT("Gore.GutRopeMax"), 40,
		TEXT("At most this many ropes of intestine lie at once; the oldest go first."), ECVF_Default);

	constexpr float GoreRopeGravity = 980.0f;
	constexpr float GoreRopeSubstep = 1.0f / 40.0f;
	constexpr float GoreRopeFriction = 0.35f;   // the share of its sideways speed a point on the ground loses each substep
	constexpr float GoreRopeShrinkSeconds = 3.0f;

	TArray<TWeakObjectPtr<AGoreRopeActor>>& GoreRopeLive()
	{
		static TArray<TWeakObjectPtr<AGoreRopeActor>> Live;
		return Live;
	}

	float GoreCvarFloat(const TCHAR* Name, float Default)
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
		return Var ? Var->GetFloat() : Default;
	}

	/** The sim's ground under a point (world cm). */
	float GoreFloorZ(const USimSubsystem& Bridge, const FVector& At)
	{
		const FVector Here = (At - Bridge.GetWorldOrigin()) / 100.0;
		return static_cast<float>(Bridge.SimToWorld(static_cast<float>(Here.X), static_cast<float>(Here.Y), 0).Z);
	}

	/** A shape's arrays for a procedural mesh, `Scale` times its size; triangles turned to Unreal's clockwise fronts. */
	void GoreShapeArrays(const goreshape::Mesh& M, float Scale, TArray<FVector>& P, TArray<FVector>& N, TArray<FVector2D>& UV, TArray<FColor>& C,
		TArray<int32>* T, bool bColours)
	{
		const int32 Count = static_cast<int32>(M.P.size());
		P.Reset(Count);
		N.Reset(Count);
		UV.Reset(Count);
		C.Reset(bColours ? Count : 0);
		for (int32 i = 0; i < Count; ++i)
		{
			P.Add(FVector(M.P[i].x, M.P[i].y, M.P[i].z) * Scale);
			N.Add(i < static_cast<int32>(M.N.size()) ? FVector(M.N[i].x, M.N[i].y, M.N[i].z) : FVector::UpVector);
			UV.Add(FVector2D(M.U[i], M.V[i]));
			if (bColours)
			{
				C.Add(FLinearColor(M.C[i].r, M.C[i].g, M.C[i].b, M.C[i].a).ToFColor(false));
			}
		}
		if (T)
		{
			T->Reset(static_cast<int32>(M.T.size()));
			for (size_t i = 0; i + 2 < M.T.size(); i += 3)
			{
				T->Add(M.T[i]);
				T->Add(M.T[i + 2]);
				T->Add(M.T[i + 1]);
			}
		}
	}

	UMaterialInterface* GoreLoadMaterial(const TCHAR* Path)
	{
		return LoadObject<UMaterialInterface>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
}

UMaterialInterface* GoreFleshMaterial()
{
	UMaterialInterface* Found = GoreLoadMaterial(TEXT("/Game/Rim3D/Generated/Blood/M_Gore.M_Gore"));
	return Found ? Found : GoreLoadMaterial(TEXT("/Game/Rim3D/Generated/Blood/M_BloodDrop.M_BloodDrop"));
}

UMaterialInterface* GoreOrganMaterial()
{
	UMaterialInterface* Found = GoreLoadMaterial(TEXT("/Game/Rim3D/Generated/Blood/M_Organ.M_Organ"));
	return Found ? Found : GoreFleshMaterial();
}

UProceduralMeshComponent* GoreMakeOrganMesh(AActor* Owner, USceneComponent* Parent, EGoreOrgan Kind, float Scale, int32 Seed, float& OutRest, FVector& OutRestAxis)
{
	OutRest = 4.0f;
	OutRestAxis = FVector::UpVector;
	if (!Owner || !Parent || Scale <= 0.0f)
	{
		return nullptr;
	}
	const uint32 S = static_cast<uint32>(Seed);
	goreshape::Mesh M;
	switch (Kind)
	{
	case EGoreOrgan::Heart: M = goreshape::makeHeart(S); break;
	case EGoreOrgan::LungLeft: M = goreshape::makeLung(S, true); break;
	case EGoreOrgan::LungRight: M = goreshape::makeLung(S, false); break;
	case EGoreOrgan::Liver: M = goreshape::makeLiver(S); break;
	case EGoreOrgan::Stomach: M = goreshape::makeStomach(S); break;
	case EGoreOrgan::Kidney: M = goreshape::makeKidney(S); break;
	case EGoreOrgan::Spleen: M = goreshape::makeSpleen(S); break;
	case EGoreOrgan::Windpipe: M = goreshape::makeWindpipe(S); break;
	case EGoreOrgan::Spine: M = goreshape::makeSpine(S, 4); break;
	case EGoreOrgan::Ribs: M = goreshape::makeRibRack(S); break;
	case EGoreOrgan::Fat: M = goreshape::makeFatLump(S, 9.0f + static_cast<float>(S % 7u)); break;
	}
	if (M.P.empty())
	{
		return nullptr;
	}
	// Its flattest side: the box's smallest side.
	FBox Box(ForceInit);
	for (const goreshape::V3& Point : M.P)
	{
		Box += FVector(Point.x, Point.y, Point.z);
	}
	const FVector Size = Box.GetSize();
	OutRestAxis = Size.X <= Size.Y && Size.X <= Size.Z ? FVector::XAxisVector : (Size.Y <= Size.Z ? FVector::YAxisVector : FVector::ZAxisVector);
	OutRest = FMath::Max(0.5f * static_cast<float>(Size.GetMin()) * Scale, 0.6f);

	TArray<FVector> P;
	TArray<FVector> N;
	TArray<FVector2D> UV;
	TArray<FColor> C;
	TArray<int32> T;
	GoreShapeArrays(M, Scale, P, N, UV, C, &T, true);
	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Owner);
	Mesh->bUseAsyncCooking = false;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->bReceivesDecals = false;
	Mesh->SetCastShadow(true);
	Mesh->CreateMeshSection(0, P, T, N, UV, C, TArray<FProcMeshTangent>(), false);
	if (UMaterialInterface* Material = GoreOrganMaterial())
	{
		Mesh->SetMaterial(0, Material);
	}
	Mesh->SetupAttachment(Parent);
	Mesh->RegisterComponent();
	return Mesh;
}

// ---------------------------------------------------------------------------
// Ropes of gut

AGoreRopeActor::AGoreRopeActor()
{
	PrimaryActorTick.bCanEverTick = true;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

void AGoreRopeActor::GetLive(const UWorld* World, TArray<AGoreRopeActor*>& Out)
{
	for (const TWeakObjectPtr<AGoreRopeActor>& Rope : GoreRopeLive())
	{
		if (Rope.IsValid() && Rope->GetWorld() == World)
		{
			Out.Add(Rope.Get());
		}
	}
}

void AGoreRopeActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GoreRopeLive().RemoveAll([this](const TWeakObjectPtr<AGoreRopeActor>& Rope) { return !Rope.IsValid() || Rope.Get() == this; });
	Super::EndPlay(EndPlayReason);
}

void AGoreRopeActor::Setup(const TArray<FVector>& Nodes, const TArray<FVector>& Velocity, bool bInLarge, float InScale, int32 InSeed)
{
	bLarge = bInLarge;
	Scale = FMath::Clamp(InScale, 0.4f, 1.6f);
	Seed = InSeed;
	Radius = goreshape::gutRadius(bLarge) * Scale;
	Pos = Nodes;
	Prev.SetNum(Nodes.Num());
	float Total = 0.0f;
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		const FVector V = Velocity.IsValidIndex(i) ? Velocity[i] : FVector::ZeroVector;
		Prev[i] = Pos[i] - V * GoreRopeSubstep;
		Total += i > 0 ? static_cast<float>(FVector::Dist(Nodes[i], Nodes[i - 1])) : 0.0f;
	}
	Segment = Nodes.Num() > 1 ? Total / (Nodes.Num() - 1) : 6.0f;
	// The actor sits on the ground under the middle, so shrinking away at the end sinks the rope into the ground.
	FVector Middle = FVector::ZeroVector;
	for (const FVector& Node : Nodes)
	{
		Middle += Node;
	}
	Middle /= FMath::Max(Nodes.Num(), 1);
	if (const USimSubsystem* Bridge = GetWorld() ? GetWorld()->GetSubsystem<USimSubsystem>() : nullptr; Bridge && Bridge->GetSim())
	{
		Middle.Z = GoreFloorZ(*Bridge, Middle);
	}
	SetActorLocationAndRotation(Middle, FQuat::Identity);
	Tube = NewObject<UProceduralMeshComponent>(this);
	Tube->bUseAsyncCooking = false;
	Tube->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Tube->SetCanEverAffectNavigation(false);
	Tube->bReceivesDecals = false;
	Tube->SetCastShadow(true);
	Tube->SetupAttachment(Root);
	Tube->RegisterComponent();
	if (UMaterialInterface* Material = GoreOrganMaterial())
	{
		Tube->SetMaterial(0, Material);
	}
	Rebuild(true);
	TArray<TWeakObjectPtr<AGoreRopeActor>>& Live = GoreRopeLive();
	Live.RemoveAll([](const TWeakObjectPtr<AGoreRopeActor>& Rope) { return !Rope.IsValid(); });
	while (Live.Num() >= FMath::Max(GGoreRopeMax.GetValueOnGameThread(), 1))
	{
		const TWeakObjectPtr<AGoreRopeActor> Old = Live[0];
		Live.RemoveAt(0);
		if (Old.IsValid())
		{
			Old->Destroy();
		}
	}
	Live.Add(this);
}

void AGoreRopeActor::Rebuild(bool bFirst)
{
	if (!Tube || Pos.Num() < 2)
	{
		return;
	}
	const FVector Origin = GetActorLocation();
	const float Shown = FMath::Max(static_cast<float>(GetActorScale3D().X), 0.01f);
	std::vector<goreshape::V3> Nodes;
	Nodes.reserve(Pos.Num());
	for (const FVector& Node : Pos)
	{
		const FVector Local = (Node - Origin) / Shown;
		Nodes.push_back(goreshape::V3(static_cast<float>(Local.X), static_cast<float>(Local.Y), static_cast<float>(Local.Z)));
	}
	const goreshape::Mesh M = goreshape::makeGut(Nodes, bLarge, Scale, static_cast<uint32>(Seed), 10, 3, bFirst);
	TArray<FVector> P;
	TArray<FVector> N;
	TArray<FVector2D> UV;
	TArray<FColor> C;
	if (bFirst)
	{
		TArray<int32> T;
		GoreShapeArrays(M, 1.0f, P, N, UV, C, &T, true);
		Tube->CreateMeshSection(0, P, T, N, UV, C, TArray<FProcMeshTangent>(), false);
	}
	else
	{
		// The same number of points, so the mesh moves in place (its colours and UVs as they were).
		GoreShapeArrays(M, 1.0f, P, N, UV, C, nullptr, false);
		Tube->UpdateMeshSection(0, P, N, TArray<FVector2D>(), TArray<FColor>(), TArray<FProcMeshTangent>());
	}
}

void AGoreRopeActor::Push(const FVector& DeltaVelocity, const FVector& At, float InRadius)
{
	if (DeltaVelocity.IsNearlyZero() || Pos.Num() == 0)
	{
		return;
	}
	const float Reach = FMath::Max(InRadius, 5.0f);
	for (int32 i = 0; i < Pos.Num(); ++i)
	{
		const float D = static_cast<float>(FVector::Dist(Pos[i], At));
		const float W = D <= Reach ? 1.0f : FMath::Clamp(1.0f - (D - Reach) / (2.0f * Reach), 0.0f, 1.0f) * 0.35f;
		Prev[i] -= DeltaVelocity * W * GoreRopeSubstep;
	}
	bAtRest = false;
	StillFor = 0.0f;
}

void AGoreRopeActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;
	const float Life = FMath::Max(GoreCvarFloat(TEXT("Gore.GibSeconds"), 300.0f), 1.0f);
	if (Age >= Life)
	{
		const float Left = 1.0f - (Age - Life) / GoreRopeShrinkSeconds;
		if (Left <= 0.0f)
		{
			Destroy();
			return;
		}
		SetActorScale3D(FVector(FMath::Max(Left, 0.01f)));
	}
	if (bAtRest || Pos.Num() < 2)
	{
		return;
	}
	const USimSubsystem* Bridge = GetWorld() ? GetWorld()->GetSubsystem<USimSubsystem>() : nullptr;
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	// Fixed substeps (the verlet points keep their last move, one substep's worth, as their velocity).
	Accum = FMath::Min(Accum + FMath::Min(DeltaSeconds, 0.05f), GoreRopeSubstep * 3.0f);
	int32 Substeps = 0;
	while (Accum >= GoreRopeSubstep)
	{
		Accum -= GoreRopeSubstep;
		++Substeps;
	}
	if (Substeps == 0)
	{
		return;
	}
	const float H = GoreRopeSubstep;
	float Moved = 0.0f;
	int32 Touching = 0;
	TArray<float> Floor;
	Floor.SetNumUninitialized(Pos.Num());
	for (int32 i = 0; i < Pos.Num(); ++i)
	{
		Floor[i] = GoreFloorZ(*Bridge, Pos[i]) + Radius;
	}
	for (int32 Sub = 0; Sub < Substeps; ++Sub)
	{
		for (int32 i = 0; i < Pos.Num(); ++i)
		{
			const FVector V = (Pos[i] - Prev[i]) * 0.995f;
			Prev[i] = Pos[i];
			Pos[i] += V + FVector(0.0, 0.0, -GoreRopeGravity * H * H);
		}
		// Fixed lengths between the points (a few passes), and a gentle stiffness so it lies in curves, not kinks.
		for (int32 Pass = 0; Pass < 6; ++Pass)
		{
			for (int32 i = 0; i + 1 < Pos.Num(); ++i)
			{
				const FVector D = Pos[i + 1] - Pos[i];
				const float Len = static_cast<float>(D.Size());
				if (Len < 1e-3f)
				{
					continue;
				}
				const FVector Fix = D * (0.5f * (Len - Segment) / Len);
				Pos[i] += Fix;
				Pos[i + 1] -= Fix;
			}
			for (int32 i = 0; i + 2 < Pos.Num(); ++i)
			{
				const FVector D = Pos[i + 2] - Pos[i];
				const float Len = static_cast<float>(D.Size());
				const float Want = Segment * 1.6f;
				if (Len < Want && Len > 1e-3f)
				{
					const FVector Fix = D * (0.08f * (Len - Want) / Len);
					Pos[i] += Fix;
					Pos[i + 2] -= Fix;
				}
			}
		}
		// The ground: no falling through, no bounce, friction on what lies on it.
		for (int32 i = 0; i < Pos.Num(); ++i)
		{
			if (Pos[i].Z <= Floor[i])
			{
				Pos[i].Z = Floor[i];
				Prev[i].Z = Pos[i].Z;
				Prev[i].X = FMath::Lerp(Prev[i].X, Pos[i].X, GoreRopeFriction);
				Prev[i].Y = FMath::Lerp(Prev[i].Y, Pos[i].Y, GoreRopeFriction);
				Touching += Sub == Substeps - 1 ? 1 : 0;
			}
			Moved = FMath::Max(Moved, static_cast<float>(FVector::Dist(Pos[i], Prev[i])));
		}
	}
	if (!bLanded && Touching * 3 >= Pos.Num())
	{
		bLanded = true;
		// A wet slap where it comes down.
		if (UBloodSubsystem* Blood = GetWorld()->GetSubsystem<UBloodSubsystem>(); Blood && Blood->IsReady())
		{
			FBloodHit Hit;
			Hit.At = GetMiddle();
			Hit.Direction = FVector(0.0, 0.0, -1.0);
			Hit.Kind = EBloodWound::Blunt;
			Hit.Damage = 9.0f;
			Hit.Force = 0.4f;
			Blood->AddHit(Hit);
		}
	}
	StillFor = Moved < 0.04f ? StillFor + DeltaSeconds : 0.0f;
	if (StillFor > 0.5f)
	{
		bAtRest = true;
	}
	Rebuild(false);
}

// ---------------------------------------------------------------------------
// Pushes

void GorePushLine(UWorld* World, const FVector& From, const FVector& To, const FVector& Dir, float Damage)
{
	if (!World)
	{
		return;
	}
	const float Speed = FMath::Clamp(150.0f + 14.0f * FMath::Max(Damage, 0.0f), 150.0f, 700.0f);
	TArray<AGoreGibActor*> Gibs;
	AGoreGibActor::GetLive(World, Gibs);
	for (AGoreGibActor* Gib : Gibs)
	{
		const FVector Middle = Gib->GetActorLocation();
		const FVector Closest = FMath::ClosestPointOnSegment(Middle, From, To);
		if (FVector::Dist(Closest, Middle) <= Gib->GetReachCm())
		{
			Gib->Push(Dir.GetSafeNormal() * Speed + FVector(0.0, 0.0, Speed * 0.35f));
		}
	}
	TArray<AGoreRopeActor*> Ropes;
	AGoreRopeActor::GetLive(World, Ropes);
	for (AGoreRopeActor* Rope : Ropes)
	{
		const FVector Middle = Rope->GetMiddle();
		const FVector Closest = FMath::ClosestPointOnSegment(Middle, From, To);
		if (FVector::Dist(Closest, Middle) <= 120.0)
		{
			Rope->Push(Dir.GetSafeNormal() * Speed * 0.8f + FVector(0.0, 0.0, Speed * 0.3f), Closest, 14.0f);
		}
	}
}

void GorePushRadial(UWorld* World, const FVector& At, float RadiusCm, float Speed, const FVector& Bias)
{
	if (!World || RadiusCm <= 0.0f)
	{
		return;
	}
	TArray<AGoreGibActor*> Gibs;
	AGoreGibActor::GetLive(World, Gibs);
	for (AGoreGibActor* Gib : Gibs)
	{
		const FVector Offset = Gib->GetActorLocation() - At;
		const float Away = static_cast<float>(Offset.Size());
		if (Away < RadiusCm)
		{
			const float Falloff = 1.0f - Away / RadiusCm;
			Gib->Push((Offset.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector) + FVector(0.0, 0.0, 0.5)).GetSafeNormal() * Speed * Falloff + Bias * Falloff);
		}
	}
	TArray<AGoreRopeActor*> Ropes;
	AGoreRopeActor::GetLive(World, Ropes);
	for (AGoreRopeActor* Rope : Ropes)
	{
		const FVector Offset = Rope->GetMiddle() - At;
		const float Away = static_cast<float>(Offset.Size());
		if (Away < RadiusCm + 60.0f)
		{
			const float Falloff = FMath::Clamp(1.0f - Away / (RadiusCm + 60.0f), 0.0f, 1.0f);
			Rope->Push((Offset.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector) + FVector(0.0, 0.0, 0.5)).GetSafeNormal() * Speed * Falloff + Bias * Falloff,
				At, RadiusCm);
		}
	}
}

// ---------------------------------------------------------------------------
// The burst

int32 GoreBurstTrunk(UWorld* World, const TArray<USkinnedMeshComponent*>& Meshes, const USkinnedMeshComponent* SkinMesh, const FVector& Centre,
	const FVector& InUp, float BodyScale, float Strength, const FLinearColor& Skin, int32 Seed, colony::Entity Victim, bool bOrgans)
{
	if (!World)
	{
		return 0;
	}
	if (!bOrgans || GGoreOrgans.GetValueOnGameThread() == 0)
	{
		if (UBloodSubsystem* Blood = World->GetSubsystem<UBloodSubsystem>(); Blood && Blood->IsReady())
		{
			Blood->AddBurstPool(Centre, Victim, 120.0f * BodyScale, SkinMesh ? const_cast<AActor*>(SkinMesh->GetOwner()) : nullptr);
		}
		return AGoreGibActor::Burst(World, Centre, BodyScale, Strength, Skin, Seed, Victim);
	}
	FRandomStream Random(Seed);
	const float S = FMath::Clamp(BodyScale, 0.5f, 1.5f);
	const float Speed = FMath::Clamp(Strength, 0.8f, 1.6f) * FMath::Max(GoreCvarFloat(TEXT("Gore.GibSpeed"), 1.0f), 0.0f);
	const FVector Up = InUp.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	FVector Side;
	FVector Front;
	Up.FindBestAxisVectors(Side, Front);
	AGoreGibActor::MakeRoom(60);
	int32 Made = 0;
	const auto Spin = [&Random](float Most) { return FRotator(Random.FRandRange(-Most, Most), Random.FRandRange(-Most, Most), Random.FRandRange(-Most, Most)); };
	// Out from the trunk all round and up, as the organs and slabs go: more up than sideways, never down into the ground.
	const auto Outward = [&](const FVector& From)
	{
		FVector Dir = (From - Centre).GetSafeNormal(UE_SMALL_NUMBER, Random.GetUnitVector());
		Dir += Random.GetUnitVector() * 0.45f;
		Dir.Z = FMath::Abs(Dir.Z) * 0.6f + 0.45f;
		return Dir.GetSafeNormal();
	};

	// 1. The torso's own skin and clothes, cut into slabs.
	TArray<AGoreGibActor*> Slabs;
	TArray<bool> Genital;
	const int32 SlabCount = FMath::Clamp(GGoreSlabs.GetValueOnGameThread(), 0, 40);
	const bool bSliced = SlabCount > 0 && GoreSliceTorso(World, Meshes, SkinMesh, S, SlabCount, Seed, Slabs, Genital);
	for (int32 i = 0; i < Slabs.Num(); ++i)
	{
		AGoreGibActor* Slab = Slabs[i];
		const FVector Dir = Outward(Slab->GetActorLocation());
		const float Thrown = Random.FRandRange(180.0f, 400.0f) * Speed * (Genital.IsValidIndex(i) && Genital[i] ? 1.15f : 1.0f);
		Slab->Throw(Dir * Thrown, Spin(700.0f), 2.5f * S, true);
		AGoreGibActor::Track(Slab);
		++Made;
	}

	// 2. The organs, from about where they lay: heart and lungs high in the chest, the liver and stomach under them, the
	// kidneys at the back, fat, the spine and the ribs.
	struct FGoreOrganThrow
	{
		EGoreOrgan Kind;
		float UpCm;
	};
	const FGoreOrganThrow Organs[] = {
		{EGoreOrgan::Heart, 18.0f}, {EGoreOrgan::LungLeft, 20.0f}, {EGoreOrgan::LungRight, 20.0f}, {EGoreOrgan::Windpipe, 30.0f},
		{EGoreOrgan::Liver, 4.0f}, {EGoreOrgan::Stomach, 2.0f}, {EGoreOrgan::Spleen, 4.0f}, {EGoreOrgan::Kidney, -4.0f}, {EGoreOrgan::Kidney, -4.0f},
		{EGoreOrgan::Spine, 0.0f}, {EGoreOrgan::Ribs, 14.0f}, {EGoreOrgan::Ribs, 10.0f}, {EGoreOrgan::Fat, -8.0f}, {EGoreOrgan::Fat, -10.0f},
		{EGoreOrgan::Fat, -2.0f},
	};
	int32 OrganIndex = 0;
	for (const FGoreOrganThrow& Organ : Organs)
	{
		const FVector Start = Centre + Up * (Organ.UpCm * S) + (Side * Random.FRandRange(-9.0f, 9.0f) + Front * Random.FRandRange(-6.0f, 6.0f)) * S;
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AGoreGibActor* Gib = World->SpawnActor<AGoreGibActor>(AGoreGibActor::StaticClass(), FTransform(Random.GetUnitVector().Rotation(), Start), Params);
		if (!Gib)
		{
			continue;
		}
		float Rest = 4.0f;
		FVector Axis = FVector::UpVector;
		UProceduralMeshComponent* Mesh = GoreMakeOrganMesh(Gib, Gib->GetRootComponent(), Organ.Kind, S, Seed * 37 + OrganIndex * 11 + 5, Rest, Axis);
		++OrganIndex;
		if (!Mesh)
		{
			Gib->Destroy();
			continue;
		}
		Gib->SetChunk(Mesh);
		Gib->SetRestAxis(Axis);
		// Heavier than the slabs: they don't go as far (most within 1 to 2.5 m).
		Gib->Throw(Outward(Start) * Random.FRandRange(130.0f, 300.0f) * Speed, Spin(420.0f), Rest, true);
		AGoreGibActor::Track(Gib);
		++Made;
	}

	// 3. The intestines: ropes coiled where the belly was, flung out to uncoil and drape over the ground.
	const int32 Ropes = FMath::Clamp(GGoreRopes.GetValueOnGameThread(), 0, 12);
	for (int32 r = 0; r < Ropes; ++r)
	{
		const bool bLargeGut = r == 1;
		const int32 Count = bLargeGut ? 18 : (r == 0 ? 28 : 20);
		const float Coil = (bLargeGut ? 8.0f : 5.5f) * S;
		const float Turn = bLargeGut ? 0.9f : 1.0f;
		const FVector Base = Centre - Up * ((6.0f + 3.0f * r) * S) + (Side * Random.FRandRange(-6.0f, 6.0f) + Front * Random.FRandRange(2.0f, 8.0f)) * S;
		FVector Fling = Front * Random.FRandRange(-1.0f, 1.0f) + Side * Random.FRandRange(-1.0f, 1.0f);
		Fling.Z = 0.0;
		Fling = Fling.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector) * Random.FRandRange(140.0f, 300.0f) * Speed + FVector(0.0, 0.0, Random.FRandRange(160.0f, 300.0f) * Speed);
		const float Phase = Random.FRandRange(0.0f, 2.0f * PI);
		TArray<FVector> Nodes;
		TArray<FVector> Velocity;
		for (int32 k = 0; k < Count; ++k)
		{
			const float A = Phase + k * Turn;
			Nodes.Add(Base + (Side * FMath::Cos(A) + Front * FMath::Sin(A)) * Coil + Up * (k * 0.5f * S));
			// The far end flies harder than the near one, so the coil pays out as it goes.
			const float Share = static_cast<float>(k) / FMath::Max(Count - 1, 1);
			Velocity.Add(Fling * FMath::Lerp(0.55f, 1.25f, Share) + Random.GetUnitVector() * 60.0f);
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (AGoreRopeActor* Rope = World->SpawnActor<AGoreRopeActor>(AGoreRopeActor::StaticClass(), FTransform(Base), Params))
		{
			Rope->Setup(Nodes, Velocity, bLargeGut, S, Seed * 53 + r);
			++Made;
		}
	}

	// 4. Small bits (meat, bone shards, ribs, dark wet lumps) and the burst's sprays and pool; more of them where the
	// slabs could not be cut.
	Made += AGoreGibActor::Burst(World, Centre, S, Strength, Skin, Seed + 7, Victim, bSliced ? 10 : 22);

	// 5. More blood: hard sprays all round and pools where the trunk lay.
	if (UBloodSubsystem* Blood = World->GetSubsystem<UBloodSubsystem>(); Blood && Blood->IsReady())
	{
		for (int32 i = 0; i < 10; ++i)
		{
			FVector Dir = Random.GetUnitVector();
			Dir.Z = FMath::Abs(Dir.Z) * 0.7f;
			FBloodHit Hit;
			Hit.At = Centre + Random.GetUnitVector() * 8.0f * S;
			Hit.Direction = Dir;
			Hit.Kind = EBloodWound::Blast;
			Hit.Damage = 50.0f;
			Hit.Force = FMath::Clamp(1.8f * Speed, 1.0f, 3.0f);
			Hit.Victim = Victim;
			Hit.BleedSecondsWithoutSim = 1.5f;
			Blood->AddHit(Hit);
		}
		for (int32 i = 0; i < 4; ++i)
		{
			FBloodHit Pool;
			Pool.At = Centre + (Side * Random.FRandRange(-30.0f, 30.0f) + Front * Random.FRandRange(-30.0f, 30.0f)) * S;
			Pool.Direction = FVector(0.0, 0.0, -1.0);
			Pool.Kind = EBloodWound::Blunt;
			Pool.Damage = 70.0f;
			Pool.Force = 1.6f;
			Pool.Victim = Victim;
			Pool.BleedSecondsWithoutSim = 2.0f;
			Blood->AddHit(Pool);
		}
		// One big pool spreading out where it burst (sneed 2026-11-24: "when the torso explodes it should leave a big
		// blood puddle where it explodes"), about 1.2 m across a man-sized body's middle.
		Blood->AddBurstPool(Centre, Victim, 120.0f * S, SkinMesh ? const_cast<AActor*>(SkinMesh->GetOwner()) : nullptr);
	}
	UE_LOG(LogTemp, Log, TEXT("Gibs: a trunk burst open: %d slabs (sliced %d), %d organs, %d gut ropes, %d pieces in all"), Slabs.Num(), bSliced ? 1 : 0, OrganIndex,
		Ropes, Made);
	return Made;
}
