#pragma once

#include "CoreMinimal.h"
#include "ImageCore.h"
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"

// Where the whole body meets the face mesh (the neck and the top of the chest), the two textures must give the same
// colour, or a ring shows. Each skin texture's SourceAverage (M_PawnSkin divides the texture by it and multiplies by
// the pawn's skin colour) is measured there, on the texels the seam's vertices use, not over the whole texture.
// Shared by the PawnLooks (body, Kellan's face) and PawnFaces (the Creator face skins) commandlets.

/** UV channel 0 of the vertices of `Mesh` that are within `Within` cm of any of `Others`. */
inline TArray<FVector2f> PawnSeamUVs(const FMeshDescription& Mesh, const TArray<FVector>& Others, double Within)
{
	const double Cell = FMath::Max(Within, 0.5);
	TMap<FIntVector, TArray<int32>> Cells;
	const auto Key = [Cell](const FVector& P) { return FIntVector(FMath::FloorToInt32(P.X / Cell), FMath::FloorToInt32(P.Y / Cell), FMath::FloorToInt32(P.Z / Cell)); };
	for (int32 i = 0; i < Others.Num(); ++i)
	{
		Cells.FindOrAdd(Key(Others[i])).Add(i);
	}
	FSkeletalMeshConstAttributes Attributes(Mesh);
	const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
	const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TArray<FVector2f> Out;
	for (const FVertexID V : Mesh.Vertices().GetElementIDs())
	{
		const FVector P(Positions[V]);
		const FIntVector C = Key(P);
		bool bNear = false;
		for (int32 x = -1; x <= 1 && !bNear; ++x)
		{
			for (int32 y = -1; y <= 1 && !bNear; ++y)
			{
				for (int32 z = -1; z <= 1 && !bNear; ++z)
				{
					if (const TArray<int32>* Found = Cells.Find(C + FIntVector(x, y, z)))
					{
						for (const int32 i : *Found)
						{
							if (FVector::DistSquared(P, Others[i]) < Within * Within)
							{
								bNear = true;
								break;
							}
						}
					}
				}
			}
		}
		if (bNear)
		{
			for (const FVertexInstanceID I : Mesh.GetVertexVertexInstanceIDs(V))
			{
				Out.Add(UVs.Get(I, 0));
			}
		}
	}
	return Out;
}

/** The average linear colour of `Image` (in its own gamma) around each UV (wrapping, 5 x 5 texels); `Fallback` without any. */
inline FLinearColor PawnAverageAt(const FImage& Image, const TArray<FVector2f>& UVs, const FLinearColor& Fallback)
{
	FImage Linear;
	Image.CopyTo(Linear, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
	const TArrayView64<FLinearColor> Pixels = Linear.AsRGBA32F();
	FLinearColor Sum(0, 0, 0, 0);
	int64 Count = 0;
	for (const FVector2f& UV : UVs)
	{
		const int32 X = FMath::FloorToInt32(FMath::Frac(UV.X) * Linear.SizeX);
		const int32 Y = FMath::FloorToInt32(FMath::Frac(UV.Y) * Linear.SizeY);
		for (int32 dx = -2; dx <= 2; ++dx)
		{
			for (int32 dy = -2; dy <= 2; ++dy)
			{
				const int64 px = (X + dx + Linear.SizeX) % Linear.SizeX;
				const int64 py = (Y + dy + Linear.SizeY) % Linear.SizeY;
				const FLinearColor& P = Pixels[py * Linear.SizeX + px];
				const float Luminance = P.GetLuminance();
				if (Luminance > 0.02f && Luminance < 0.9f)
				{
					Sum += P;
					++Count;
				}
			}
		}
	}
	if (Count < 50)
	{
		return Fallback;
	}
	const FLinearColor Average = Sum / static_cast<float>(Count);
	return FLinearColor(Average.R, Average.G, Average.B, 1.0f);
}
