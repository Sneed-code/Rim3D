#include "GoreStump.h"

#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
#include "ProceduralMeshComponent.h"

namespace
{
	TAutoConsoleVariable<float> GGoreClothLift(TEXT("Gore.ClothLift"), 0.25f,
		TEXT("On a clothed limb, how far (limb radii) the stump's face stands out past the cut, in front of the sleeve or trouser leg's own end, ")
		TEXT("with the soaked cloth hanging back over the gap. Only where the meshes' bulge past the cut could not be measured."), ECVF_Default);

	/** A smooth random wobble round a circle: a few waves with random phases (periodic in Theta). */
	struct FGoreWave
	{
		float Phase[4] = {0, 0, 0, 0};
		float Amp[4] = {0.5f, 0.3f, 0.2f, 0.12f};
		int32 Freq[4] = {2, 3, 5, 9};

		explicit FGoreWave(FRandomStream& Random)
		{
			for (int32 i = 0; i < 4; ++i)
			{
				Phase[i] = Random.FRandRange(0.0f, 2.0f * PI);
				Amp[i] *= Random.FRandRange(0.6f, 1.4f);
			}
		}

		/** About -1 to 1. */
		float At(float Theta) const
		{
			float Sum = 0.0f;
			for (int32 i = 0; i < 4; ++i)
			{
				Sum += Amp[i] * FMath::Sin(Freq[i] * Theta + Phase[i]);
			}
			return Sum;
		}
	};

	/** Vertices, triangles and colours built up in the stump's own frame (Z out of the cut, cm). */
	struct FGoreMesh
	{
		TArray<FVector> Positions;
		TArray<FVector> Normals;
		TArray<FColor> Colors;
		TArray<FVector2D> UVs;
		TArray<int32> Triangles;

		/**
		 * A grid of Rows x Cols points, closed round the columns (a ring). Faces point along cross(row step, column
		 * step), which the callers arrange to be outwards; normals are worked out the same way from the neighbours.
		 * `Wet` 0 is wet and glossy, 1 dry (the colour's alpha, which M_Gore reads).
		 */
		void AddRingGrid(int32 Rows, int32 Cols, TFunctionRef<FVector(int32, int32)> Position, TFunctionRef<FLinearColor(int32, int32)> Color)
		{
			const int32 Base = Positions.Num();
			for (int32 R = 0; R < Rows; ++R)
			{
				for (int32 C = 0; C < Cols; ++C)
				{
					Positions.Add(Position(R, C));
					Colors.Add(Color(R, C).ToFColor(false));
					UVs.Add(FVector2D(static_cast<float>(C) / Cols, static_cast<float>(R) / FMath::Max(Rows - 1, 1)));
				}
			}
			for (int32 R = 0; R < Rows; ++R)
			{
				for (int32 C = 0; C < Cols; ++C)
				{
					const FVector& Up = Positions[Base + FMath::Min(R + 1, Rows - 1) * Cols + C];
					const FVector& Down = Positions[Base + FMath::Max(R - 1, 0) * Cols + C];
					const FVector& Next = Positions[Base + R * Cols + (C + 1) % Cols];
					const FVector& Prev = Positions[Base + R * Cols + (C + Cols - 1) % Cols];
					FVector N = FVector::CrossProduct(Up - Down, Next - Prev);
					if (N.SizeSquared() < 1e-8)
					{
						// The middle of a disc (a ring of radius 0): straight out.
						N = FVector::CrossProduct(Positions[Base + FMath::Min(R + 1, Rows - 1) * Cols + C] - Positions[Base + R * Cols + C],
							Positions[Base + FMath::Min(R + 1, Rows - 1) * Cols + (C + 1) % Cols] - Positions[Base + R * Cols + C]);
					}
					Normals.Add(N.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector));
				}
			}
			for (int32 R = 0; R + 1 < Rows; ++R)
			{
				for (int32 C = 0; C < Cols; ++C)
				{
					const int32 A = Base + R * Cols + C;
					const int32 B = Base + (R + 1) * Cols + C;
					const int32 D = Base + R * Cols + (C + 1) % Cols;
					const int32 E = Base + (R + 1) * Cols + (C + 1) % Cols;
					// Unreal draws a triangle's front where its corners run clockwise as seen: wound the other way, every
					// face showed its back (culled or black) and the meat was never seen, only the skirt's inside.
					Triangles.Append({A, D, B, B, D, E});
				}
			}
		}
	};

	FLinearColor GoreWet(const FLinearColor& Color, float Wet)
	{
		return FLinearColor(Color.R, Color.G, Color.B, Wet);
	}

	// The tissue colours (linear).
	const FLinearColor GoreMuscleDark(0.12f, 0.007f, 0.005f);
	const FLinearColor GoreMuscleLight(0.32f, 0.026f, 0.018f);
	const FLinearColor GoreFat(0.86f, 0.62f, 0.26f);
	const FLinearColor GoreCloth(0.50f, 0.16f, 0.11f);
	const FLinearColor GoreDermis(0.40f, 0.07f, 0.05f);
	const FLinearColor GoreBlood(0.18f, 0.008f, 0.006f);
	const FLinearColor GoreBone(0.90f, 0.86f, 0.74f);
	const FLinearColor GoreMarrow(0.30f, 0.06f, 0.035f);
	const FLinearColor GoreCord(0.82f, 0.76f, 0.66f);
	const FLinearColor GoreCartilage(0.80f, 0.62f, 0.56f);
	const FLinearColor GoreHole(0.03f, 0.006f, 0.005f);

	/** A broken bone standing out of the cut: its jagged wall, the ring of hard bone on top and the marrow inside. */
	void GoreAddBone(FGoreMesh& Mesh, FRandomStream& Random, const FVector2D& Centre, float Radius, float Base, float Top, float Jag)
	{
		constexpr int32 Cols = 14;
		const FGoreWave Wave(Random);
		const float Marrow = Radius * 0.55f;
		TArray<float> Tops;
		for (int32 C = 0; C < Cols; ++C)
		{
			const float Theta = 2.0f * PI * C / Cols;
			Tops.Add(Top + Jag * (Wave.At(Theta) + Random.FRandRange(-0.5f, 0.5f)));
		}
		const auto Round = [&](float R, int32 C, float Z)
		{
			const float Theta = 2.0f * PI * C / Cols;
			return FVector(Centre.X + R * FMath::Cos(Theta), Centre.Y + R * FMath::Sin(Theta), Z);
		};
		// The wall, top to bottom (faces outward), streaked with blood lower down.
		Mesh.AddRingGrid(3, Cols,
			[&](int32 R, int32 C) { return Round(Radius, C, FMath::Lerp(Tops[C], Base, R * 0.5f)); },
			[&](int32 R, int32 C) { return GoreWet(FMath::Lerp(GoreBone, GoreBlood, R * 0.45f + 0.1f * Random.FRand()), R == 0 ? 0.75f : 0.2f); });
		// The ring of hard bone across the top, marrow edge out to the wall.
		Mesh.AddRingGrid(2, Cols,
			[&](int32 R, int32 C) { return Round(R == 0 ? Marrow : Radius, C, Tops[C] - (R == 0 ? 0.08f * Radius : 0.0f)); },
			[&](int32 R, int32 C) { return GoreWet(FMath::Lerp(GoreBone, GoreMarrow, R == 0 ? 0.25f : 0.0f), 0.85f); });
		// The marrow, sunk a little.
		Mesh.AddRingGrid(2, Cols,
			[&](int32 R, int32 C) { return Round(R == 0 ? 0.0f : Marrow, C, (R == 0 ? Top - 0.35f * Radius : Tops[C] - 0.08f * Radius)); },
			[&](int32 R, int32 C) { return GoreWet(GoreMarrow * (0.8f + 0.4f * Random.FRand()), 0.05f); });
	}

	/** The windpipe at the neck: a pale ring of cartilage round a dark hole. */
	void GoreAddWindpipe(FGoreMesh& Mesh, const FVector2D& Centre, float Outer, float Inner, float Base, float Top)
	{
		constexpr int32 Cols = 14;
		const auto Round = [&](float R, int32 C, float Z)
		{
			const float Theta = 2.0f * PI * C / Cols;
			return FVector(Centre.X + R * FMath::Cos(Theta), Centre.Y + R * FMath::Sin(Theta), Z);
		};
		Mesh.AddRingGrid(2, Cols, [&](int32 R, int32 C) { return Round(Outer, C, R == 0 ? Top : Base); },
			[&](int32 R, int32 C) { return GoreWet(GoreCartilage, 0.3f); });
		Mesh.AddRingGrid(2, Cols, [&](int32 R, int32 C) { return Round(R == 0 ? Inner : Outer, C, Top); },
			[&](int32 R, int32 C) { return GoreWet(GoreCartilage, 0.3f); });
		// Inside, bottom to top (faces inward), going dark.
		Mesh.AddRingGrid(2, Cols, [&](int32 R, int32 C) { return Round(Inner, C, R == 0 ? Base - 1.5f : Top); },
			[&](int32 R, int32 C) { return GoreWet(R == 0 ? GoreHole : GoreCartilage * 0.6f, 0.1f); });
		Mesh.AddRingGrid(2, Cols, [&](int32 R, int32 C) { return Round(R == 0 ? 0.0f : Inner, C, Base - 1.5f); },
			[&](int32 R, int32 C) { return GoreWet(GoreHole, 0.1f); });
	}
}

UProceduralMeshComponent* GoreMakeStump(AActor* Owner, USceneComponent* Parent, FName Socket, const FVector& At, const FVector& Outward,
	const FVector& Front, float RadiusCm, EGoreStumpKind Kind, const FLinearColor& Skin, int32 Seed, float ClothOuter, float DomeCm)
{
	const bool bClothed = ClothOuter > 1.01f;
	if (!Owner || !Parent || RadiusCm <= 0.0f)
	{
		return nullptr;
	}
	FRandomStream Random(Seed);
	const float R = RadiusCm;
	const FGoreWave Bulge(Random);
	const FGoreWave Edge(Random);
	const FGoreWave Fibres(Random);
	FGoreMesh Mesh;

	// The cut face: rings out from the middle (fractions of the radius), then the torn rim and the skin hanging back.
	static const float Rings[] = {0.0f, 0.14f, 0.28f, 0.42f, 0.55f, 0.66f, 0.74f, 0.81f, 0.85f, 0.89f, 0.92f, 0.95f, 0.975f, 1.0f, 1.02f, 1.04f, 1.05f};
	constexpr int32 RingCount = UE_ARRAY_COUNT(Rings);
	constexpr int32 Cols = 40;
	const float Skirt = 0.12f * R + 0.6f;
	// Clothed: a sleeve or trouser leg is thicker than the skin and ends a little past the cut in a closed, rounded end of
	// its own, which hid the meat. The face stands in front of it (and is as wide as the cloth), the soaked cloth hanging
	// back over the gap.
	const float Lift = DomeCm >= 0.0f ? FMath::Min(DomeCm, R) : (bClothed ? FMath::Max(GGoreClothLift.GetValueOnGameThread(), 0.0f) * R : 0.0f);
	// Height of the face (cm) at a fraction of the radius: the muscle bulges out unevenly, the fat lies lower, the skin
	// curls up in a lip at the edge and then hangs back over the limb.
	const auto FaceZ = [&](float F, float Theta) -> float
	{
		if (F < 0.70f)
		{
			return 0.13f * R * (1.0f - F * F) * (0.65f + 0.35f * Bulge.At(Theta * 1.3f + F * 2.0f)) + 0.06f * R * Fibres.At(Theta * 2.0f + F * 7.0f) * F;
		}
		if (F < 0.89f)
		{
			// The fat stands a little proud of the muscle's edge, so it catches the light instead of lying in shadow.
			return 0.08f * R * (0.85f + 0.15f * Edge.At(Theta * 3.0f));
		}
		if (F < 1.0f)
		{
			return 0.04f * R + 0.03f * R * FMath::Sin((F - 0.91f) / 0.09f * PI) * (0.7f + 0.3f * Edge.At(Theta));
		}
		return bClothed ? 0.02f * R : FMath::Lerp(0.0f, -0.06f * R, (F - 1.0f) / 0.06f);
	};
	const FLinearColor SkinBloodied = FMath::Lerp(Skin, GoreBlood, 0.4f);
	Mesh.AddRingGrid(RingCount + 1, Cols,
		[&](int32 Row, int32 C)
		{
			const float Theta = 2.0f * PI * C / Cols;
			const bool bSkirt = Row == RingCount;
			const float F = Rings[FMath::Min(Row, RingCount - 1)];
			// The edge is torn, not round: the outer rings wander in and out.
			const float Torn = F >= 0.94f ? 1.0f + 0.05f * Edge.At(Theta) + (bSkirt ? 0.03f * Random.FRandRange(-1.0f, 1.0f) : 0.0f) : 1.0f;
			// Clothed, the outer rings are torn, soaked cloth reaching out to the sleeve's own measured width (ClothOuter),
			// covering the white inside of the open sleeve round the meat.
			const float Spread = bClothed && F > 1.0f ? 1.0f + (F - 1.0f) / 0.05f * (ClothOuter - 1.0f) : F;
			// The skirt hangs back onto the limb: bare skin just inside its own edge, soaked cloth folded back down onto
			// the sleeve rather than standing off it as a wide hollow collar.
			// Tucked in a little and short, so it reads as an edge curling back onto the limb, never a hoop standing off a
			// thinner sleeve.
			const float SkirtSpread = bClothed ? ClothOuter * 0.97f : 0.88f;
			const float Radius = R * (bSkirt ? SkirtSpread : Spread) * Torn;
			const float Z = bSkirt ? -(Skirt + Lift) * (0.75f + 0.25f * Edge.At(Theta * 1.7f + 1.0f)) : FaceZ(F, Theta) + (F < 0.7f ? 0.04f * R * Random.FRandRange(-0.5f, 0.5f) : 0.0f);
			return FVector(Radius * FMath::Cos(Theta), Radius * FMath::Sin(Theta), Z);
		},
		[&](int32 Row, int32 C)
		{
			// Rough, wet and smeared rather than clean rings ("cartoon salami" on the PC): the layer edges wander a lot,
			// and blood lies in patches across everything.
			const FLinearColor Tissue = [&]() -> FLinearColor
			{
				const float Theta = 2.0f * PI * C / Cols;
				const float F = Rings[FMath::Min(Row, RingCount - 1)];
				const float Wobble = 0.07f * Edge.At(Theta * 2.0f) + 0.04f * Fibres.At(Theta * 5.0f + F * 3.0f) + 0.03f * Random.FRandRange(-1.0f, 1.0f);
				const float G = F + Wobble;
				if (bClothed && (Row == RingCount || F > 1.0f))
				{
					return GoreWet(FMath::Lerp(GoreCloth, GoreBlood, 0.1f + 0.3f * Random.FRand()), 0.45f);
				}
				if (Row == RingCount)
				{
					return GoreWet(FMath::Lerp(Skin, GoreBlood, 0.35f + 0.3f * Random.FRand()), 0.35f);
				}
				if (G < 0.71f)
				{
					// Muscle in bundles: darker and lighter streaks across the cut, wet.
					const float Streak = 0.5f + 0.5f * Fibres.At(Theta * 3.0f + F * 11.0f);
					return GoreWet(FMath::Lerp(GoreMuscleDark, GoreMuscleLight, Streak * 0.8f + 0.2f * Random.FRand()), 0.0f);
				}
				if (G < 0.89f)
				{
					return GoreWet(FMath::Lerp(GoreFat, GoreBlood, 0.1f + 0.25f * Random.FRand()), 0.15f);
				}
				if (G < 0.975f)
				{
					return GoreWet(GoreDermis, 0.05f);
				}
				return GoreWet(FMath::Lerp(SkinBloodied, GoreBlood, 0.3f + 0.4f * Random.FRand()), 0.3f);
			}();
			if (Random.FRand() < 0.3f)
			{
				// A smear of blood: darker, and wet.
				return GoreWet(FMath::Lerp(Tissue, GoreBlood, Random.FRandRange(0.3f, 0.65f)), 0.0f);
			}
			return Tissue;
		});

	// Clothed: a sleeve or trouser leg often runs on past the cut as an open tube. A ragged, blood-soaked lining just
	// inside it, facing in, so looking down the tube shows wet red cloth instead of a hollow white sleeve.
	if (bClothed)
	{
		const float Lining = 0.45f * R;
		Mesh.AddRingGrid(2, Cols,
			[&](int32 Row, int32 C)
			{
				const float Theta = 2.0f * PI * C / Cols;
				const float Rr = R * ClothOuter * (0.99f + 0.02f * Edge.At(Theta));
				const float Z = Row == 0 ? FaceZ(1.0f, Theta) - 0.02f * R : Lining * (0.55f + 0.45f * (0.5f + 0.5f * Fibres.At(Theta * 2.3f + 0.7f)));
				return FVector(Rr * FMath::Cos(Theta), Rr * FMath::Sin(Theta), Z);
			},
			[&](int32 Row, int32 C) { return GoreWet(FMath::Lerp(GoreCloth, GoreBlood, Row == 0 ? 0.6f : 0.25f + 0.3f * Random.FRand()), 0.4f); });
	}

	// What stands out of the middle.
	const auto TopOver = [&](const FVector2D& Centre, float Radius)
	{
		// The face's highest point round the bone, so the bone always stands proud of the muscle.
		float Highest = 0.0f;
		for (int32 i = 0; i < 8; ++i)
		{
			const float Theta = 2.0f * PI * i / 8;
			const FVector2D P = Centre + FVector2D(FMath::Cos(Theta), FMath::Sin(Theta)) * Radius;
			Highest = FMath::Max(Highest, FaceZ(static_cast<float>(P.Size()) / R, FMath::Atan2(P.Y, P.X)) + 0.06f * R);
		}
		return Highest;
	};
	switch (Kind)
	{
	case EGoreStumpKind::OneBone:
	{
		const FVector2D Centre(0.05f * R, 0.02f * R);
		const float Radius = 0.32f * R;
		GoreAddBone(Mesh, Random, Centre, Radius, -1.0f, TopOver(Centre, Radius) + 1.2f + 0.3f * R, 0.15f * R);
		break;
	}
	case EGoreStumpKind::TwoBones:
	{
		const FVector2D A(-0.30f * R, 0.06f * R);
		const FVector2D B(0.32f * R, -0.12f * R);
		GoreAddBone(Mesh, Random, A, 0.24f * R, -1.0f, TopOver(A, 0.24f * R) + 1.0f + 0.25f * R, 0.12f * R);
		GoreAddBone(Mesh, Random, B, 0.17f * R, -1.0f, TopOver(B, 0.17f * R) + 0.7f + 0.18f * R, 0.1f * R);
		break;
	}
	case EGoreStumpKind::Neck:
	{
		// +X is the front: the windpipe there, the spine behind with the cord at its back.
		const FVector2D Spine(-0.36f * R, 0.0f);
		GoreAddBone(Mesh, Random, Spine, 0.27f * R, -1.0f, TopOver(Spine, 0.27f * R) + 0.3f, 0.06f * R);
		const FVector2D Cord(-0.66f * R, 0.0f);
		Mesh.AddRingGrid(2, 10,
			[&](int32 Row, int32 C)
			{
				const float Theta = 2.0f * PI * C / 10;
				const float Rr = Row == 0 ? 0.0f : 0.09f * R;
				return FVector(Cord.X + Rr * FMath::Cos(Theta), Cord.Y + Rr * FMath::Sin(Theta), TopOver(Cord, 0.09f * R) + (Row == 0 ? 0.25f : 0.1f));
			},
			[&](int32 Row, int32 C) { return GoreWet(GoreCord, 0.2f); });
		const FVector2D Pipe(0.46f * R, 0.0f);
		GoreAddWindpipe(Mesh, Pipe, 0.2f * R, 0.13f * R, -1.0f, TopOver(Pipe, 0.2f * R) + 0.35f);
		break;
	}
	}

	UProceduralMeshComponent* Stump = NewObject<UProceduralMeshComponent>(Owner);
	Stump->bUseAsyncCooking = false;
	Stump->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Stump->SetCanEverAffectNavigation(false);
	Stump->bReceivesDecals = false;
	Stump->SetCastShadow(true);
	Stump->CreateMeshSection(0, Mesh.Positions, Mesh.Triangles, Mesh.Normals, Mesh.UVs, Mesh.Colors, TArray<FProcMeshTangent>(), false);
	UMaterialInterface* Flesh = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Rim3D/Generated/Blood/M_Gore.M_Gore"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Flesh)
	{
		Flesh = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Rim3D/Generated/Blood/M_BloodDrop.M_BloodDrop"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (Flesh)
	{
		Stump->SetMaterial(0, Flesh);
	}
	Stump->SetupAttachment(Parent, Socket);
	Stump->RegisterComponent();
	const FVector Out = Outward.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	FVector Ahead = Front - Out * FVector::DotProduct(Front, Out);
	if (Ahead.SizeSquared() < 1e-4)
	{
		Ahead = FVector::CrossProduct(Out, FMath::Abs(Out.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector);
	}
	Stump->SetWorldLocationAndRotation(At + Out * Lift, FRotationMatrix::MakeFromZX(Out, Ahead).ToQuat());
	Stump->SetWorldScale3D(FVector::OneVector);
	return Stump;
}
