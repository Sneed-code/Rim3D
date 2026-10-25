#include "TerrainSubsystem.h"

#include "Async/ParallelFor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "ProceduralMeshComponent.h"
#include "StaticMeshAttributes.h"

#include "OverseerSubsystem.h"
#include "SimSubsystem.h"
#include "TerrainDraw.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

#include <string>

DEFINE_LOG_CATEGORY_STATIC(LogSimTerrain, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GTerrainEnable(
		TEXT("Terrain.Enable"), 1, TEXT("1 draws the sim's ground and plants, 0 draws nothing."), ECVF_Default);
	TAutoConsoleVariable<int32> GTerrainRoofs(
		TEXT("Terrain.Roofs"), -1, TEXT("Roofs: -1 hidden in the overseer view and shown otherwise, 0 hidden, 1 shown."), ECVF_Default);
	TAutoConsoleVariable<int32> GTerrainPlantScan(
		TEXT("Terrain.PlantScan"), 24, TEXT("How many chunks a frame are checked for plants that grew, died or were built over."), ECVF_Default);

	// Flat vertex colours until the terrain materials (A2) come.
	const TCHAR* const TerrMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");

	// A plant is drawn at one of this many sizes between a sprout and fully grown; a chunk's plants
	// are rebuilt only when one of them steps to another size.
	constexpr int32 TerrGrowthStages = 6;
	constexpr float TerrSproutScale = 0.2f;
	// Chunks made at a time when building everything (their mesh data is made on all cores).
	constexpr int32 TerrBuildBatch = 64;

	const FColor TerrWaterColor(40, 90, 170);
	const FColor TerrSandColor(210, 195, 140);
	const FColor TerrRoughStoneColor(150, 140, 125);
	const FColor TerrRockTopColor(122, 117, 112);
	const FColor TerrRockSideColor(92, 88, 84);
	const FColor TerrThickRoofUnderColor(62, 60, 58);
	const FColor TerrThinRoofColor(132, 104, 78);
	const FColor TerrThinRoofUnderColor(92, 78, 64);

	uint64 TerrHash(uint64 Value)
	{
		Value += 0x9E3779B97F4A7C15ULL;
		Value = (Value ^ (Value >> 30)) * 0xBF58476D1CE4E5B9ULL;
		Value = (Value ^ (Value >> 27)) * 0x94D049BB133111EBULL;
		return Value ^ (Value >> 31);
	}

	FColor TerrShade(const FColor& Color, int32 Delta)
	{
		return FColor(
			static_cast<uint8>(FMath::Clamp(static_cast<int32>(Color.R) + Delta, 0, 255)),
			static_cast<uint8>(FMath::Clamp(static_cast<int32>(Color.G) + Delta, 0, 255)),
			static_cast<uint8>(FMath::Clamp(static_cast<int32>(Color.B) + Delta, 0, 255)));
	}

	/** The colour of a cell's ground. Soil follows the fertility the plants use: gravel, soil, rich soil. */
	FColor TerrCellColor(const colony::Map& Map, const colony::Plants& Plants, int32 X, int32 Y)
	{
		const colony::CellPos Cell{X, Y, 0};
		switch (Map.terrain(Cell))
		{
			case colony::Terrain::Water: return TerrWaterColor;
			case colony::Terrain::Sand: return TerrSandColor;
			case colony::Terrain::RoughStone: return TerrRoughStoneColor;
			case colony::Terrain::Rock: return TerrRockTopColor;
			case colony::Terrain::Soil: break;
		}
		const float Fertility = Plants.fertility(Cell);
		const FLinearColor Gravel(120.0f, 122.0f, 84.0f);
		const FLinearColor Soil(90.0f, 130.0f, 60.0f);
		const FLinearColor Rich(66.0f, 104.0f, 48.0f);
		const FLinearColor Mix = Fertility < 1.0f
			? FMath::Lerp(Gravel, Soil, FMath::Clamp((Fertility - 0.7f) / 0.3f, 0.0f, 1.0f))
			: FMath::Lerp(Soil, Rich, FMath::Clamp((Fertility - 1.0f) / 0.4f, 0.0f, 1.0f));
		return FColor(static_cast<uint8>(Mix.R), static_cast<uint8>(Mix.G), static_cast<uint8>(Mix.B));
	}

	/** Triangles with their own vertices (flat shaded), for a procedural mesh section. */
	struct FTerrMesh
	{
		TArray<FVector> Positions;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FColor> Colors;
		TArray<int32> Triangles;

		int32 Num() const { return Positions.Num(); }

		int32 AddVertex(const FVector& Position, const FVector& Normal, const FColor& Color)
		{
			UVs.Add(FVector2D(Position.X * 0.01, Position.Y * 0.01));
			Normals.Add(Normal);
			// The colours here are the sRGB ones you'd pick in a paint program; the material reads vertex colours as linear.
			Colors.Add(FLinearColor::FromSRGBColor(Color).ToFColor(false));
			return Positions.Add(Position);
		}

		/** A triangle whose front faces the `Facing` side, whichever way round the corners were given. */
		void AddFlatTriangle(const FVector& A, FVector B, FVector C, const FVector& Facing, const FColor& Color)
		{
			FVector Normal = FVector::CrossProduct(C - A, B - A);
			if (Normal.IsNearlyZero(1.0e-6))
			{
				return;
			}
			if (FVector::DotProduct(Normal, Facing) < 0.0)
			{
				Swap(B, C);
				Normal = -Normal;
			}
			Normal.Normalize();
			const int32 First = AddVertex(A, Normal, Color);
			AddVertex(B, Normal, Color);
			AddVertex(C, Normal, Color);
			Triangles.Append({First, First + 1, First + 2});
		}

		/** A quad given around its edge. */
		void AddFlatQuad(const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FVector& Facing, const FColor& Color)
		{
			AddFlatTriangle(A, B, C, Facing, Color);
			AddFlatTriangle(A, C, D, Facing, Color);
		}
	};

	struct FTerrChunkBuild
	{
		FTerrMesh Ground;
		FTerrMesh Roof;
		TArray<TArray<FTransform>> Plants;
		uint64 PlantSignature = 0;
	};

	int32 TerrChunksFor(int32 Cells)
	{
		return (Cells + TerrainDraw::ChunkCells - 1) / TerrainDraw::ChunkCells;
	}

	/** The ground of a chunk (section 0) and its roofs (section 1), as vertices in cm from the map's corner. */
	void TerrBuildChunkMeshes(const colony::Map& Map, const colony::Plants& Plants, const TArray<uint8>& ThinRoof,
		int32 ChunkX, int32 ChunkY, FTerrMesh& Ground, FTerrMesh& Roof)
	{
		const int32 MapW = Map.width();
		const int32 MapH = Map.height();
		const int32 X0 = ChunkX * TerrainDraw::ChunkCells;
		const int32 Y0 = ChunkY * TerrainDraw::ChunkCells;
		const int32 X1 = FMath::Min(X0 + TerrainDraw::ChunkCells, MapW);
		const int32 Y1 = FMath::Min(Y0 + TerrainDraw::ChunkCells, MapH);
		const int32 Nx = X1 - X0;
		const int32 Ny = Y1 - Y0;
		const double RockCm = TerrainDraw::RockHeightM * 100.0;
		const double RoofCm = TerrainDraw::RoofHeightM * 100.0;

		// Ground height of a cell corner in cm (the map's last corner reuses the last cell's).
		const auto Height = [&Map, MapW, MapH](int32 X, int32 Y)
		{
			return static_cast<double>(Map.groundHeight(FMath::Clamp(X, 0, MapW - 1), FMath::Clamp(Y, 0, MapH - 1))) * 100.0;
		};
		const auto Corner = [&Height](int32 X, int32 Y, double Up)
		{
			return FVector(X * 100.0, Y * 100.0, Height(X, Y) + Up);
		};
		const auto IsRock = [&Map](int32 X, int32 Y)
		{
			return Map.terrain(X, Y) == colony::Terrain::Rock;
		};

		// The ground: one shared vertex per cell corner, coloured by the cells around it. Rock cells get no ground.
		const int32 Side = Nx + 1;
		Ground.Positions.Reserve(Side * (Ny + 1));
		for (int32 VY = 0; VY <= Ny; ++VY)
		{
			for (int32 VX = 0; VX <= Nx; ++VX)
			{
				const int32 MX = X0 + VX;
				const int32 MY = Y0 + VY;
				int32 R = 0, G = 0, B = 0, Count = 0;
				for (int32 DY = -1; DY <= 0; ++DY)
				{
					for (int32 DX = -1; DX <= 0; ++DX)
					{
						const int32 CX = MX + DX;
						const int32 CY = MY + DY;
						if (CX >= 0 && CY >= 0 && CX < MapW && CY < MapH && !IsRock(CX, CY))
						{
							const FColor Color = TerrCellColor(Map, Plants, CX, CY);
							R += Color.R;
							G += Color.G;
							B += Color.B;
							++Count;
						}
					}
				}
				const FColor Color = Count ? FColor(static_cast<uint8>(R / Count), static_cast<uint8>(G / Count), static_cast<uint8>(B / Count)) : TerrRockTopColor;
				const FVector Normal = FVector(Height(MX - 1, MY) - Height(MX + 1, MY), Height(MX, MY - 1) - Height(MX, MY + 1), 200.0).GetSafeNormal();
				Ground.AddVertex(Corner(MX, MY, 0.0), Normal, Color);
			}
		}
		for (int32 Y = 0; Y < Ny; ++Y)
		{
			for (int32 X = 0; X < Nx; ++X)
			{
				if (!IsRock(X0 + X, Y0 + Y))
				{
					const int32 I = Y * Side + X;
					Ground.Triangles.Append({I, I + Side, I + 1, I + 1, I + Side, I + Side + 1});
				}
			}
		}

		// Rock: a block on the ground of each rock cell, with sides where the neighbour isn't rock.
		const FVector Up(0.0, 0.0, 1.0);
		for (int32 Y = Y0; Y < Y1; ++Y)
		{
			for (int32 X = X0; X < X1; ++X)
			{
				if (!IsRock(X, Y))
				{
					continue;
				}
				const int32 Variation = static_cast<int32>(TerrHash(static_cast<uint64>(Y) * 65536ULL + static_cast<uint64>(X)) % 15ULL) - 7;
				const FVector Low00 = Corner(X, Y, 0.0);
				const FVector Low01 = Corner(X, Y + 1, 0.0);
				const FVector Low10 = Corner(X + 1, Y, 0.0);
				const FVector Low11 = Corner(X + 1, Y + 1, 0.0);
				const FVector Top00 = Low00 + FVector(0.0, 0.0, RockCm);
				const FVector Top01 = Low01 + FVector(0.0, 0.0, RockCm);
				const FVector Top10 = Low10 + FVector(0.0, 0.0, RockCm);
				const FVector Top11 = Low11 + FVector(0.0, 0.0, RockCm);
				Ground.AddFlatQuad(Top00, Top01, Top11, Top10, Up, TerrShade(TerrRockTopColor, Variation));
				const FColor SideColor = TerrShade(TerrRockSideColor, Variation);
				const auto Face = [&](int32 NX, int32 NY, const FVector& LowA, const FVector& LowB, const FVector& TopB, const FVector& TopA, const FVector& Out)
				{
					if (NX >= 0 && NY >= 0 && NX < MapW && NY < MapH && !IsRock(NX, NY))
					{
						Ground.AddFlatQuad(LowA, LowB, TopB, TopA, Out, SideColor);
					}
				};
				Face(X - 1, Y, Low00, Low01, Top01, Top00, FVector(-1.0, 0.0, 0.0));
				Face(X + 1, Y, Low10, Low11, Top11, Top10, FVector(1.0, 0.0, 0.0));
				Face(X, Y - 1, Low00, Low10, Top10, Top00, FVector(0.0, -1.0, 0.0));
				Face(X, Y + 1, Low01, Low11, Top11, Top01, FVector(0.0, 1.0, 0.0));
			}
		}

		// Roofs: the mountain's thick roof over mined-out rock (level with the rock around it) and the
		// thin roof over built rooms. Each is a slab seen from above and from below.
		const FVector Down(0.0, 0.0, -1.0);
		for (int32 Y = Y0; Y < Y1; ++Y)
		{
			for (int32 X = X0; X < X1; ++X)
			{
				const colony::CellPos Cell{X, Y, 0};
				const bool bThick = Map.roof(Cell) == colony::Roof::Thick && !IsRock(X, Y);
				const bool bThin = !bThick && ThinRoof[Map.index(Cell)] != 0;
				if (!bThick && !bThin)
				{
					continue;
				}
				const double Rise = bThick ? RockCm : RoofCm;
				const FVector A = Corner(X, Y, Rise);
				const FVector B = Corner(X, Y + 1, Rise);
				const FVector C = Corner(X + 1, Y + 1, Rise);
				const FVector D = Corner(X + 1, Y, Rise);
				Roof.AddFlatQuad(A, B, C, D, Up, bThick ? TerrRockTopColor : TerrThinRoofColor);
				// The engine's vertex colour material draws both sides, so the underside sits a hand below the top.
				const FVector Thickness(0.0, 0.0, -4.0);
				Roof.AddFlatQuad(A + Thickness, B + Thickness, C + Thickness, D + Thickness, Down, bThick ? TerrThickRoofUnderColor : TerrThinRoofUnderColor);
			}
		}
	}

	// ----- Plant meshes made from a def's `visual` -----

	struct FTerrPart
	{
		bool bValid = false;
		int32 Shape = 1;  // 0 cylinder, 1 cone, 2 sphere
		float Radius = 0.3f;
		float Height = 0.5f;
		float Lift = 0.0f;
		FLinearColor Color = FLinearColor(0.25f, 0.45f, 0.15f);
	};

	FTerrPart TerrReadPart(const colony::Def& Def, const std::string& Path)
	{
		FTerrPart Part;
		if (!Def.has(Path.c_str()))
		{
			return Part;
		}
		Part.bValid = true;
		const std::string Shape = Def.text((Path + ".shape").c_str(), "cone");
		Part.Shape = Shape == "cylinder" ? 0 : (Shape == "sphere" ? 2 : 1);
		Part.Radius = static_cast<float>(Def.number((Path + ".radius").c_str(), 0.3));
		Part.Height = static_cast<float>(Def.number((Path + ".height").c_str(), 0.5));
		Part.Lift = static_cast<float>(Def.number((Path + ".lift").c_str(), 0.0));
		const std::string ColorPath = Path + ".color";
		if (Def.count(ColorPath.c_str()) >= 3)
		{
			// Written as the sRGB colour you'd pick in a paint program; the vertex colour is linear.
			const auto Linear = [&Def, &ColorPath](size_t I, double Fallback) { return FMath::Pow(static_cast<float>(Def.numberAt(ColorPath.c_str(), I, Fallback)), 2.2f); };
			Part.Color = FLinearColor(Linear(0, 0.3), Linear(1, 0.5), Linear(2, 0.2));
		}
		return Part;
	}

	struct FTerrTriangle
	{
		FVector3f A, B, C;
		FLinearColor Color;
	};

	void TerrAddTriangle(TArray<FTerrTriangle>& Out, const FVector3f& A, FVector3f B, FVector3f C, const FVector3f& Outward, const FLinearColor& Color)
	{
		if (FVector3f::DotProduct(FVector3f::CrossProduct(C - A, B - A), Outward) < 0.0f)
		{
			Swap(B, C);
		}
		Out.Add({A, B, C, Color});
	}

	/** A solid of revolution about the Z axis from rings of (height, radius) in cm, bottom to top; a ring of radius 0 is a point. */
	void TerrLathe(TArray<FTerrTriangle>& Out, const TArray<FVector2f>& Rings, int32 Sides, const FLinearColor& Color, bool bBottomCap, bool bTopCap)
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
					TerrAddTriangle(Out, P00, P01, P11, Radial(P00, P01, P11), Color);
					TerrAddTriangle(Out, P00, P11, P10, Radial(P00, P11, P10), Color);
				}
				else if (Rings[R].Y <= 0.0f)
				{
					TerrAddTriangle(Out, P00, P10, P11, Radial(P00, P10, P11), Color);
				}
				else
				{
					TerrAddTriangle(Out, P00, P01, P10, Radial(P00, P01, P10), Color);
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
				TerrAddTriangle(Out, Centre, Ring(Index, S), Ring(Index, S + 1), FVector3f(0.0f, 0.0f, Direction), Color);
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

	void TerrAddPart(TArray<FTerrTriangle>& Out, const FTerrPart& Part)
	{
		if (!Part.bValid)
		{
			return;
		}
		const float Radius = Part.Radius * 100.0f;
		const float Height = Part.Height * 100.0f;
		const float Bottom = Part.Lift * 100.0f;
		TArray<FVector2f> Rings;
		const FLinearColor Color = Part.Color;
		const bool bFloating = Part.Lift > 0.02f;
		switch (Part.Shape)
		{
			case 0:
				Rings = {FVector2f(Bottom, Radius), FVector2f(Bottom + Height, Radius)};
				TerrLathe(Out, Rings, 6, Color, bFloating, true);
				break;
			case 1:
				Rings = {FVector2f(Bottom, Radius), FVector2f(Bottom + Height, 0.0f)};
				TerrLathe(Out, Rings, 5, Color, true, false);
				break;
			default:
				for (int32 K = 0; K <= 4; ++K)
				{
					const float Theta = PI * static_cast<float>(K) / 4.0f;
					Rings.Add(FVector2f(Bottom + Height * 0.5f * (1.0f - FMath::Cos(Theta)), K == 0 || K == 4 ? 0.0f : Radius * FMath::Sin(Theta)));
				}
				TerrLathe(Out, Rings, 7, Color, false, false);
				break;
		}
	}

	/** A plant mesh with its colours baked into the vertices, flat shaded, standing on Z = 0 with its middle at X = Y = 0. */
	UStaticMesh* TerrMakePlantMesh(UObject* Outer, UMaterialInterface* Material, const FTerrPart& Stem, const FTerrPart& Crown)
	{
		TArray<FTerrTriangle> Triangles;
		TerrAddPart(Triangles, Stem);
		TerrAddPart(Triangles, Crown);
		if (Triangles.IsEmpty())
		{
			return nullptr;
		}
		float Top = 1.0f;
		for (const FTerrTriangle& T : Triangles)
		{
			Top = FMath::Max(Top, FMath::Max3(T.A.Z, T.B.Z, T.C.Z));
		}

		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Plant");
		for (const FTerrTriangle& T : Triangles)
		{
			const FVector3f Normal = FVector3f::CrossProduct(T.C - T.A, T.B - T.A).GetSafeNormal();
			const FVector3f Tangent = (T.B - T.A).GetSafeNormal();
			// A little baked light: brighter facing up and higher up, so a plant doesn't read as flat colour.
			const float Light = 0.62f + 0.38f * (Normal.Z * 0.5f + 0.5f);
			TArray<FVertexInstanceID> Corners;
			for (const FVector3f* P : {&T.A, &T.B, &T.C})
			{
				const FVertexID Vertex = Description.CreateVertex();
				Attributes.GetVertexPositions()[Vertex] = *P;
				const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
				const float Shade = Light * (0.9f + 0.2f * (P->Z / Top));
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
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("Plant"), TEXT("Plant")));
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bBuildSimpleCollision = false;
		Params.bFastBuild = true;
		Params.bMarkPackageDirty = false;
		Params.bCommitMeshDescription = false;
		Mesh->BuildFromMeshDescriptions({&Description}, Params);
		return Mesh;
	}
}

bool UTerrainSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UTerrainSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UTerrainSubsystem, STATGROUP_Tickables);
}

void UTerrainSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Sim = Collection.InitializeDependency<USimSubsystem>())
	{
		EventHandle = Sim->OnSimEvent.AddUObject(this, &UTerrainSubsystem::OnSimEvent);
		Bridge = Sim;
	}
}

void UTerrainSubsystem::Deinitialize()
{
	if (USimSubsystem* Sim = Bridge.Get())
	{
		Sim->OnSimEvent.Remove(EventHandle);
	}
	// The world is going away and takes the actor and its components with it.
	TerrainActor = nullptr;
	Chunks.Reset();
	Super::Deinitialize();
}

void UTerrainSubsystem::Teardown()
{
	if (TerrainActor)
	{
		TerrainActor->Destroy();
	}
	TerrainActor = nullptr;
	Chunks.Reset();
	ThinRoof.Reset();
	ThinRoofCells.Reset();
	bBuilt = false;
	BuiltSim = nullptr;
	BuiltGeneration = -1;
	Stats = FTerrainStats();
}

void UTerrainSubsystem::ChunkOfCell(colony::CellPos Cell, int32& OutX, int32& OutY)
{
	OutX = FMath::FloorToInt32(static_cast<float>(Cell.x) / TerrainDraw::ChunkCells);
	OutY = FMath::FloorToInt32(static_cast<float>(Cell.y) / TerrainDraw::ChunkCells);
}

bool UTerrainSubsystem::HasChunk(int32 ChunkX, int32 ChunkY) const
{
	return bBuilt && ChunkX >= 0 && ChunkY >= 0 && ChunkX < ChunksX && ChunkY < ChunksY;
}

int32 UTerrainSubsystem::GetChunkGroundVertices(int32 ChunkX, int32 ChunkY) const
{
	return HasChunk(ChunkX, ChunkY) ? Chunks[ChunkIndex(ChunkX, ChunkY)].GroundVertices : 0;
}

int32 UTerrainSubsystem::GetChunkRoofVertices(int32 ChunkX, int32 ChunkY) const
{
	return HasChunk(ChunkX, ChunkY) ? Chunks[ChunkIndex(ChunkX, ChunkY)].RoofVertices : 0;
}

int32 UTerrainSubsystem::GetChunkPlantInstances(int32 ChunkX, int32 ChunkY) const
{
	return HasChunk(ChunkX, ChunkY) ? Chunks[ChunkIndex(ChunkX, ChunkY)].PlantCount : 0;
}

UProceduralMeshComponent* UTerrainSubsystem::GetChunkMesh(int32 ChunkX, int32 ChunkY) const
{
	return HasChunk(ChunkX, ChunkY) ? Chunks[ChunkIndex(ChunkX, ChunkY)].Mesh : nullptr;
}

void UTerrainSubsystem::MarkCellDirty(colony::CellPos Cell)
{
	if (!bBuilt)
	{
		return;
	}
	// The cell's own chunk, and the chunks of its four neighbours: rock beside a mined cell grows a side there.
	static const int32 Offsets[5][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
	for (const int32(&Offset)[2] : Offsets)
	{
		int32 CX, CY;
		ChunkOfCell(colony::CellPos{Cell.x + Offset[0], Cell.y + Offset[1], 0}, CX, CY);
		if (Cell.x + Offset[0] >= 0 && Cell.y + Offset[1] >= 0 && HasChunk(CX, CY))
		{
			Chunks[ChunkIndex(CX, CY)].bGroundDirty = true;
		}
	}
}

void UTerrainSubsystem::MarkPlantDirty(colony::CellPos Cell)
{
	int32 CX, CY;
	ChunkOfCell(Cell, CX, CY);
	if (Cell.x >= 0 && Cell.y >= 0 && HasChunk(CX, CY))
	{
		Chunks[ChunkIndex(CX, CY)].bPlantsDirty = true;
	}
}

void UTerrainSubsystem::OnSimEvent(const colony::Event& Event)
{
	switch (Event.type)
	{
		case colony::EventType::CellMined: MarkCellDirty(Event.cell); break;
		case colony::EventType::PlantRemoved:
		case colony::EventType::PlantHarvested: MarkPlantDirty(Event.cell); break;
		default: break;
	}
}

void UTerrainSubsystem::LoadPlantVisuals(const colony::Sim& Sim)
{
	PlantVisuals.Reset();
	PlantMeshes.Reset();
	const colony::DefDatabase& Defs = Sim.defs();
	VisualOfDef.Init(-1, static_cast<int32>(Defs.size()));
	if (!PlantMaterial)
	{
		PlantMaterial = LoadObject<UMaterialInterface>(nullptr, TerrMaterialPath);
	}
	for (const colony::DefId Id : Defs.idsOfKind("Things"))
	{
		const colony::Def& Def = Defs.get(Id);
		if (Def.text("category") != "Plant")
		{
			continue;
		}
		FPlantVisual Visual;
		Visual.DefValue = static_cast<int32>(Id.value);
		Visual.CullDistanceCm = static_cast<float>(Def.number("visual.cullDistance", 0.0) * 100.0);
		Visual.Scale = static_cast<float>(Def.number("visual.scale", 1.0));
		Visual.Jitter = static_cast<float>(Def.number("visual.jitter", 0.15));

		// A mesh of the def's own (A1 sets visual.mesh), else one made from its stem and crown shapes.
		const std::string MeshPath = Def.text("visual.mesh");
		if (!MeshPath.empty())
		{
			Visual.Mesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(MeshPath.c_str()));
			if (!Visual.Mesh)
			{
				UE_LOG(LogSimTerrain, Warning, TEXT("%s: mesh %s not found, drawing its shapes instead"), UTF8_TO_TCHAR(Def.defName().c_str()), UTF8_TO_TCHAR(MeshPath.c_str()));
			}
		}
		if (!Visual.Mesh)
		{
			FTerrPart Stem = TerrReadPart(Def, "visual.stem");
			FTerrPart Crown = TerrReadPart(Def, "visual.crown");
			if (!Stem.bValid && !Crown.bValid)
			{
				// A plant with no look yet: a small green cone, so it still shows.
				Crown.bValid = true;
				Crown.Radius = 0.3f;
				Crown.Height = 0.5f;
			}
			Visual.Mesh = TerrMakePlantMesh(this, PlantMaterial, Stem, Crown);
		}
		if (!Visual.Mesh)
		{
			continue;
		}
		PlantMeshes.Add(Visual.Mesh);
		// Anything shorter than a metre and a half casts no shadow worth the cost.
		Visual.bCastShadow = Visual.Mesh->GetBounds().BoxExtent.Z * 2.0f * Visual.Scale >= 150.0f;
		VisualOfDef[static_cast<int32>(Id.value)] = PlantVisuals.Add(Visual);
	}
	Stats.PlantKinds = PlantVisuals.Num();
}

void UTerrainSubsystem::ReadThinRoof(bool& bOutChanged)
{
	bOutChanged = false;
	USimSubsystem* Sim = Bridge.Get();
	if (!Sim || !Sim->GetSim())
	{
		return;
	}
	const colony::Snapshot& Snapshot = Sim->GetCurrentSnapshot();
	if (bBuilt && Snapshot.roofChanges == LastRoofChanges)
	{
		return;
	}
	LastRoofChanges = Snapshot.roofChanges;
	const colony::Map& Map = Sim->GetSim()->map();
	TArray<uint8> Flags;
	Flags.Init(0, Map.width() * Map.height());
	TArray<int32> Cells;
	Cells.Reserve(static_cast<int32>(Snapshot.roofCells.size()));
	for (const colony::CellPos& Cell : Snapshot.roofCells)
	{
		if (Map.inBounds(Cell))
		{
			const int32 Index = Map.index(Cell);
			Flags[Index] = 1;
			Cells.Add(Index);
		}
	}
	if (bBuilt && ThinRoof.Num() == Flags.Num())
	{
		const auto Dirty = [this, &Map](int32 Index)
		{
			int32 CX, CY;
			ChunkOfCell(Map.cellAt(Index), CX, CY);
			if (HasChunk(CX, CY))
			{
				Chunks[ChunkIndex(CX, CY)].bGroundDirty = true;
			}
		};
		for (const int32 Index : Cells)
		{
			if (!ThinRoof[Index])
			{
				Dirty(Index);
			}
		}
		for (const int32 Index : ThinRoofCells)
		{
			if (!Flags[Index])
			{
				Dirty(Index);
			}
		}
	}
	ThinRoof = MoveTemp(Flags);
	ThinRoofCells = MoveTemp(Cells);
	bOutChanged = true;
}

bool UTerrainSubsystem::WantRoofs() const
{
	const int32 Mode = GTerrainRoofs.GetValueOnGameThread();
	if (Mode >= 0)
	{
		return Mode != 0;
	}
	const UOverseerSubsystem* Overseer = GetWorld() ? GetWorld()->GetSubsystem<UOverseerSubsystem>() : nullptr;
	return !(Overseer && Overseer->IsOverseerActive());
}

void UTerrainSubsystem::UpdateRoofVisibility(bool bForce)
{
	const bool bWant = WantRoofs();
	if (!bForce && bWant == bRoofsShown)
	{
		return;
	}
	bRoofsShown = bWant;
	for (FChunk& Chunk : Chunks)
	{
		if (Chunk.Mesh && Chunk.RoofVertices > 0)
		{
			Chunk.Mesh->SetMeshSectionVisible(1, bRoofsShown);
		}
	}
}

void UTerrainSubsystem::CreateChunk(int32 ChunkX, int32 ChunkY)
{
	FChunk& Chunk = Chunks[ChunkIndex(ChunkX, ChunkY)];
	Chunk.Mesh = NewObject<UProceduralMeshComponent>(TerrainActor);
	Chunk.Mesh->bUseAsyncCooking = false;
	Chunk.Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Chunk.Mesh->SetCanEverAffectNavigation(false);
	Chunk.Mesh->SetupAttachment(TerrainActor->GetRootComponent());
	Chunk.Mesh->RegisterComponent();
	Chunk.Plants.Init(nullptr, PlantVisuals.Num());
}

namespace
{
	void TerrApplySection(UProceduralMeshComponent* Mesh, int32 Section, FTerrMesh& Data, UMaterialInterface* Material)
	{
		if (Data.Num() == 0)
		{
			Mesh->ClearMeshSection(Section);
			return;
		}
		Mesh->CreateMeshSection(Section, Data.Positions, Data.Triangles, Data.Normals, Data.UVs, Data.Colors, TArray<FProcMeshTangent>(), false);
		if (Material)
		{
			Mesh->SetMaterial(Section, Material);
		}
	}
}

void UTerrainSubsystem::RebuildChunkGround(int32 ChunkX, int32 ChunkY)
{
	const double Start = FPlatformTime::Seconds();
	const colony::Sim& Sim = *BuiltSim;
	FChunk& Chunk = Chunks[ChunkIndex(ChunkX, ChunkY)];
	FTerrMesh Ground;
	FTerrMesh Roof;
	TerrBuildChunkMeshes(Sim.map(), Sim.plants(), ThinRoof, ChunkX, ChunkY, Ground, Roof);
	Stats.GroundVertices += Ground.Num() - Chunk.GroundVertices;
	Stats.RoofVertices += Roof.Num() - Chunk.RoofVertices;
	Chunk.GroundVertices = Ground.Num();
	Chunk.RoofVertices = Roof.Num();
	TerrApplySection(Chunk.Mesh, 0, Ground, GroundMaterial);
	TerrApplySection(Chunk.Mesh, 1, Roof, GroundMaterial);
	if (Roof.Num() > 0 && !bRoofsShown)
	{
		Chunk.Mesh->SetMeshSectionVisible(1, false);
	}
	Chunk.bGroundDirty = false;
	++Stats.ChunkRebuilds;
	Stats.SlowestChunkMs = FMath::Max(Stats.SlowestChunkMs, (FPlatformTime::Seconds() - Start) * 1000.0);
}

void UTerrainSubsystem::RebuildChunkPlants(int32 ChunkX, int32 ChunkY)
{
	const double Start = FPlatformTime::Seconds();
	FChunk& Chunk = Chunks[ChunkIndex(ChunkX, ChunkY)];
	TArray<TArray<FTransform>> Transforms;
	Transforms.SetNum(PlantVisuals.Num());
	Chunk.PlantSignature = GatherChunkPlants(ChunkX, ChunkY, &Transforms);
	ApplyChunkPlants(Chunk, Transforms);
	Chunk.bPlantsDirty = false;
	++Stats.PlantRebuilds;
	Stats.SlowestChunkMs = FMath::Max(Stats.SlowestChunkMs, (FPlatformTime::Seconds() - Start) * 1000.0);
}

void UTerrainSubsystem::ApplyChunkPlants(FChunk& Chunk, TArray<TArray<FTransform>>& Transforms)
{
	int32 Count = 0;
	for (int32 Kind = 0; Kind < PlantVisuals.Num(); ++Kind)
	{
		const TArray<FTransform>& List = Transforms[Kind];
		UInstancedStaticMeshComponent*& Component = Chunk.Plants[Kind];
		if (List.IsEmpty())
		{
			if (Component)
			{
				Stats.PlantComponents -= Component->GetInstanceCount() > 0 ? 1 : 0;
				Component->ClearInstances();
			}
			continue;
		}
		if (!Component)
		{
			const FPlantVisual& Visual = PlantVisuals[Kind];
			Component = NewObject<UInstancedStaticMeshComponent>(TerrainActor);
			Component->SetStaticMesh(Visual.Mesh);
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetCanEverAffectNavigation(false);
			Component->SetCastShadow(Visual.bCastShadow);
			Component->bAffectDistanceFieldLighting = false;
			if (Visual.CullDistanceCm > 0.0f)
			{
				Component->SetCullDistances(static_cast<int32>(Visual.CullDistanceCm * 0.85f), static_cast<int32>(Visual.CullDistanceCm));
			}
			Component->SetupAttachment(TerrainActor->GetRootComponent());
			Component->RegisterComponent();
		}
		else
		{
			Stats.PlantComponents -= Component->GetInstanceCount() > 0 ? 1 : 0;
			Component->ClearInstances();
		}
		Component->AddInstances(List, false, false, false);
		++Stats.PlantComponents;
		Count += List.Num();
	}
	Stats.PlantInstances += Count - Chunk.PlantCount;
	Chunk.PlantCount = Count;
}

uint64 UTerrainSubsystem::GatherChunkPlants(int32 ChunkX, int32 ChunkY, TArray<TArray<FTransform>>* Out) const
{
	const colony::Sim& Sim = *BuiltSim;
	const colony::Map& Map = Sim.map();
	const colony::Plants& Plants = Sim.plants();
	const int32 X0 = ChunkX * TerrainDraw::ChunkCells;
	const int32 Y0 = ChunkY * TerrainDraw::ChunkCells;
	const int32 X1 = FMath::Min(X0 + TerrainDraw::ChunkCells, Map.width());
	const int32 Y1 = FMath::Min(Y0 + TerrainDraw::ChunkCells, Map.height());
	uint64 Signature = 1469598103934665603ULL;
	for (int32 Y = Y0; Y < Y1; ++Y)
	{
		for (int32 X = X0; X < X1; ++X)
		{
			const colony::CellPos Cell{X, Y, 0};
			if (!Plants.has(Cell))
			{
				continue;
			}
			const uint32 DefValue = Plants.defAt(Cell).value;
			const int32 Kind = DefValue < static_cast<uint32>(VisualOfDef.Num()) ? VisualOfDef[static_cast<int32>(DefValue)] : -1;
			if (Kind < 0)
			{
				continue;
			}
			const uint32 Growth = Plants.growthPpm(Cell);
			const int32 Stage = Growth >= colony::kPlantFullGrowth
				? TerrGrowthStages
				: static_cast<int32>(static_cast<uint64>(Growth) * TerrGrowthStages / colony::kPlantFullGrowth);
			const int32 Index = Map.index(Cell);
			Signature = (Signature ^ ((static_cast<uint64>(Index) << 12) | (static_cast<uint64>(Kind + 1) << 4) | static_cast<uint64>(Stage))) * 1099511628211ULL;
			if (!Out)
			{
				continue;
			}
			// Where in the cell, which way round and how big: a look made from the cell, not from the sim.
			const FPlantVisual& Visual = PlantVisuals[Kind];
			const uint64 H = TerrHash(static_cast<uint64>(Index) + 0x517CC1B7ULL);
			const float R1 = static_cast<float>(H & 0xFFFF) / 65535.0f;
			const float R2 = static_cast<float>((H >> 16) & 0xFFFF) / 65535.0f;
			const float R3 = static_cast<float>((H >> 32) & 0xFFFF) / 65535.0f;
			const float R4 = static_cast<float>((H >> 48) & 0xFFFF) / 65535.0f;
			const float PX = static_cast<float>(X) + 0.5f + (R1 - 0.5f) * 0.7f;
			const float PY = static_cast<float>(Y) + 0.5f + (R2 - 0.5f) * 0.7f;
			const float Grown = TerrSproutScale + (1.0f - TerrSproutScale) * static_cast<float>(Stage) / static_cast<float>(TerrGrowthStages);
			const float Size = Visual.Scale * Grown * (1.0f + (R4 - 0.5f) * 2.0f * Visual.Jitter);
			(*Out)[Kind].Add(FTransform(FRotator(0.0, R3 * 360.0f, 0.0), FVector(PX * 100.0f, PY * 100.0f, Map.heightAt(PX, PY) * 100.0f), FVector(Size)));
		}
	}
	return Signature;
}

void UTerrainSubsystem::ScanPlants()
{
	const int32 Total = Chunks.Num();
	if (Total == 0)
	{
		return;
	}
	const int32 Budget = FMath::Min(FMath::Max(GTerrainPlantScan.GetValueOnGameThread(), 1), Total);
	for (int32 Step = 0; Step < Budget; ++Step)
	{
		const int32 Index = ScanCursor;
		ScanCursor = (ScanCursor + 1) % Total;
		FChunk& Chunk = Chunks[Index];
		if (!Chunk.bPlantsDirty && GatherChunkPlants(Index % ChunksX, Index / ChunksX, nullptr) != Chunk.PlantSignature)
		{
			Chunk.bPlantsDirty = true;
		}
	}
}

void UTerrainSubsystem::FlushDirty()
{
	if (!bBuilt)
	{
		return;
	}
	for (int32 CY = 0; CY < ChunksY; ++CY)
	{
		for (int32 CX = 0; CX < ChunksX; ++CX)
		{
			FChunk& Chunk = Chunks[ChunkIndex(CX, CY)];
			if (Chunk.bGroundDirty)
			{
				RebuildChunkGround(CX, CY);
			}
			if (Chunk.bPlantsDirty)
			{
				RebuildChunkPlants(CX, CY);
			}
		}
	}
}

void UTerrainSubsystem::RebuildAll()
{
	USimSubsystem* Sim = Bridge.Get();
	Teardown();
	if (!Sim || !Sim->GetSim() || !GetWorld())
	{
		return;
	}
	const double Start = FPlatformTime::Seconds();
	const colony::Sim& Core = *Sim->GetSim();
	const colony::Map& Map = Core.map();
	BuiltSim = &Core;
	BuiltGeneration = Sim->GetSimGeneration();
	BuiltOrigin = Sim->GetWorldOrigin();
	ChunksX = TerrChunksFor(Map.width());
	ChunksY = TerrChunksFor(Map.height());

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	TerrainActor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), BuiltOrigin, FRotator::ZeroRotator, Params);
	USceneComponent* Root = NewObject<USceneComponent>(TerrainActor, TEXT("TerrainRoot"));
	TerrainActor->SetRootComponent(Root);
	Root->RegisterComponent();
	Root->SetWorldLocation(BuiltOrigin);
#if WITH_EDITOR
	TerrainActor->SetActorLabel(TEXT("Terrain"));
#endif

	if (!GroundMaterial)
	{
		GroundMaterial = LoadObject<UMaterialInterface>(nullptr, TerrMaterialPath);
	}
	LoadPlantVisuals(Core);
	bool bUnused = false;
	LastRoofChanges = 0;
	bBuilt = false;
	ReadThinRoof(bUnused);
	bRoofsShown = WantRoofs();

	Chunks.Reset();
	Chunks.SetNum(ChunksX * ChunksY);
	bBuilt = true;
	const int32 Total = ChunksX * ChunksY;
	for (int32 First = 0; First < Total; First += TerrBuildBatch)
	{
		const int32 Count = FMath::Min(TerrBuildBatch, Total - First);
		TArray<FTerrChunkBuild> Builds;
		Builds.SetNum(Count);
		ParallelFor(Count, [this, &Builds, &Core, First](int32 I)
		{
			const int32 CX = (First + I) % ChunksX;
			const int32 CY = (First + I) / ChunksX;
			FTerrChunkBuild& Build = Builds[I];
			TerrBuildChunkMeshes(Core.map(), Core.plants(), ThinRoof, CX, CY, Build.Ground, Build.Roof);
			Build.Plants.SetNum(PlantVisuals.Num());
			Build.PlantSignature = GatherChunkPlants(CX, CY, &Build.Plants);
		});
		for (int32 I = 0; I < Count; ++I)
		{
			const int32 CX = (First + I) % ChunksX;
			const int32 CY = (First + I) / ChunksX;
			CreateChunk(CX, CY);
			FChunk& Chunk = Chunks[ChunkIndex(CX, CY)];
			FTerrChunkBuild& Build = Builds[I];
			Chunk.GroundVertices = Build.Ground.Num();
			Chunk.RoofVertices = Build.Roof.Num();
			Stats.GroundVertices += Chunk.GroundVertices;
			Stats.RoofVertices += Chunk.RoofVertices;
			TerrApplySection(Chunk.Mesh, 0, Build.Ground, GroundMaterial);
			TerrApplySection(Chunk.Mesh, 1, Build.Roof, GroundMaterial);
			if (Build.Roof.Num() > 0 && !bRoofsShown)
			{
				Chunk.Mesh->SetMeshSectionVisible(1, false);
			}
			Chunk.PlantSignature = Build.PlantSignature;
			ApplyChunkPlants(Chunk, Build.Plants);
		}
	}

	Stats.ChunksX = ChunksX;
	Stats.ChunksY = ChunksY;
	Stats.PlantKinds = PlantVisuals.Num();
	Stats.FullBuildMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	++Stats.FullBuilds;
	ScanCursor = 0;
	UE_LOG(LogSimTerrain, Display, TEXT("Terrain: %d x %d chunks, %lld ground vertices, %lld plants in %d kinds, built in %.0f ms"), ChunksX, ChunksY,
		Stats.GroundVertices, Stats.PlantInstances, Stats.PlantKinds, Stats.FullBuildMs);
}

void UTerrainSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Sim = Bridge.Get();
	if (!Sim || !Sim->GetSim() || GTerrainEnable.GetValueOnGameThread() == 0)
	{
		if (bBuilt || TerrainActor)
		{
			Teardown();
		}
		return;
	}

	if (!bBuilt || BuiltGeneration != Sim->GetSimGeneration() || BuiltSim != Sim->GetSim())
	{
		RebuildAll();
		return;
	}
	if (Sim->GetWorldOrigin() != BuiltOrigin && TerrainActor)
	{
		BuiltOrigin = Sim->GetWorldOrigin();
		TerrainActor->SetActorLocation(BuiltOrigin);
	}

	bool bRoofChanged = false;
	ReadThinRoof(bRoofChanged);
	UpdateRoofVisibility(false);
	ScanPlants();
	FlushDirty();
}
