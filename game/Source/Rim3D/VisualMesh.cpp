#include "VisualMesh.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

namespace
{
	struct FVisTriangle
	{
		FVector3f A, B, C;
		FLinearColor Color;
	};

	void VisAddTriangle(TArray<FVisTriangle>& Out, const FVector3f& A, FVector3f B, FVector3f C, const FVector3f& Outward, const FLinearColor& Color)
	{
		if (FVector3f::DotProduct(FVector3f::CrossProduct(C - A, B - A), Outward) < 0.0f)
		{
			Swap(B, C);
		}
		Out.Add({A, B, C, Color});
	}

	/** A solid of revolution about the Z axis from rings of (height, radius) in cm, bottom to top; a ring of radius 0 is a point. */
	void VisLathe(TArray<FVisTriangle>& Out, const TArray<FVector2f>& Rings, int32 Sides, const FLinearColor& Color, bool bBottomCap, bool bTopCap)
	{
		const auto Ring = [&Rings, Sides](int32 Index, int32 Side)
		{
			const float Angle = 2.0f * PI * static_cast<float>(Side % Sides) / static_cast<float>(Sides);
			return FVector3f(FMath::Cos(Angle) * Rings[Index].Y, FMath::Sin(Angle) * Rings[Index].Y, Rings[Index].X);
		};
		for (int32 R = 0; R + 1 < Rings.Num(); ++R)
		{
			for (int32 S = 0; S < Sides; ++S)
			{
				const FVector3f P00 = Ring(R, S);
				const FVector3f P01 = Ring(R, S + 1);
				const FVector3f P10 = Ring(R + 1, S);
				const FVector3f P11 = Ring(R + 1, S + 1);
				const auto Radial = [](const FVector3f& A, const FVector3f& B, const FVector3f& C)
				{
					const FVector3f Middle = (A + B + C) / 3.0f;
					return FVector3f(Middle.X, Middle.Y, 0.0f);
				};
				if (Rings[R].Y > 0.0f && Rings[R + 1].Y > 0.0f)
				{
					VisAddTriangle(Out, P00, P01, P11, Radial(P00, P01, P11), Color);
					VisAddTriangle(Out, P00, P11, P10, Radial(P00, P11, P10), Color);
				}
				else if (Rings[R].Y <= 0.0f)
				{
					VisAddTriangle(Out, P00, P10, P11, Radial(P00, P10, P11), Color);
				}
				else
				{
					VisAddTriangle(Out, P00, P01, P10, Radial(P00, P01, P10), Color);
				}
			}
		}
		const auto Cap = [&](int32 Index, float Direction)
		{
			if (Rings[Index].Y <= 0.0f)
			{
				return;
			}
			const FVector3f Centre(0.0f, 0.0f, Rings[Index].X);
			for (int32 S = 0; S < Sides; ++S)
			{
				VisAddTriangle(Out, Centre, Ring(Index, S), Ring(Index, S + 1), FVector3f(0.0f, 0.0f, Direction), Color);
			}
		};
		if (bBottomCap)
		{
			Cap(0, -1.0f);
		}
		if (bTopCap)
		{
			Cap(Rings.Num() - 1, 1.0f);
		}
	}

	void VisBox(TArray<FVisTriangle>& Out, float Width, float Depth, float Height, const FLinearColor& Color)
	{
		const float X = Width * 0.5f;
		const float Y = Depth * 0.5f;
		const FVector3f Corner[8] = {
			FVector3f(-X, -Y, 0.0f), FVector3f(X, -Y, 0.0f), FVector3f(X, Y, 0.0f), FVector3f(-X, Y, 0.0f),
			FVector3f(-X, -Y, Height), FVector3f(X, -Y, Height), FVector3f(X, Y, Height), FVector3f(-X, Y, Height)};
		const auto Quad = [&](int32 A, int32 B, int32 C, int32 D, const FVector3f& Outward)
		{
			VisAddTriangle(Out, Corner[A], Corner[B], Corner[C], Outward, Color);
			VisAddTriangle(Out, Corner[A], Corner[C], Corner[D], Outward, Color);
		};
		Quad(0, 1, 2, 3, FVector3f(0.0f, 0.0f, -1.0f));
		Quad(4, 5, 6, 7, FVector3f(0.0f, 0.0f, 1.0f));
		Quad(0, 1, 5, 4, FVector3f(0.0f, -1.0f, 0.0f));
		Quad(2, 3, 7, 6, FVector3f(0.0f, 1.0f, 0.0f));
		Quad(1, 2, 6, 5, FVector3f(1.0f, 0.0f, 0.0f));
		Quad(3, 0, 4, 7, FVector3f(-1.0f, 0.0f, 0.0f));
	}

	/** One part as triangles in the thing's own frame, its slide (cm) not yet applied. */
	void VisAddPart(TArray<FVisTriangle>& Out, const VisualMesh::FPart& Part, const FLinearColor& Color)
	{
		const int32 First = Out.Num();
		const float Radius = FMath::Max(Part.Size.X, 0.01f);
		const float Height = Part.Size.Z;
		switch (Part.Shape)
		{
			case VisualMesh::EShape::Box:
				VisBox(Out, Part.Size.X, Part.Size.Y, Height, Color);
				break;
			case VisualMesh::EShape::Cylinder:
			{
				// A cap under a part that stands on the ground is never seen; one under a part lifted off it is.
				const TArray<FVector2f> Rings = {FVector2f(0.0f, Radius), FVector2f(Height, Radius)};
				VisLathe(Out, Rings, 6, Color, Part.At.Z > 2.0f, true);
				break;
			}
			case VisualMesh::EShape::Cone:
			{
				const TArray<FVector2f> Rings = {FVector2f(0.0f, Radius), FVector2f(Height, 0.0f)};
				VisLathe(Out, Rings, 5, Color, true, false);
				break;
			}
			case VisualMesh::EShape::Sphere:
			{
				TArray<FVector2f> Rings;
				for (int32 K = 0; K <= 4; ++K)
				{
					const float Theta = PI * static_cast<float>(K) / 4.0f;
					Rings.Add(FVector2f(Height * 0.5f * (1.0f - FMath::Cos(Theta)), K == 0 || K == 4 ? 0.0f : Radius * FMath::Sin(Theta)));
				}
				VisLathe(Out, Rings, 7, Color, false, false);
				break;
			}
		}
		// Round parts may be oval; turn the part about its own middle, then put it where it goes.
		const float Squash = Part.Shape == VisualMesh::EShape::Box ? 1.0f : Part.Size.Y / Radius;
		const FRotator3f Rotation = Part.Rotation;
		const FVector3f Pivot(0.0f, 0.0f, Height * 0.5f);
		const auto Place = [&](const FVector3f& P)
		{
			const FVector3f Local(P.X, P.Y * Squash, P.Z);
			return Rotation.RotateVector(Local - Pivot) + Pivot + Part.At;
		};
		for (int32 I = First; I < Out.Num(); ++I)
		{
			Out[I].A = Place(Out[I].A);
			Out[I].B = Place(Out[I].B);
			Out[I].C = Place(Out[I].C);
		}
	}

	std::string VisPath(const std::string& Base, size_t Index, const char* Field)
	{
		return Base + "." + std::to_string(Index) + "." + Field;
	}
}

FLinearColor VisualMesh::ReadColor(const colony::Def& Def, const std::string& Path, const FLinearColor& Fallback)
{
	if (Def.count(Path.c_str()) < 3)
	{
		return Fallback;
	}
	// Written as the sRGB colour you'd pick in a paint program; the vertex colour is linear.
	const auto Linear = [&Def, &Path](size_t I, double Default) { return FMath::Pow(static_cast<float>(Def.numberAt(Path.c_str(), I, Default)), 2.2f); };
	return FLinearColor(Linear(0, 0.3), Linear(1, 0.5), Linear(2, 0.2));
}

bool VisualMesh::ReadPlantPart(const colony::Def& Def, const std::string& Path, FPart& Out)
{
	if (!Def.has(Path.c_str()))
	{
		return false;
	}
	const std::string Shape = Def.text((Path + ".shape").c_str(), "cone");
	Out.Shape = Shape == "cylinder" ? EShape::Cylinder : (Shape == "sphere" ? EShape::Sphere : EShape::Cone);
	const float Radius = static_cast<float>(Def.number((Path + ".radius").c_str(), 0.3)) * 100.0f;
	const float Height = static_cast<float>(Def.number((Path + ".height").c_str(), 0.5)) * 100.0f;
	Out.Size = FVector3f(Radius, Radius, Height);
	Out.At = FVector3f(0.0f, 0.0f, static_cast<float>(Def.number((Path + ".lift").c_str(), 0.0)) * 100.0f);
	Out.Color = ReadColor(Def, Path + ".color", FLinearColor(0.25f, 0.45f, 0.15f));
	return true;
}

void VisualMesh::ReadParts(const colony::Def& Def, const std::string& Path, const FVector2f& FootprintCm, TArray<FPart>& Out)
{
	const std::string List = Path + ".parts";
	const size_t Count = Def.count(List.c_str());
	FLinearColor Tint(1.0f, 1.0f, 1.0f);
	if (Def.count((Path + ".tint").c_str()) >= 3)
	{
		Tint = ReadColor(Def, Path + ".tint", Tint);
	}
	for (size_t I = 0; I < Count; ++I)
	{
		const auto Field = [&](const char* Name) { return VisPath(List, I, Name); };
		FPart Part;
		const std::string Shape = Def.text(Field("shape").c_str(), "box");
		Part.Shape = Shape == "cylinder" ? EShape::Cylinder : (Shape == "sphere" ? EShape::Sphere : (Shape == "cone" ? EShape::Cone : EShape::Box));
		if (Part.Shape == EShape::Box)
		{
			const std::string SizePath = Field("size");
			float X = static_cast<float>(Def.numberAt(SizePath.c_str(), 0, 0.0)) * 100.0f;
			float Y = static_cast<float>(Def.numberAt(SizePath.c_str(), 1, 0.0)) * 100.0f;
			const float Z = static_cast<float>(Def.numberAt(SizePath.c_str(), 2, 0.5)) * 100.0f;
			// An edge of 0 or less is measured from the footprint's: 0 fills it, -0.1 leaves 5 cm bare on each side.
			X = X > 0.0f ? X : FMath::Max(FootprintCm.X + X, 1.0f);
			Y = Y > 0.0f ? Y : FMath::Max(FootprintCm.Y + Y, 1.0f);
			Part.Size = FVector3f(X, Y, Z);
		}
		else
		{
			const float Radius = static_cast<float>(Def.number(Field("radius").c_str(), 0.3)) * 100.0f;
			Part.Size = FVector3f(Radius, Radius, static_cast<float>(Def.number(Field("height").c_str(), 0.5)) * 100.0f);
		}
		const std::string AtPath = Field("at");
		Part.At = FVector3f(static_cast<float>(Def.numberAt(AtPath.c_str(), 0, 0.0)), static_cast<float>(Def.numberAt(AtPath.c_str(), 1, 0.0)),
			static_cast<float>(Def.numberAt(AtPath.c_str(), 2, 0.0))) * 100.0f;
		const std::string RotPath = Field("rot");
		Part.Rotation = FRotator3f(static_cast<float>(Def.numberAt(RotPath.c_str(), 0, 0.0)), static_cast<float>(Def.numberAt(RotPath.c_str(), 1, 0.0)),
			static_cast<float>(Def.numberAt(RotPath.c_str(), 2, 0.0)));
		Part.Color = ReadColor(Def, Field("color"), FLinearColor(0.5f, 0.5f, 0.5f)) * Tint;
		Part.Color.A = 1.0f;
		Part.bStuff = Def.flag(Field("stuff").c_str(), false);
		Part.Slide = static_cast<float>(Def.number(Field("slide").c_str(), 0.0)) * 100.0f;

		const std::string CornersPath = Field("corners");
		if (Def.count(CornersPath.c_str()) >= 2)
		{
			const float InsetX = static_cast<float>(Def.numberAt(CornersPath.c_str(), 0, 0.0)) * 100.0f;
			const float InsetY = static_cast<float>(Def.numberAt(CornersPath.c_str(), 1, 0.0)) * 100.0f;
			for (int32 Sx = -1; Sx <= 1; Sx += 2)
			{
				for (int32 Sy = -1; Sy <= 1; Sy += 2)
				{
					FPart Corner = Part;
					Corner.At.X = static_cast<float>(Sx) * (FootprintCm.X * 0.5f - InsetX);
					Corner.At.Y = static_cast<float>(Sy) * (FootprintCm.Y * 0.5f - InsetY);
					Out.Add(Corner);
				}
			}
		}
		else
		{
			Out.Add(Part);
		}
	}
}

float VisualMesh::HeightOf(const TArray<FPart>& Parts)
{
	float Top = 0.0f;
	for (const FPart& Part : Parts)
	{
		Top = FMath::Max(Top, Part.At.Z + Part.Size.Z);
	}
	return Top;
}

UStaticMesh* VisualMesh::MakeMesh(UObject* Outer, UMaterialInterface* Material, const TArray<FPart>& Parts, EPartSet Set, const FLinearColor* StuffColor, bool bBakeLight)
{
	TArray<FVisTriangle> Triangles;
	for (const FPart& Part : Parts)
	{
		const bool bSliding = Part.Slide > 0.0f;
		if ((Set == EPartSet::Fixed && bSliding) || (Set == EPartSet::Sliding && !bSliding))
		{
			continue;
		}
		FLinearColor Color = Part.Color;
		if (Part.bStuff)
		{
			Color = (StuffColor ? *StuffColor : FLinearColor(0.35f, 0.35f, 0.35f)) * Part.Color;
		}
		Color.A = 1.0f;
		VisAddPart(Triangles, Part, Color);
	}
	if (Triangles.IsEmpty())
	{
		return nullptr;
	}
	float Top = 1.0f;
	for (const FVisTriangle& T : Triangles)
	{
		Top = FMath::Max(Top, FMath::Max3(T.A.Z, T.B.Z, T.C.Z));
	}

	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	const FPolygonGroupID Group = Description.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Visual");
	for (const FVisTriangle& T : Triangles)
	{
		const FVector3f Normal = FVector3f::CrossProduct(T.C - T.A, T.B - T.A).GetSafeNormal();
		const FVector3f Tangent = (T.B - T.A).GetSafeNormal();
		// A little baked light for the plants: brighter facing up and higher up, so a plant doesn't read as flat colour.
		const float Light = bBakeLight ? 0.62f + 0.38f * (Normal.Z * 0.5f + 0.5f) : 1.0f;
		TArray<FVertexInstanceID> Corners;
		for (const FVector3f* P : {&T.A, &T.B, &T.C})
		{
			const FVertexID Vertex = Description.CreateVertex();
			Attributes.GetVertexPositions()[Vertex] = *P;
			const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
			const float Shade = bBakeLight ? Light * (0.9f + 0.2f * (P->Z / Top)) : 1.0f;
			Attributes.GetVertexInstanceNormals()[Instance] = Normal;
			Attributes.GetVertexInstanceTangents()[Instance] = Tangent;
			Attributes.GetVertexInstanceBinormalSigns()[Instance] = 1.0f;
			Attributes.GetVertexInstanceColors()[Instance] = FVector4f(T.Color.R * Shade, T.Color.G * Shade, T.Color.B * Shade, 1.0f);
			Attributes.GetVertexInstanceUVs().Set(Instance, 0, FVector2f(0.0f, 0.0f));
			Corners.Add(Instance);
		}
		Description.CreateTriangle(Group, Corners);
	}

	UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, NAME_None, RF_Transient);
	Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("Visual"), TEXT("Visual")));
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bBuildSimpleCollision = false;
	Params.bFastBuild = true;
	Params.bMarkPackageDirty = false;
	Params.bCommitMeshDescription = false;
	Mesh->BuildFromMeshDescriptions({&Description}, Params);
	return Mesh;
}
