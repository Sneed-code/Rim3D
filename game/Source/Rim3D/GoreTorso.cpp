// A burst trunk's torso cut into slabs of the body's own skin and clothes (GoreOrgans.h, GoreSliceTorso;
// docs/design/body-health.md "Trunks burst"). sneed, 2026-11-23: "the balls of flesh that come from the torso gibbing
// should have chunks of the torso including breasts etc".

#include "Animation/MorphTarget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Math/RandomStream.h"
#include "ProceduralMeshComponent.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"

#include "GoreGibActor.h"
#include "GoreOrgans.h"
#include "GoreOrganShapes.h"

namespace
{
	/** One vertex of a mesh as drawn now (world), with what the slicing needs to know about its bones. */
	struct FGoreTorsoVertex
	{
		FVector P = FVector::ZeroVector;
		FVector N = FVector::UpVector;
		FVector T = FVector::ForwardVector;
		FVector2D UV0 = FVector2D::ZeroVector;
		FVector2D UV1 = FVector2D::ZeroVector;
		float Torso = 0.0f;    // the share of its weight on the torso's bones
		float Genital = 0.0f;  // on the genital bone
		bool bRead = false;
	};

	struct FGoreTorsoSection
	{
		int32 Material = 0;
		TArray<int32> Triangles;  // global vertex indices, three a triangle
	};

	/** A mesh read: every vertex skinned to where it is drawn now, and its sections' triangles. */
	struct FGoreTorsoMesh
	{
		const USkinnedMeshComponent* Mesh = nullptr;
		TArray<FGoreTorsoVertex> Vertices;
		TArray<FGoreTorsoSection> Sections;
		bool bSkin = false;
		int32 UVCount = 1;
	};

	/** The torso's bones: the pelvis and everything under it but the legs, the arms and the neck with the head. */
	void GoreTorsoBones(const FReferenceSkeleton& Ref, TArray<bool>& Out, int32& OutGenital)
	{
		Out.Init(false, Ref.GetNum());
		OutGenital = Ref.FindBoneIndex(TEXT("genital"));
		const int32 Pelvis = Ref.FindBoneIndex(TEXT("pelvis"));
		if (Pelvis == INDEX_NONE)
		{
			return;
		}
		TArray<int32> Away;
		for (const TCHAR* Name : {TEXT("thigh_l"), TEXT("thigh_r"), TEXT("upperarm_l"), TEXT("upperarm_r"), TEXT("neck_01"), TEXT("head")})
		{
			const int32 Index = Ref.FindBoneIndex(Name);
			if (Index != INDEX_NONE)
			{
				Away.Add(Index);
			}
		}
		for (int32 i = 0; i < Ref.GetNum(); ++i)
		{
			if (i != Pelvis && !Ref.BoneIsChildOf(i, Pelvis))
			{
				continue;
			}
			bool bAway = false;
			for (const int32 A : Away)
			{
				bAway = bAway || i == A || Ref.BoneIsChildOf(i, A);
			}
			Out[i] = !bAway;
		}
	}

	/** Reads a mesh as it is drawn now. False if its vertices or triangles keep no copy to read (a cooked build). */
	bool GoreTorsoRead(const USkinnedMeshComponent& Mesh, bool bAllSections, FGoreTorsoMesh& Out)
	{
		const USkeletalMesh* Asset = Cast<USkeletalMesh>(Mesh.GetSkinnedAsset());
		FSkeletalMeshRenderData* Data = Asset ? Asset->GetResourceForRendering() : nullptr;
		if (!Data || Data->LODRenderData.Num() == 0 || FPlatformProperties::RequiresCookedData())
		{
			return false;
		}
		FSkeletalMeshLODRenderData& Lod = Data->LODRenderData[0];
		const FPositionVertexBuffer& Positions = Lod.StaticVertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& Surface = Lod.StaticVertexBuffers.StaticMeshVertexBuffer;
		const FSkinWeightVertexBuffer& Weights = Lod.SkinWeightVertexBuffer;
		const uint32 Count = Positions.GetNumVertices();
		const TArray<FMatrix44f>& InvRef = Asset->GetRefBasesInvMatrix();
		const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
		if (Count == 0 || !Positions.GetVertexData() || Weights.GetNumVertices() != Count || Surface.GetNumVertices() != Count ||
			!Surface.GetTangentData() || !Surface.GetTexCoordData() || InvRef.Num() != Ref.GetNum())
		{
			return false;
		}
		TArray<uint32> Indices;
		Lod.MultiSizeIndexContainer.GetIndexBuffer(Indices);
		if (Indices.Num() == 0)
		{
			return false;
		}
		TArray<bool> TorsoBone;
		int32 GenitalBone = INDEX_NONE;
		GoreTorsoBones(Ref, TorsoBone, GenitalBone);
		// Each bone's skinning matrix as drawn now (a mesh following another's pose reads that pose).
		TArray<FMatrix44f> Skin;
		Skin.SetNum(Ref.GetNum());
		for (int32 b = 0; b < Ref.GetNum(); ++b)
		{
			Skin[b] = InvRef[b] * FMatrix44f(Mesh.GetBoneTransform(b).ToMatrixWithScale());
		}
		// The body's shape sliders (morph targets) move the vertices before they are skinned.
		TArray<FVector3f> Shaped;
		Shaped.SetNumUninitialized(Count);
		for (uint32 V = 0; V < Count; ++V)
		{
			Shaped[V] = Positions.VertexPosition(V);
		}
		if (const USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(&Mesh))
		{
			for (const TObjectPtr<UMorphTarget>& Morph : Asset->GetMorphTargets())
			{
				const float W = Morph ? Skeletal->GetMorphTarget(Morph->GetFName()) : 0.0f;
				if (FMath::Abs(W) < 0.01f || Morph->GetMorphLODModels().Num() == 0)
				{
					continue;
				}
				for (const FMorphTargetDelta& Delta : Morph->GetMorphLODModels()[0].Vertices)
				{
					if (Delta.SourceIdx < Count)
					{
						Shaped[Delta.SourceIdx] += Delta.PositionDelta * W;
					}
				}
			}
		}
		Out.Mesh = &Mesh;
		Out.Vertices.SetNum(Count);
		Out.UVCount = static_cast<int32>(Surface.GetNumTexCoords());
		const uint32 Influences = Weights.GetMaxBoneInfluences();
		for (const FSkelMeshRenderSection& Section : Lod.RenderSections)
		{
			if (!bAllSections && !const_cast<USkinnedMeshComponent&>(Mesh).IsMaterialSectionShown(Section.MaterialIndex, 0))
			{
				continue;
			}
			for (uint32 V = Section.BaseVertexIndex; V < Section.BaseVertexIndex + Section.NumVertices && V < Count; ++V)
			{
				FVector3f P = FVector3f::ZeroVector;
				FVector3f N = FVector3f::ZeroVector;
				FVector3f T = FVector3f::ZeroVector;
				float Total = 0.0f;
				float Torso = 0.0f;
				float Genital = 0.0f;
				const FVector3f RefN = FVector3f(Surface.VertexTangentZ(V));
				const FVector3f RefT = FVector3f(Surface.VertexTangentX(V));
				for (uint32 K = 0; K < Influences; ++K)
				{
					const float W = static_cast<float>(Weights.GetBoneWeight(V, K));
					const uint32 Local = Weights.GetBoneIndex(V, K);
					if (W <= 0.0f || static_cast<int32>(Local) >= Section.BoneMap.Num())
					{
						continue;
					}
					const int32 Bone = Section.BoneMap[Local];
					if (!Skin.IsValidIndex(Bone))
					{
						continue;
					}
					P += FVector3f(Skin[Bone].TransformPosition(Shaped[V])) * W;
					N += FVector3f(Skin[Bone].TransformVector(RefN)) * W;
					T += FVector3f(Skin[Bone].TransformVector(RefT)) * W;
					Total += W;
					Torso += TorsoBone.IsValidIndex(Bone) && TorsoBone[Bone] ? W : 0.0f;
					Genital += Bone == GenitalBone ? W : 0.0f;
				}
				if (Total <= 0.0f)
				{
					continue;
				}
				FGoreTorsoVertex& Out1 = Out.Vertices[V];
				// The bones' world transforms already hold the component's place, so this is world space.
				Out1.P = FVector(P / Total);
				Out1.N = FVector(N).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
				Out1.T = FVector(T).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
				Out1.UV0 = FVector2D(Surface.GetVertexUV(V, 0));
				Out1.UV1 = Out.UVCount > 1 ? FVector2D(Surface.GetVertexUV(V, 1)) : Out1.UV0;
				Out1.Torso = Torso / Total;
				Out1.Genital = Genital / Total;
				Out1.bRead = true;
			}
			FGoreTorsoSection& Kept = Out.Sections.AddDefaulted_GetRef();
			Kept.Material = Section.MaterialIndex;
			for (uint32 t = 0; t < Section.NumTriangles; ++t)
			{
				const uint32 Base = Section.BaseIndex + t * 3;
				if (Base + 2 >= static_cast<uint32>(Indices.Num()))
				{
					break;
				}
				Kept.Triangles.Add(static_cast<int32>(Indices[Base]));
				Kept.Triangles.Add(static_cast<int32>(Indices[Base + 1]));
				Kept.Triangles.Add(static_cast<int32>(Indices[Base + 2]));
			}
		}
		return Out.Sections.Num() > 0;
	}

	/** One material's part of a slab, copied from one mesh. */
	struct FGoreSlabPart
	{
		int32 Material = 0;
		TMap<int32, int32> Remap;
		TArray<FVector> P;
		TArray<FVector> N;
		TArray<FProcMeshTangent> T;
		TArray<FVector2D> UV0;
		TArray<FVector2D> UV1;
		TArray<int32> I;
	};

	struct FGoreSlab
	{
		// Per mesh read, its parts by material.
		TArray<TArray<FGoreSlabPart>> Parts;
		TArray<int32> SkinTriangles;  // the bare skin's triangles in it (global vertex indices of the skin mesh)
		FVector Sum = FVector::ZeroVector;
		double Count = 0.0;
		FVector NormalSum = FVector::ZeroVector;
		bool bFatty = false;
		bool bGenital = false;
	};

	FGoreSlabPart& GoreSlabPartFor(FGoreSlab& Slab, int32 MeshIndex, int32 Material)
	{
		TArray<FGoreSlabPart>& Parts = Slab.Parts[MeshIndex];
		for (FGoreSlabPart& Part : Parts)
		{
			if (Part.Material == Material)
			{
				return Part;
			}
		}
		FGoreSlabPart& Part = Parts.AddDefaulted_GetRef();
		Part.Material = Material;
		return Part;
	}

	void GoreSlabAddTriangle(FGoreSlab& Slab, FGoreSlabPart& Part, const FGoreTorsoMesh& Read, const int32 Corners[3])
	{
		for (int32 k = 0; k < 3; ++k)
		{
			const int32 V = Corners[k];
			if (const int32* Found = Part.Remap.Find(V))
			{
				Part.I.Add(*Found);
				continue;
			}
			const FGoreTorsoVertex& Vertex = Read.Vertices[V];
			const int32 New = Part.P.Add(Vertex.P);
			Part.N.Add(Vertex.N);
			Part.T.Add(FProcMeshTangent(Vertex.T, false));
			Part.UV0.Add(Vertex.UV0);
			Part.UV1.Add(Vertex.UV1);
			Part.Remap.Add(V, New);
			Part.I.Add(New);
			Slab.Sum += Vertex.P;
			Slab.Count += 1.0;
		}
	}

	FColor GoreSlabColour(const goreshape::C4& C)
	{
		return FLinearColor(C.r, C.g, C.b, C.a).ToFColor(false);
	}

	/**
	 * The raw side of a slab's bare skin: the skin set in by a few cm (more where it was a breast, a buttock or the belly,
	 * mostly fat there), its inner face muscle or fat, and torn sides round its edge with the yellow fat under the skin.
	 * World space; Unreal's clockwise fronts.
	 */
	void GoreSlabFlesh(const FGoreSlab& Slab, const FGoreTorsoMesh& Skin, float Scale, uint32 Seed, TArray<FVector>& P, TArray<FColor>& C, TArray<int32>& I)
	{
		// The skin's corners welded by place (UV seams split a corner into several vertices).
		TMap<FIntVector, int32> WeldOf;
		TArray<FVector> WeldP;
		TArray<FVector> WeldN;
		TArray<int32> Corner;
		Corner.SetNum(Slab.SkinTriangles.Num());
		for (int32 i = 0; i < Slab.SkinTriangles.Num(); ++i)
		{
			const FGoreTorsoVertex& V = Skin.Vertices[Slab.SkinTriangles[i]];
			const FIntVector Key(FMath::RoundToInt32(V.P.X * 20.0), FMath::RoundToInt32(V.P.Y * 20.0), FMath::RoundToInt32(V.P.Z * 20.0));
			int32* Found = WeldOf.Find(Key);
			if (!Found)
			{
				const int32 New = WeldP.Add(V.P);
				WeldN.Add(V.N);
				Found = &WeldOf.Add(Key, New);
			}
			else
			{
				WeldN[*Found] += V.N;
			}
			Corner[i] = *Found;
		}
		// How deep it goes at each corner, and the inner face's corners.
		const int32 Inner = P.Num();
		for (int32 w = 0; w < WeldP.Num(); ++w)
		{
			const FVector N = WeldN[w].GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
			const goreshape::V3 At(static_cast<float>(WeldP[w].X), static_cast<float>(WeldP[w].Y), static_cast<float>(WeldP[w].Z));
			const float Lump = goreshape::noise(At * 0.25f, Seed);
			const float Depth = ((Slab.bFatty ? 3.4f : 2.0f) + 1.8f * Lump) * Scale;
			P.Add(WeldP[w] - N * Depth);
			const float Mottle = goreshape::noise(At * 0.6f, Seed + 3u);
			goreshape::C4 Col = Slab.bFatty ? goreshape::mix(goreshape::kFat, goreshape::kMuscle, 0.25f * Mottle) : goreshape::mix(goreshape::kMuscle, goreshape::kFat, Mottle > 0.78f ? 0.6f : 0.0f);
			Col = goreshape::bloodied(goreshape::scaled(Col, 0.8f + 0.4f * Mottle), At, Seed + 9u, 0.45f);
			C.Add(GoreSlabColour(Col));
		}
		for (int32 i = 0; i + 2 < Corner.Num(); i += 3)
		{
			I.Append({Inner + Corner[i], Inner + Corner[i + 2], Inner + Corner[i + 1]});
		}
		// The torn sides: every edge used by one skin triangle only.
		TMap<uint64, int32> Uses;
		const auto EdgeKey = [](int32 A, int32 B) { return (static_cast<uint64>(FMath::Min(A, B)) << 32) | static_cast<uint32>(FMath::Max(A, B)); };
		for (int32 i = 0; i + 2 < Corner.Num(); i += 3)
		{
			for (int32 k = 0; k < 3; ++k)
			{
				const int32 A = Corner[i + k];
				const int32 B = Corner[i + (k + 1) % 3];
				if (A != B)
				{
					Uses.FindOrAdd(EdgeKey(A, B)) += 1;
				}
			}
		}
		for (int32 i = 0; i + 2 < Corner.Num(); i += 3)
		{
			for (int32 k = 0; k < 3; ++k)
			{
				const int32 A = Corner[i + k];
				const int32 B = Corner[i + (k + 1) % 3];
				if (A == B || Uses.FindRef(EdgeKey(A, B)) != 1)
				{
					continue;
				}
				// From the skin down: a band of skin's underside and yellow fat, then muscle (or more fat) to the inner face.
				const goreshape::V3 At(static_cast<float>(WeldP[A].X), static_cast<float>(WeldP[A].Y), static_cast<float>(WeldP[A].Z));
				const goreshape::C4 Top = goreshape::bloodied(goreshape::kFat, At, Seed + 21u, 0.5f);
				const goreshape::C4 Low = goreshape::bloodied(Slab.bFatty ? goreshape::kFat : goreshape::kMuscle, At, Seed + 22u, 0.55f);
				// (Copied first: adding an element of the same array to it is not allowed.)
				const FVector InA = P[Inner + A];
				const FVector InB = P[Inner + B];
				const int32 AO = P.Add(WeldP[A]);
				const int32 BO = P.Add(WeldP[B]);
				const int32 AI = P.Add(InA);
				const int32 BI = P.Add(InB);
				C.Add(GoreSlabColour(Top));
				C.Add(GoreSlabColour(Top));
				C.Add(GoreSlabColour(Low));
				C.Add(GoreSlabColour(Low));
				I.Append({AO, BI, BO, AO, AI, BI});
			}
		}
	}

	/** Normals for a mesh in Unreal's winding (front = clockwise seen: cross(p2 - p0, p1 - p0) points out). */
	void GoreSlabNormals(const TArray<FVector>& P, const TArray<int32>& I, TArray<FVector>& N)
	{
		N.Init(FVector::ZeroVector, P.Num());
		for (int32 i = 0; i + 2 < I.Num(); i += 3)
		{
			const FVector Face = FVector::CrossProduct(P[I[i + 2]] - P[I[i]], P[I[i + 1]] - P[I[i]]);
			N[I[i]] += Face;
			N[I[i + 1]] += Face;
			N[I[i + 2]] += Face;
		}
		for (FVector& Each : N)
		{
			Each = Each.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		}
	}

	UProceduralMeshComponent* GoreSlabComponent(AActor* Owner)
	{
		UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Owner);
		Mesh->bUseAsyncCooking = false;
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetCanEverAffectNavigation(false);
		Mesh->bReceivesDecals = false;
		Mesh->SetCastShadow(true);
		Mesh->SetupAttachment(Owner->GetRootComponent());
		Mesh->RegisterComponent();
		return Mesh;
	}
}

bool GoreSliceTorso(UWorld* World, const TArray<USkinnedMeshComponent*>& Meshes, const USkinnedMeshComponent* SkinMesh, float BodyScale, int32 Count,
	int32 Seed, TArray<AGoreGibActor*>& Out, TArray<bool>& OutGenital)
{
	if (!World || !SkinMesh || Count <= 0)
	{
		return false;
	}
	const float S = FMath::Clamp(BodyScale, 0.5f, 1.5f);
	// Read the bare body first (all its torso skin, also what clothes hide), then the clothes as drawn.
	TArray<FGoreTorsoMesh> Reads;
	{
		FGoreTorsoMesh& Skin = Reads.AddDefaulted_GetRef();
		Skin.bSkin = true;
		if (!GoreTorsoRead(*SkinMesh, true, Skin))
		{
			UE_LOG(LogTemp, Log, TEXT("Gibs: the body's vertices can't be read; no torso slabs"));
			return false;
		}
	}
	for (const USkinnedMeshComponent* Mesh : Meshes)
	{
		if (!Mesh || Mesh == SkinMesh)
		{
			continue;
		}
		FGoreTorsoMesh Read;
		if (GoreTorsoRead(*Mesh, false, Read))
		{
			Reads.Add(MoveTemp(Read));
		}
	}
	// Which triangles are the torso's: all three corners mostly on its bones, none stretched far (a stitch triangle of
	// a body proxy reaches across the thighs).
	const float Longest = 18.0f * S;
	const auto IsTorso = [Longest](const FGoreTorsoMesh& Read, const int32 V[3])
	{
		const FGoreTorsoVertex& A = Read.Vertices[V[0]];
		const FGoreTorsoVertex& B = Read.Vertices[V[1]];
		const FGoreTorsoVertex& C = Read.Vertices[V[2]];
		if (!A.bRead || !B.bRead || !C.bRead || A.Torso < 0.5f || B.Torso < 0.5f || C.Torso < 0.5f)
		{
			return false;
		}
		if (FVector::Dist(A.P, B.P) > Longest || FVector::Dist(B.P, C.P) > Longest || FVector::Dist(C.P, A.P) > Longest)
		{
			return false;
		}
		return FVector::CrossProduct(B.P - A.P, C.P - A.P).SizeSquared() > 1e-4;
	};
	// The cells' middles: the breasts, the belly and the buttocks where the body has those bones, then the points of the
	// torso's skin furthest from all the others.
	TArray<FVector> Seeds;
	TArray<bool> SeedFatty;
	for (const TCHAR* Name : {TEXT("breast_l"), TEXT("breast_r"), TEXT("belly"), TEXT("butt_l"), TEXT("butt_r")})
	{
		if (Seeds.Num() < Count && SkinMesh->GetBoneIndex(Name) != INDEX_NONE)
		{
			Seeds.Add(SkinMesh->GetBoneLocation(Name, EBoneSpaces::WorldSpace));
			SeedFatty.Add(true);
		}
	}
	TArray<FVector> Centroids;
	for (const FGoreTorsoSection& Section : Reads[0].Sections)
	{
		for (int32 t = 0; t + 2 < Section.Triangles.Num(); t += 3)
		{
			const int32 V[3] = {Section.Triangles[t], Section.Triangles[t + 1], Section.Triangles[t + 2]};
			if (IsTorso(Reads[0], V))
			{
				Centroids.Add((Reads[0].Vertices[V[0]].P + Reads[0].Vertices[V[1]].P + Reads[0].Vertices[V[2]].P) / 3.0);
			}
		}
	}
	if (Centroids.Num() < Count * 4)
	{
		UE_LOG(LogTemp, Log, TEXT("Gibs: only %d torso triangles; no torso slabs"), Centroids.Num());
		return false;
	}
	{
		FRandomStream Pick(Seed);
		TArray<double> Nearest;
		Nearest.Init(TNumericLimits<double>::Max(), Centroids.Num());
		if (Seeds.Num() == 0)
		{
			Seeds.Add(Centroids[Pick.RandRange(0, Centroids.Num() - 1)]);
			SeedFatty.Add(false);
		}
		for (const FVector& Seed1 : Seeds)
		{
			for (int32 i = 0; i < Centroids.Num(); ++i)
			{
				Nearest[i] = FMath::Min(Nearest[i], FVector::DistSquared(Centroids[i], Seed1));
			}
		}
		while (Seeds.Num() < Count)
		{
			int32 Far = 0;
			for (int32 i = 1; i < Centroids.Num(); ++i)
			{
				Far = Nearest[i] > Nearest[Far] ? i : Far;
			}
			// A little off the furthest point, so cuts don't run in neat lines.
			const FVector New = Centroids[Far] + Pick.GetUnitVector() * 2.0 * S;
			Seeds.Add(New);
			SeedFatty.Add(false);
			for (int32 i = 0; i < Centroids.Num(); ++i)
			{
				Nearest[i] = FMath::Min(Nearest[i], FVector::DistSquared(Centroids[i], New));
			}
		}
	}
	const int32 GenitalCell = Seeds.Num();
	TArray<FGoreSlab> Slabs;
	Slabs.SetNum(Seeds.Num() + 1);
	for (int32 c = 0; c < Slabs.Num(); ++c)
	{
		Slabs[c].Parts.SetNum(Reads.Num());
		Slabs[c].bFatty = SeedFatty.IsValidIndex(c) && SeedFatty[c];
		Slabs[c].bGenital = c == GenitalCell;
	}
	// Every torso triangle to its cell: the genitals' own skin to theirs, the rest to the nearest middle (a jagged,
	// torn line between cells, as the triangles fall).
	for (int32 m = 0; m < Reads.Num(); ++m)
	{
		const FGoreTorsoMesh& Read = Reads[m];
		for (const FGoreTorsoSection& Section : Read.Sections)
		{
			for (int32 t = 0; t + 2 < Section.Triangles.Num(); t += 3)
			{
				const int32 V[3] = {Section.Triangles[t], Section.Triangles[t + 1], Section.Triangles[t + 2]};
				if (!IsTorso(Read, V))
				{
					continue;
				}
				int32 Cell = INDEX_NONE;
				if (Read.bSkin && (Read.Vertices[V[0]].Genital >= 0.3f || Read.Vertices[V[1]].Genital >= 0.3f || Read.Vertices[V[2]].Genital >= 0.3f))
				{
					Cell = GenitalCell;
				}
				else
				{
					const FVector Middle = (Read.Vertices[V[0]].P + Read.Vertices[V[1]].P + Read.Vertices[V[2]].P) / 3.0;
					double Best = TNumericLimits<double>::Max();
					for (int32 c = 0; c < Seeds.Num(); ++c)
					{
						const double D = FVector::DistSquared(Middle, Seeds[c]);
						if (D < Best)
						{
							Best = D;
							Cell = c;
						}
					}
				}
				FGoreSlab& Slab = Slabs[Cell];
				GoreSlabAddTriangle(Slab, GoreSlabPartFor(Slab, m, Section.Material), Read, V);
				if (Read.bSkin)
				{
					Slab.SkinTriangles.Append({V[0], V[1], V[2]});
					Slab.NormalSum += Read.Vertices[V[0]].N + Read.Vertices[V[1]].N + Read.Vertices[V[2]].N;
				}
			}
		}
	}
	// Each cell a gib: its meshes about its own middle, one procedural mesh per source mesh (each keeps its own custom
	// data: the cloth's colour, the skin's blood), a section per material, the raw flesh as one more on the skin's.
	for (int32 c = 0; c < Slabs.Num(); ++c)
	{
		FGoreSlab& Slab = Slabs[c];
		if (Slab.Count < 3.0)
		{
			continue;
		}
		const FVector Middle = Slab.Sum / Slab.Count;
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AGoreGibActor* Gib = World->SpawnActor<AGoreGibActor>(AGoreGibActor::StaticClass(), FTransform(FQuat::Identity, Middle), Params);
		if (!Gib)
		{
			continue;
		}
		UProceduralMeshComponent* SkinPart = nullptr;
		for (int32 m = 0; m < Reads.Num(); ++m)
		{
			TArray<FGoreSlabPart>& Parts = Slab.Parts[m];
			const bool bFlesh = Reads[m].bSkin && Slab.SkinTriangles.Num() >= 3;
			if (Parts.Num() == 0 && !bFlesh)
			{
				continue;
			}
			UProceduralMeshComponent* Mesh = GoreSlabComponent(Gib);
			const USkinnedMeshComponent* Source = Reads[m].Mesh;
			int32 Section = 0;
			for (FGoreSlabPart& Part : Parts)
			{
				for (FVector& Point : Part.P)
				{
					Point -= Middle;
				}
				Mesh->CreateMeshSection(Section, Part.P, Part.I, Part.N, Part.UV0, Part.UV1, TArray<FVector2D>(), TArray<FVector2D>(), TArray<FColor>(), Part.T, false);
				Mesh->SetMaterial(Section, Source ? Source->GetMaterial(Part.Material) : nullptr);
				++Section;
			}
			if (bFlesh)
			{
				TArray<FVector> P;
				TArray<FColor> C;
				TArray<int32> I;
				GoreSlabFlesh(Slab, Reads[m], S, static_cast<uint32>(Seed * 29 + c), P, C, I);
				for (FVector& Point : P)
				{
					Point -= Middle;
				}
				TArray<FVector> N;
				GoreSlabNormals(P, I, N);
				TArray<FVector2D> UV;
				UV.Init(FVector2D::ZeroVector, P.Num());
				Mesh->CreateMeshSection(Section, P, I, N, UV, C, TArray<FProcMeshTangent>(), false);
				Mesh->SetMaterial(Section, GoreFleshMaterial());
				SkinPart = Mesh;
			}
			if (Source)
			{
				const TArray<float>& Custom = Source->GetCustomPrimitiveData().Data;
				for (int32 i = 0; i < Custom.Num(); ++i)
				{
					Mesh->SetCustomPrimitiveDataFloat(i, Custom[i]);
				}
			}
			if (!SkinPart)
			{
				SkinPart = Mesh;
			}
		}
		if (!SkinPart)
		{
			Gib->Destroy();
			continue;
		}
		Gib->SetChunk(SkinPart);
		Gib->SetRestAxis(Slab.NormalSum);
		Out.Add(Gib);
		OutGenital.Add(Slab.bGenital);
	}
	UE_LOG(LogTemp, Log, TEXT("Gibs: torso cut into %d slabs from %d meshes (%d torso triangles of skin)"), Out.Num(), Reads.Num(), Centroids.Num());
	return Out.Num() > 0;
}
