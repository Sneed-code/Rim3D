#include "BuildingSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include "LevelsSubsystem.h"
#include "SimSubsystem.h"
#include "TerrainDraw.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/Windows.h"

#include "WetnessShared.h"

#include <string>

DEFINE_LOG_CATEGORY_STATIC(LogSimBuildings, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GBuildingsEnable(
		TEXT("Buildings.Enable"), 1, TEXT("1 draws the sim's buildings, blueprints and items, 0 draws nothing."), ECVF_Default);

	// The same flat-colour material as the terrain, and the see-through one the BuildingMaterials commandlet makes for the ghosts.
	const TCHAR* const BldGhostMaterialPath = TEXT("/Game/Rim3D/Generated/Buildings/M_BuildingGhost.M_BuildingGhost");
	const TCHAR* const BldGlassMaterialPath = TEXT("/Game/Rim3D/Generated/Buildings/M_WindowGlass.M_WindowGlass");
	const FLinearColor BldGhostColor(0.18f, 0.42f, 0.95f, 0.38f);

	// What a component draws: the fixed parts of a finished building, a door's leaf, a frame (all parts, solid), a ghost (all parts, see-through).
	constexpr int32 BldFixed = 0;
	constexpr int32 BldSliding = 1;
	constexpr int32 BldAll = 2;
	constexpr int32 BldGhost = 3;
	constexpr int32 BldGlass = 4;  // a window's pane (docs/design/windows.md)
	constexpr int32 BldRotor = 5;  // a turning part (visual.rotor: wind turbine blades)
	constexpr int32 BldVariants = 8;  // batch and component keys are Look * BldVariants + variant

	// A frame is drawn this fraction of its height with nothing delivered, and a full height when it is done.
	constexpr float BldFrameMinScale = 0.1f;
	// Frame heights come in this many steps, so a chunk is not rebuilt for every second of work.
	constexpr int32 BldFrameSteps = 10;
	// Items are not drawn beyond this (cm), starting to vanish at 80% of it.
	constexpr float BldItemCullCm = 15000.0f;

	uint64 BldMix(uint64 Value)
	{
		Value += 0x9E3779B97F4A7C15ULL;
		Value = (Value ^ (Value >> 30)) * 0xBF58476D1CE4E5B9ULL;
		Value = (Value ^ (Value >> 27)) * 0x94D049BB133111EBULL;
		return Value ^ (Value >> 31);
	}

	uint64 BldCombine(uint64 Seed, uint64 Value)
	{
		return BldMix(Seed * 0x100000001B3ULL ^ Value);
	}

	float BldYawOf(colony::Rot4 Rot)
	{
		switch (Rot)
		{
			case colony::Rot4::East: return 90.0f;
			case colony::Rot4::South: return 180.0f;
			case colony::Rot4::West: return 270.0f;
			default: return 0.0f;
		}
	}

	int32 BldChunkOf(float Metres)
	{
		return FMath::FloorToInt32(Metres / static_cast<float>(TerrainDraw::ChunkCells));
	}

	/** How many pieces of an item's pile a stack of `Count` is drawn as: one for the smallest, all of them for a full stack. */
	int32 BldPieces(int32 MaxPieces, int32 StackLimit, int32 Count)
	{
		if (MaxPieces <= 1 || StackLimit <= 1)
		{
			return 1;
		}
		const float Fill = FMath::Clamp(static_cast<float>(Count) / static_cast<float>(StackLimit), 0.0f, 1.0f);
		return FMath::Clamp(FMath::CeilToInt32(Fill * static_cast<float>(MaxPieces)), 1, MaxPieces);
	}

	/** A frame's height as a fraction of the finished building's: from the materials delivered, then the work done. */
	float BldFrameScale(const colony::ThingView& View)
	{
		const float Materials = View.materialsNeeded > 0 ? FMath::Clamp(static_cast<float>(View.materialsHave) / static_cast<float>(View.materialsNeeded), 0.0f, 1.0f) : 1.0f;
		const float Done = BldFrameMinScale + (1.0f - BldFrameMinScale) * (0.25f * Materials + 0.75f * FMath::Clamp(View.buildProgress, 0.0f, 1.0f));
		return static_cast<float>(FMath::FloorToInt32(Done * BldFrameSteps)) / static_cast<float>(BldFrameSteps);
	}

	/** Whether a door's leaf runs along y: the walls it sits in are to its north and south. Otherwise the door's facing says. */
	bool BldDoorAlongY(const colony::Map& Map, const colony::ThingView& View)
	{
		const int32 X = FMath::FloorToInt32(View.x);
		const int32 Y = FMath::FloorToInt32(View.y);
		const auto Wall = [&Map, &View](int32 CX, int32 CY)
		{
			const colony::CellPos Cell{CX, CY, View.level};
			return Map.inBounds(Cell) && Map.passage(Cell) == colony::Passage::Blocked;
		};
		const bool bWallsEastWest = Wall(X - 1, Y) || Wall(X + 1, Y);
		const bool bWallsNorthSouth = Wall(X, Y - 1) || Wall(X, Y + 1);
		if (bWallsEastWest != bWallsNorthSouth)
		{
			return bWallsNorthSouth;
		}
		return View.rotation == colony::Rot4::East || View.rotation == colony::Rot4::West;
	}

	// A linked wall's base course and cap (L4): this much wider than the wall on each side, this high, and this shade of it.
	constexpr float BldBaseLedgeCm = 4.0f;
	constexpr float BldBaseHeightCm = 35.0f;
	constexpr float BldCapLedgeCm = 2.5f;
	constexpr float BldCapHeightCm = 8.0f;
	const FLinearColor BldBaseShade(0.72f, 0.72f, 0.72f);
	const FLinearColor BldCapShade(0.85f, 0.85f, 0.85f);
	const TCHAR* const BldSurfaceFolder = TEXT("/Game/Rim3D/Generated/Surfaces");

	uint64 BldCellKey(int32 X, int32 Y, int32 Level)
	{
		return (static_cast<uint64>(static_cast<uint32>(Level)) << 48) | (static_cast<uint64>(static_cast<uint16>(Y)) << 24) | static_cast<uint64>(static_cast<uint16>(X));
	}

	/**
	 * A linked building (a wall) as boxes in a 1 x 1 cell facing north: a post in the middle and an arm out to each side in
	 * `Link` (1 north, 2 east, 4 south, 8 west), with a wider base course and a cap, all made of the stuff.
	 */
	void BldLinkParts(int32 Link, float Thickness, float Height, TArray<VisualMesh::FPart>& Out)
	{
		const float Body = FMath::Max(Height - BldCapHeightCm, BldBaseHeightCm + 1.0f);
		const auto Add = [&Out](float SizeX, float SizeY, float SizeZ, float AtX, float AtY, float AtZ, const FLinearColor& Shade)
		{
			VisualMesh::FPart Part;
			Part.Shape = VisualMesh::EShape::Box;
			Part.Size = FVector3f(SizeX, SizeY, SizeZ);
			Part.At = FVector3f(AtX, AtY, AtZ);
			Part.Color = Shade;
			Part.bStuff = true;
			Out.Add(Part);
		};
		// Each layer (base course, wall, cap) is the post and its arms at one width.
		const auto Layer = [&](float Width, float SizeZ, float AtZ, const FLinearColor& Shade)
		{
			Add(Width, Width, SizeZ, 0.0f, 0.0f, AtZ, Shade);
			const float Length = 50.0f - Width * 0.5f;
			if (Length <= 0.0f)
			{
				return;
			}
			const float Middle = Width * 0.5f + Length * 0.5f;
			if (Link & 1) { Add(Width, Length, SizeZ, 0.0f, Middle, AtZ, Shade); }
			if (Link & 2) { Add(Length, Width, SizeZ, Middle, 0.0f, AtZ, Shade); }
			if (Link & 4) { Add(Width, Length, SizeZ, 0.0f, -Middle, AtZ, Shade); }
			if (Link & 8) { Add(Length, Width, SizeZ, -Middle, 0.0f, AtZ, Shade); }
		};
		Layer(Thickness + 2.0f * BldBaseLedgeCm, BldBaseHeightCm, 0.0f, BldBaseShade);
		Layer(Thickness, Body, 0.0f, FLinearColor::White);
		Layer(Thickness + 2.0f * BldCapLedgeCm, Height - Body, Body, BldCapShade);
	}

	/**
	 * A power cable (visual.cable) in a 1 x 1 cell facing north: a small junction in the middle and a low arm out to each
	 * side in `Link` (1 north, 2 east, 4 south, 8 west), all in the cable's own colour (not the stuff's).
	 */
	void BldCableParts(int32 Link, float Thickness, float Height, const FLinearColor& Color, TArray<VisualMesh::FPart>& Out)
	{
		const auto Add = [&Out, &Color](float SizeX, float SizeY, float SizeZ, float AtX, float AtY)
		{
			VisualMesh::FPart Part;
			Part.Shape = VisualMesh::EShape::Box;
			Part.Size = FVector3f(SizeX, SizeY, SizeZ);
			Part.At = FVector3f(AtX, AtY, 0.0f);
			Part.Color = Color;
			Out.Add(Part);
		};
		const float Junction = Thickness * 2.0f;
		Add(Junction, Junction, Height * 1.2f, 0.0f, 0.0f);
		const float Length = 50.0f;
		const float Middle = Length * 0.5f;
		if (Link & 1) { Add(Thickness, Length, Height, 0.0f, Middle); }
		if (Link & 2) { Add(Length, Thickness, Height, Middle, 0.0f); }
		if (Link & 4) { Add(Thickness, Length, Height, 0.0f, -Middle); }
		if (Link & 8) { Add(Length, Thickness, Height, -Middle, 0.0f); }
	}

	// A window's frame and the gap round the glass (docs/design/windows.md).
	constexpr float BldWindowJambCm = 10.0f;    // wall left at a cell's edge beside the opening
	constexpr float BldWindowFrameCm = 5.0f;    // the frame's width round the pane
	constexpr float BldWindowFrameDepthCm = 7.0f;
	constexpr float BldWindowPaneCm = 1.5f;     // the glass's thickness
	const FLinearColor BldWindowFrameColor(0.16f, 0.15f, 0.14f);

	/**
	 * A linked wall with a window in it (docs/design/windows.md): the wall below the sill and above the lintel as BldLinkParts
	 * makes it, and between the two the opening along the wall's line with a frame round it. `Glass` (if not null) gets the
	 * pane, one box per opening, drawn by its own see-through mesh. A corner or a T keeps its post whole and opens only the
	 * arms along the line. Heights in centimetres.
	 */
	void BldWindowParts(int32 Link, float Thickness, float Height, float SillCm, float LintelCm, TArray<VisualMesh::FPart>& Out,
		TArray<VisualMesh::FPart>* Glass)
	{
		const float Body = FMath::Max(Height - BldCapHeightCm, BldBaseHeightCm + 1.0f);
		SillCm = FMath::Clamp(SillCm, BldBaseHeightCm, Body - 20.0f);
		LintelCm = FMath::Clamp(LintelCm, SillCm + 20.0f, Body - 5.0f);
		const float Band = LintelCm - SillCm;
		// The line the wall runs along: east-west if it joins anything that way (or nothing at all), else north-south.
		const bool bAlongX = (Link & (2 | 8)) != 0 || (Link & (1 | 4)) == 0;
		const int32 PosBit = bAlongX ? 2 : 1;
		const int32 NegBit = bAlongX ? 8 : 4;
		const int32 PerpBits = bAlongX ? (1 | 4) : (2 | 8);
		// Boxes are given along the line (u) and across it (v), and turned into x and y here.
		const auto Add = [&](TArray<VisualMesh::FPart>& To, float SizeU, float SizeV, float SizeZ, float AtU, float AtV, float AtZ,
			const FLinearColor& Color, bool bStuff)
		{
			VisualMesh::FPart Part;
			Part.Shape = VisualMesh::EShape::Box;
			Part.Size = bAlongX ? FVector3f(SizeU, SizeV, SizeZ) : FVector3f(SizeV, SizeU, SizeZ);
			Part.At = bAlongX ? FVector3f(AtU, AtV, AtZ) : FVector3f(AtV, AtU, AtZ);
			Part.Color = Color;
			Part.bStuff = bStuff;
			To.Add(Part);
		};
		// Below the sill and above the lintel: the plain wall, cut short. BldLinkParts gives base course, body and cap; the
		// lower part keeps the base course and the body up to the sill, the upper part the body from the lintel and the cap.
		{
			TArray<VisualMesh::FPart> Whole;
			BldLinkParts(Link, Thickness, Height, Whole);
			for (const VisualMesh::FPart& Part : Whole)
			{
				const float Bottom = Part.At.Z;
				const float Top = Part.At.Z + Part.Size.Z;
				if (Top <= SillCm || Bottom >= LintelCm)
				{
					Out.Add(Part);  // the base course, the cap
					continue;
				}
				if (Bottom < SillCm)
				{
					VisualMesh::FPart Low = Part;
					Low.Size.Z = SillCm - Bottom;
					Out.Add(Low);
				}
				if (Top > LintelCm)
				{
					VisualMesh::FPart High = Part;
					High.At.Z = LintelCm;
					High.Size.Z = Top - LintelCm;
					Out.Add(High);
				}
			}
		}
		// Between them: the arms across the line stay whole, and so does the post if there are any.
		const float Half = Thickness * 0.5f;
		const float ArmLength = 50.0f - Half;
		const bool bPerp = (Link & PerpBits) != 0;
		if (ArmLength > 0.0f)
		{
			const int32 PerpPos = bAlongX ? 1 : 2;  // north for an east-west wall, east for a north-south one
			const int32 PerpNeg = bAlongX ? 4 : 8;
			if (Link & PerpPos) { Add(Out, Thickness, ArmLength, Band, 0.0f, Half + ArmLength * 0.5f, SillCm, FLinearColor::White, true); }
			if (Link & PerpNeg) { Add(Out, Thickness, ArmLength, Band, 0.0f, -Half - ArmLength * 0.5f, SillCm, FLinearColor::White, true); }
		}
		if (bPerp)
		{
			Add(Out, Thickness, Thickness, Band, 0.0f, 0.0f, SillCm, FLinearColor::White, true);
		}
		// The openings along the line, with a jamb of wall where one meets the cell's edge.
		const float PosEnd = (Link & PosBit) ? 50.0f : Half;
		const float NegEnd = (Link & NegBit) ? -50.0f : -Half;
		struct FSpan { float From; float To; };
		TArray<FSpan, TInlineAllocator<2>> Openings;
		if (bPerp)
		{
			if (Link & PosBit) { Openings.Add({Half, PosEnd - BldWindowJambCm}); }
			if (Link & NegBit) { Openings.Add({NegEnd + BldWindowJambCm, -Half}); }
		}
		else
		{
			Openings.Add({NegEnd + BldWindowJambCm, PosEnd - BldWindowJambCm});
		}
		if (Link & PosBit || !bPerp)
		{
			Add(Out, BldWindowJambCm, Thickness, Band, PosEnd - BldWindowJambCm * 0.5f, 0.0f, SillCm, FLinearColor::White, true);
		}
		if (Link & NegBit || !bPerp)
		{
			Add(Out, BldWindowJambCm, Thickness, Band, NegEnd + BldWindowJambCm * 0.5f, 0.0f, SillCm, FLinearColor::White, true);
		}
		for (const FSpan& Span : Openings)
		{
			const float Length = Span.To - Span.From;
			if (Length < 2.0f * BldWindowFrameCm + 5.0f)
			{
				// Too short to open: wall.
				Add(Out, FMath::Max(Length, 0.0f), Thickness, Band, (Span.From + Span.To) * 0.5f, 0.0f, SillCm, FLinearColor::White, true);
				continue;
			}
			const float Mid = (Span.From + Span.To) * 0.5f;
			// A stone sill a little proud of the wall, then the frame: bottom, top and two sides, at the middle of the wall's depth.
			Add(Out, Length, Thickness + 6.0f, 3.0f, Mid, 0.0f, SillCm - 3.0f, BldCapShade, true);
			Add(Out, Length, BldWindowFrameDepthCm, BldWindowFrameCm, Mid, 0.0f, SillCm, BldWindowFrameColor, false);
			Add(Out, Length, BldWindowFrameDepthCm, BldWindowFrameCm, Mid, 0.0f, LintelCm - BldWindowFrameCm, BldWindowFrameColor, false);
			Add(Out, BldWindowFrameCm, BldWindowFrameDepthCm, Band, Span.From + BldWindowFrameCm * 0.5f, 0.0f, SillCm, BldWindowFrameColor, false);
			Add(Out, BldWindowFrameCm, BldWindowFrameDepthCm, Band, Span.To - BldWindowFrameCm * 0.5f, 0.0f, SillCm, BldWindowFrameColor, false);
			// The reveal: the opening's top and bottom faces are the wall cut back, which the parts above already are.
			if (Glass)
			{
				Add(*Glass, Length - 2.0f * BldWindowFrameCm, BldWindowPaneCm, Band - 2.0f * BldWindowFrameCm, Mid, 0.0f,
					SillCm + BldWindowFrameCm, FLinearColor(0.85f, 0.92f, 0.96f), false);
			}
		}
	}

	struct FBldBatch
	{
		UStaticMesh* Mesh = nullptr;
		bool bGhost = false;
		bool bItem = false;
		bool bShadow = true;
		TArray<FTransform> Transforms;
		TArray<float> Damage;  // per instance, 0 whole .. 1 nearly gone (custom data WetnessShared::DamageDataIndex)
	};

	/** How broken a finished building looks (docs/design/destruction.md): its lost hit points, in eight steps so a chunk is not
	 * rebuilt for every bullet. */
	float BldDamageOf(const colony::ThingView& View)
	{
		if (View.maxHitPoints <= 0 || View.hitPoints >= View.maxHitPoints)
		{
			return 0.0f;
		}
		const float Lost = 1.0f - static_cast<float>(FMath::Max(View.hitPoints, 0)) / static_cast<float>(View.maxHitPoints);
		return FMath::CeilToFloat(FMath::Clamp(Lost, 0.0f, 1.0f) * 8.0f) / 8.0f;
	}
}

bool UBuildingSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UBuildingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBuildingSubsystem, STATGROUP_Tickables);
}

void UBuildingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Bridge = Collection.InitializeDependency<USimSubsystem>();
}

void UBuildingSubsystem::Deinitialize()
{
	// The world is going away and takes the actor and its components with it.
	DrawActor = nullptr;
	Chunks.Reset();
	DoorSlots.Reset();
	RotorSlots.Reset();
	Super::Deinitialize();
}

void UBuildingSubsystem::Teardown()
{
	if (DrawActor)
	{
		DrawActor->Destroy();
	}
	DrawActor = nullptr;
	Chunks.Reset();
	DoorSlots.Reset();
	RotorSlots.Reset();
	Looks.Reset();
	LookIndex.Reset();
	DefLooks.Reset();
	KeepMeshes.Reset();
	bBuilt = false;
	BuiltSim = nullptr;
	BuiltGeneration = -1;
	LastSeconds = -1;
	ShownTop = -1;
	const int32 FullBuilds = Stats.FullBuilds;
	Stats = FBuildingStats();
	Stats.FullBuilds = FullBuilds;
}

int32 UBuildingSubsystem::GetChunkInstances(int32 ChunkX, int32 ChunkY) const
{
	if (!bBuilt || ChunkX < 0 || ChunkY < 0 || ChunkX >= ChunksX || ChunkY >= ChunksY)
	{
		return 0;
	}
	int32 Count = 0;
	for (int32 Level = 0; Level < colony::Map::kLevels; ++Level)
	{
		Count += Chunks[ChunkIndex(Level, ChunkX, ChunkY)].Instances;
	}
	return Count;
}

int32 UBuildingSubsystem::GetLevelInstances(int32 Level, bool bVisibleOnly) const
{
	int32 Count = 0;
	if (!bBuilt || Level < 0 || Level >= colony::Map::kLevels)
	{
		return 0;
	}
	for (int32 CY = 0; CY < ChunksY; ++CY)
	{
		for (int32 CX = 0; CX < ChunksX; ++CX)
		{
			for (const TPair<int32, UInstancedStaticMeshComponent*>& Pair : Chunks[ChunkIndex(Level, CX, CY)].Components)
			{
				if (Pair.Value && (!bVisibleOnly || Pair.Value->IsVisible()))
				{
					Count += Pair.Value->GetInstanceCount();
				}
			}
		}
	}
	return Count;
}

bool UBuildingSubsystem::GetRotor(colony::Entity Thing, float& OutAngleDeg, float& OutRpm) const
{
	const FRotorSlot* Slot = RotorSlots.Find(static_cast<uint32>(Thing));
	if (!Slot)
	{
		return false;
	}
	OutAngleDeg = Slot->AngleDeg;
	OutRpm = Slot->Rpm;
	return true;
}

float UBuildingSubsystem::GetDoorShown(colony::Entity Door) const
{
	const FDoorSlot* Slot = DoorSlots.Find(static_cast<uint32>(Door));
	return Slot ? Slot->Shown : -1.0f;
}

float UBuildingSubsystem::GetDoorLeafOffsetCm(colony::Entity Door) const
{
	const FDoorSlot* Slot = DoorSlots.Find(static_cast<uint32>(Door));
	return Slot ? Slot->Shown * Slot->SlideCm : 0.0f;
}

float UBuildingSubsystem::GetDrawnHeightCm(colony::DefId Def) const
{
	const FDefLook* Look = LookOfDef(Def);
	return Look ? Look->HeightCm : 0.0f;
}

int32 UBuildingSubsystem::GetGhostInstances() const
{
	int32 Count = 0;
	for (const FChunk& Chunk : Chunks)
	{
		for (const TPair<int32, UInstancedStaticMeshComponent*>& Pair : Chunk.Components)
		{
			if ((Pair.Key & 3) == BldGhost && Pair.Value)
			{
				Count += Pair.Value->GetInstanceCount();
			}
		}
	}
	return Count;
}

UStaticMesh* UBuildingSubsystem::GetWholeMesh(colony::DefId Def, colony::DefId Stuff)
{
	const int32 LookId = bBuilt ? GetLook(Def, Stuff) : -1;
	return LookId >= 0 ? Looks[LookId].All : nullptr;
}

bool UBuildingSubsystem::GetPlacedTransform(colony::DefId Def, colony::CellPos Cell, colony::Rot4 Rot, FTransform& OutWorld) const
{
	const FDefLook* Look = bBuilt ? LookOfDef(Def) : nullptr;
	if (!Look || Look->bItem || !BuiltSim)
	{
		return false;
	}
	const colony::Map& Map = BuiltSim->map();
	const colony::CellRect Rect = colony::footprintRect(Cell, FMath::RoundToInt32(Look->FootprintCm.X / 100.0f), FMath::RoundToInt32(Look->FootprintCm.Y / 100.0f), Rot);
	// The lowest ground under the footprint, as the built building stands (an upper floor runs 3 m a level above it).
	const int32 Level = FMath::Clamp(Cell.level, 0, colony::Map::kLevels - 1);
	float Base = 1.0e9f;
	for (int32 Y = Rect.minY; Y <= Rect.minY + Rect.sizeY; ++Y)
	{
		for (int32 X = Rect.minX; X <= Rect.minX + Rect.sizeX; ++X)
		{
			Base = FMath::Min(Base, Map.groundHeight(colony::CellPos{FMath::Clamp(X, 0, Map.width() - 1), FMath::Clamp(Y, 0, Map.height() - 1), Level}));
		}
	}
	const FVector Local((static_cast<float>(Rect.minX) + static_cast<float>(Rect.sizeX) * 0.5f) * 100.0f, (static_cast<float>(Rect.minY) + static_cast<float>(Rect.sizeY) * 0.5f) * 100.0f, Base * 100.0f);
	OutWorld = FTransform(FRotator(0.0, BldYawOf(Rot), 0.0), BuiltOrigin + Local);
	return true;
}

const UBuildingSubsystem::FDefLook* UBuildingSubsystem::LookOfDef(colony::DefId Def) const
{
	return Def.valid() && Def.value < static_cast<uint32>(DefLooks.Num()) && DefLooks[Def.value].bRead ? &DefLooks[Def.value] : nullptr;
}

void UBuildingSubsystem::ReadDefs(const colony::Sim& Sim)
{
	const colony::DefDatabase& Defs = Sim.defs();
	DefLooks.Reset();
	DefLooks.SetNum(static_cast<int32>(Defs.size()));
	for (const colony::DefId Id : Defs.idsOfKind("Things"))
	{
		const colony::Def& Def = Defs.get(Id);
		const std::string Category = Def.text("category");
		if (Category != "Building" && Category != "Item")
		{
			continue;
		}
		FDefLook& Look = DefLooks[static_cast<int32>(Id.value)];
		Look.bRead = true;
		Look.bItem = Category == "Item";
		Look.bDoor = Def.text("thingClass") == "Building_Door";
		Look.bConform = Category == "Building" && colony::groundFitOf(Def) == colony::GroundFit::Conform;
		Look.bDeck = Category == "Building" && colony::bridgeKindOf(Def) != colony::BridgeKind::None;
		Look.bDrawnElsewhere = !Def.text("visual.drawnBy").empty();
		Look.bJoinsWalls = Def.flag("visual.joinsWalls");
		Look.bPowered = Category == "Building" && Def.has("power");
		if (Def.has("visual.cable"))
		{
			Look.bCable = true;
			Look.CableThicknessCm = FMath::Clamp(static_cast<float>(Def.number("visual.cable.thickness", 0.07)) * 100.0f, 1.0f, 30.0f);
			Look.CableHeightCm = FMath::Clamp(static_cast<float>(Def.number("visual.cable.height", 0.05)) * 100.0f, 1.0f, 30.0f);
			Look.CableColor = VisualMesh::ReadColor(Def, "visual.cable.color", Look.CableColor);
		}
		if (Def.has("visual.link"))
		{
			Look.bLink = true;
			Look.LinkThicknessCm = FMath::Clamp(static_cast<float>(Def.number("visual.link.thickness", 0.5)) * 100.0f, 10.0f, 100.0f);
		}
		if (!Look.bItem)
		{
			Look.FootprintCm = FVector2f(static_cast<float>(Def.numberAt("size", 0, 1.0)) * 100.0f, static_cast<float>(Def.numberAt("size", 1, 1.0)) * 100.0f);
		}
		VisualMesh::ReadParts(Def, "visual", Look.FootprintCm, Look.Parts);
		if (Look.Parts.IsEmpty())
		{
			// A def with no look yet still shows: a grey block, a metre high for a building and a handful for an item.
			VisualMesh::FPart Block;
			Block.Shape = VisualMesh::EShape::Box;
			Block.Size = Look.bItem ? FVector3f(25.0f, 25.0f, 25.0f) : FVector3f(Look.FootprintCm.X, Look.FootprintCm.Y, 100.0f);
			Block.Color = FLinearColor(0.35f, 0.35f, 0.35f);
			Look.Parts.Add(Block);
		}
		for (const VisualMesh::FPart& Part : Look.Parts)
		{
			if (Part.Slide > 0.0f)
			{
				Look.bHasSliding = true;
				Look.SlideCm = FMath::Max(Look.SlideCm, Part.Slide);
			}
		}
		Look.MaxPieces = FMath::Max(1, static_cast<int32>(Def.integer("visual.pieces", 1)));
		Look.LayerCm = static_cast<float>(Def.number("visual.layer", 0.1)) * 100.0f;
		Look.StackLimit = FMath::Max(1, static_cast<int32>(Def.integer("stackLimit", 1)));
		Look.HeightCm = VisualMesh::HeightOf(Look.Parts);

		// A mesh of the def's own (A1 makes them, tools/placeholder_art.py); without it the shapes above are drawn.
		const std::string MeshPath = Def.text("visual.mesh");
		if (!MeshPath.empty())
		{
			Look.ArtMesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(MeshPath.c_str()));
			if (Look.ArtMesh)
			{
				KeepMeshes.Add(Look.ArtMesh);
				if (!Look.bDoor)
				{
					Look.HeightCm = Look.ArtMesh->GetBounds().BoxExtent.Z * 2.0f;
				}
			}
			else
			{
				UE_LOG(LogSimBuildings, Warning, TEXT("%s: mesh %s not found, drawing its shapes instead (python tools/placeholder_art.py, then the PlaceholderArt commandlet)"), UTF8_TO_TCHAR(Def.defName().c_str()), UTF8_TO_TCHAR(MeshPath.c_str()));
			}
		}
		// Its turning part (docs/design/building-draw.md "Downloaded models with a moving part"): only with the def's own
		// mesh, since the shapes drawn without one already have their blades.
		const std::string RotorPath = Def.text("visual.rotor.mesh");
		if (Look.ArtMesh && !RotorPath.empty())
		{
			Look.RotorMesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(RotorPath.c_str()));
			if (Look.RotorMesh)
			{
				KeepMeshes.Add(Look.RotorMesh);
				Look.RotorAtCm = FVector(Def.numberAt("visual.rotor.at", 0, 0.0), Def.numberAt("visual.rotor.at", 1, 0.0), Def.numberAt("visual.rotor.at", 2, 0.0)) * 100.0;
				Look.RotorAxis = FVector(Def.numberAt("visual.rotor.axis", 0, 0.0), Def.numberAt("visual.rotor.axis", 1, 1.0), Def.numberAt("visual.rotor.axis", 2, 0.0)).GetSafeNormal(UE_SMALL_NUMBER, FVector::YAxisVector);
				Look.RotorRpm = static_cast<float>(Def.number("visual.rotor.rpm", 20.0));
				Look.RotorFullWatts = static_cast<float>(Def.number("power.output", 0.0));
			}
			else
			{
				UE_LOG(LogSimBuildings, Warning, TEXT("%s: rotor mesh %s not found; drawn without it"), UTF8_TO_TCHAR(Def.defName().c_str()), UTF8_TO_TCHAR(RotorPath.c_str()));
			}
		}
	}
}

UMaterialInterface* UBuildingSubsystem::SurfaceOf(colony::DefId Stuff)
{
	if (!Stuff.valid() || !BuiltSim)
	{
		return nullptr;
	}
	if (const TObjectPtr<UMaterialInterface>* Found = Surfaces.Find(static_cast<int32>(Stuff.value)))
	{
		return *Found;
	}
	UMaterialInterface* Material = nullptr;
	const std::string Surface = BuiltSim->defs().get(Stuff).text("visual.surface");
	if (!Surface.empty())
	{
		const FString Name = FString::Printf(TEXT("MI_Surface_%s"), UTF8_TO_TCHAR(Surface.c_str()));
		Material = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), BldSurfaceFolder, *Name, *Name));
		if (!Material)
		{
			// Made by the SurfaceMaterials commandlet (assets/manifest.json); without it the stuff's plain colour is drawn.
			UE_LOG(LogSimBuildings, Warning, TEXT("%s/%s not found: %s is drawn in its plain colour. Run tools/stuff_textures.py and the SurfaceMaterials commandlet."),
				BldSurfaceFolder, *Name, UTF8_TO_TCHAR(BuiltSim->defs().get(Stuff).defName().c_str()));
		}
	}
	Surfaces.Add(static_cast<int32>(Stuff.value), Material);
	return Material;
}

int32 UBuildingSubsystem::GetLook(colony::DefId Def, colony::DefId Stuff, int32 Link, int32 Window)
{
	const FDefLook* DefLook = LookOfDef(Def);
	if (!DefLook || !BuiltSim)
	{
		return -1;
	}
	Link = DefLook->bLink || DefLook->bCable ? Link : -1;
	Window = Link >= 0 && DefLook->bLink ? FMath::Clamp(Window, 0, 2) : 0;
	const uint64 Key = (static_cast<uint64>(Def.value) << 32) | (static_cast<uint64>(Window) << 29) | (static_cast<uint64>(Link + 1) << 24) |
		(Stuff.valid() ? static_cast<uint64>(Stuff.value) + 1 : 0);
	if (const int32* Found = LookIndex.Find(Key))
	{
		return *Found;
	}
	FLinearColor StuffColor(0.35f, 0.35f, 0.35f);
	if (Stuff.valid())
	{
		StuffColor = VisualMesh::ReadColor(BuiltSim->defs().get(Stuff), "visual.color", StuffColor);
	}
	UMaterialInterface* Surface = SurfaceOf(Stuff);
	TArray<VisualMesh::FPart> LinkParts;
	TArray<VisualMesh::FPart> GlassParts;
	if (Link >= 0 && DefLook->bCable)
	{
		BldCableParts(Link, DefLook->CableThicknessCm, DefLook->CableHeightCm, DefLook->CableColor, LinkParts);
	}
	else if (Link >= 0 && Window > 0)
	{
		const colony::WindowNumbers& Numbers = BuiltSim->windows().numbers();
		BldWindowParts(Link, DefLook->LinkThicknessCm, DefLook->HeightCm, static_cast<float>(Numbers.sillDm) * 10.0f,
			static_cast<float>(Numbers.lintelDm) * 10.0f, LinkParts, Window == 1 && GlassMaterial ? &GlassParts : nullptr);
	}
	else if (Link >= 0)
	{
		BldLinkParts(Link, DefLook->LinkThicknessCm, DefLook->HeightCm, LinkParts);
	}
	const TArray<VisualMesh::FPart>& Parts = Link >= 0 ? LinkParts : DefLook->Parts;
	FLook Look;
	Look.DefValue = static_cast<int32>(Def.value);
	const auto Make = [&](VisualMesh::EPartSet Set) -> UStaticMesh*
	{
		UStaticMesh* Mesh = VisualMesh::MakeMesh(this, SolidMaterial, Parts, Set, &StuffColor, false, Surface);
		if (Mesh)
		{
			KeepMeshes.Add(Mesh);
			++Stats.Meshes;
		}
		return Mesh;
	};
	Look.Fixed = Make(DefLook->bHasSliding ? VisualMesh::EPartSet::Fixed : VisualMesh::EPartSet::All);
	if (DefLook->bHasSliding)
	{
		Look.Sliding = DefLook->bDoor && DefLook->ArtMesh ? DefLook->ArtMesh : Make(VisualMesh::EPartSet::Sliding);
		Look.All = Make(VisualMesh::EPartSet::All);
	}
	else if (DefLook->ArtMesh)
	{
		// Frames and ghosts keep the plain shapes: the ghost material replaces only the first material of a mesh.
		Look.All = Make(VisualMesh::EPartSet::All);
		Look.Fixed = DefLook->ArtMesh;
	}
	else
	{
		Look.All = Look.Fixed;
	}
	if (!GlassParts.IsEmpty())
	{
		Look.Glass = VisualMesh::MakeMesh(this, GlassMaterial, GlassParts, VisualMesh::EPartSet::All, nullptr, false);
		if (Look.Glass)
		{
			KeepMeshes.Add(Look.Glass);
			++Stats.Meshes;
		}
	}
	const int32 Index = Looks.Add(Look);
	LookIndex.Add(Key, Index);
	return Index;
}

UInstancedStaticMeshComponent* UBuildingSubsystem::GetComponent(FChunk& Chunk, int32 Key, UStaticMesh* Mesh, bool bGhost, bool bItem, bool bCastShadow)
{
	if (UInstancedStaticMeshComponent** Found = Chunk.Components.Find(Key))
	{
		return *Found;
	}
	UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(DrawActor);
	Component->SetStaticMesh(Mesh);
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(bCastShadow && !bGhost);
	Component->bAffectDistanceFieldLighting = false;
	if (bGhost && GhostMaterial)
	{
		for (int32 Slot = 0; Slot < FMath::Max(1, Mesh ? Mesh->GetStaticMaterials().Num() : 1); ++Slot)
		{
			Component->SetMaterial(Slot, GhostMaterial);
		}
	}
	if (!bGhost && !bItem)
	{
		// Room for the damage look's value (docs/design/destruction.md).
		Component->NumCustomDataFloats = WetnessShared::DamageDataIndex + 1;
	}
	if (bItem)
	{
		Component->SetCullDistances(static_cast<int32>(BldItemCullCm * 0.8f), static_cast<int32>(BldItemCullCm));
	}
	Component->SetupAttachment(DrawActor->GetRootComponent());
	Component->RegisterComponent();
	Chunk.Components.Add(Key, Component);
	return Component;
}

void UBuildingSubsystem::RebuildAll()
{
	USimSubsystem* Sim = Bridge.Get();
	Teardown();
	if (!Sim || !Sim->GetSim() || !GetWorld())
	{
		return;
	}
	const colony::Sim& Core = *Sim->GetSim();
	BuiltSim = &Core;
	BuiltGeneration = Sim->GetSimGeneration();
	BuiltOrigin = Sim->GetWorldOrigin();
	ChunksX = (Core.map().width() + TerrainDraw::ChunkCells - 1) / TerrainDraw::ChunkCells;
	ChunksY = (Core.map().height() + TerrainDraw::ChunkCells - 1) / TerrainDraw::ChunkCells;

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	DrawActor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), BuiltOrigin, FRotator::ZeroRotator, Params);
	USceneComponent* Root = NewObject<USceneComponent>(DrawActor, TEXT("BuildingsRoot"));
	DrawActor->SetRootComponent(Root);
	Root->RegisterComponent();
	Root->SetWorldLocation(BuiltOrigin);
#if WITH_EDITOR
	DrawActor->SetActorLabel(TEXT("Buildings"));
#endif

	SolidMaterial = WetnessShared::LoadSolidMaterial();
	if (UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, BldGhostMaterialPath))
	{
		UMaterialInstanceDynamic* Ghost = UMaterialInstanceDynamic::Create(Parent, this);
		Ghost->SetVectorParameterValue(TEXT("Color"), BldGhostColor);
		Ghost->SetScalarParameterValue(TEXT("Opacity"), BldGhostColor.A);
		GhostMaterial = Ghost;
	}
	else
	{
		// Made by the BuildingMaterials commandlet (docs/design/building-draw.md); without it plans are drawn solid.
		UE_LOG(LogSimBuildings, Warning, TEXT("%s not found: blueprints are drawn solid. Run the BuildingMaterials commandlet (assets/manifest.json)."), BldGhostMaterialPath);
		GhostMaterial = nullptr;
	}
	GlassMaterial = LoadObject<UMaterialInterface>(nullptr, BldGlassMaterialPath);
	if (!GlassMaterial)
	{
		UE_LOG(LogSimBuildings, Warning, TEXT("%s not found: windows are drawn without glass. Run the BuildingMaterials commandlet (assets/manifest.json)."), BldGlassMaterialPath);
	}
	ReadDefs(Core);

	Chunks.SetNum(colony::Map::kLevels * ChunksX * ChunksY);
	ScanLists.SetNum(colony::Map::kLevels * ChunksX * ChunksY);
	bBuilt = true;
	++Stats.FullBuilds;
	Refresh();
}

void UBuildingSubsystem::Refresh()
{
	USimSubsystem* Sim = Bridge.Get();
	if (!bBuilt || !Sim || !Sim->GetSim() || Sim->GetSim() != BuiltSim)
	{
		return;
	}
	const colony::Snapshot& Snapshot = Sim->GetCurrentSnapshot();
	LastSeconds = Snapshot.gameSeconds;
	const colony::Map& Map = BuiltSim->map();

	for (TArray<int32>& List : ScanLists)
	{
		List.Reset();
	}
	ScanSignatures.Init(0, Chunks.Num());

	// Linked buildings (walls) join the walls, doors and rock beside them: first where those stand, then each one's neighbours.
	const int32 ThingCount = static_cast<int32>(Snapshot.things.size());
	ScanLinks.Init(-1, ThingCount);
	TSet<uint64> LinkCells;
	TSet<uint64> PowerCells;  // every cell of a built building with a power part: cables run up to them
	for (const colony::ThingView& View : Snapshot.things)
	{
		const FDefLook* Look = View.category == colony::ThingCategory::Building ? LookOfDef(View.def) : nullptr;
		if (Look && (Look->bLink || Look->bDoor || Look->bJoinsWalls))
		{
			LinkCells.Add(BldCellKey(FMath::FloorToInt32(View.x), FMath::FloorToInt32(View.y), View.level));
		}
		if (Look && Look->bPowered && View.stage == colony::ConstructionStage::Built)
		{
			const int32 MinX = FMath::RoundToInt32(View.x - static_cast<float>(View.sizeX) * 0.5f);
			const int32 MinY = FMath::RoundToInt32(View.y - static_cast<float>(View.sizeY) * 0.5f);
			for (int32 CellY = MinY; CellY < MinY + View.sizeY; ++CellY)
			{
				for (int32 CellX = MinX; CellX < MinX + View.sizeX; ++CellX)
				{
					PowerCells.Add(BldCellKey(CellX, CellY, View.level));
				}
			}
		}
	}
	const auto Joins = [&Map, &LinkCells](int32 X, int32 Y, int32 Level)
	{
		const colony::CellPos Cell{X, Y, Level};
		return Map.inBounds(Cell) && (LinkCells.Contains(BldCellKey(X, Y, Level)) || Map.terrain(Cell) == colony::Terrain::Rock);
	};
	for (int32 I = 0; I < ThingCount; ++I)
	{
		const colony::ThingView& View = Snapshot.things[static_cast<size_t>(I)];
		const FDefLook* Look = View.category == colony::ThingCategory::Building ? LookOfDef(View.def) : nullptr;
		if (Look && Look->bLink)
		{
			const int32 X = FMath::FloorToInt32(View.x);
			const int32 Y = FMath::FloorToInt32(View.y);
			ScanLinks[I] = static_cast<int8>((Joins(X, Y + 1, View.level) ? 1 : 0) | (Joins(X + 1, Y, View.level) ? 2 : 0) |
				(Joins(X, Y - 1, View.level) ? 4 : 0) | (Joins(X - 1, Y, View.level) ? 8 : 0));
		}
		else if (Look && Look->bCable && View.stage == colony::ConstructionStage::Built)
		{
			// A cable runs to the powered buildings (and other cables) on its four sides, as the sim's nets join them.
			const int32 X = FMath::FloorToInt32(View.x);
			const int32 Y = FMath::FloorToInt32(View.y);
			const auto Wired = [&PowerCells, &View](int32 CellX, int32 CellY) { return PowerCells.Contains(BldCellKey(CellX, CellY, View.level)); };
			ScanLinks[I] = static_cast<int8>((Wired(X, Y + 1) ? 1 : 0) | (Wired(X + 1, Y) ? 2 : 0) | (Wired(X, Y - 1) ? 4 : 0) | (Wired(X - 1, Y) ? 8 : 0));
		}
	}
	int32 Built = 0, Frames = 0, Blueprints = 0, Items = 0, Doors = 0;
	for (int32 I = 0; I < static_cast<int32>(Snapshot.things.size()); ++I)
	{
		const colony::ThingView& View = Snapshot.things[static_cast<size_t>(I)];
		if (View.category == colony::ThingCategory::Pawn)
		{
			continue;
		}
		const FDefLook* Look = LookOfDef(View.def);
		const int32 CX = BldChunkOf(View.x);
		const int32 CY = BldChunkOf(View.y);
		if (!Look || Look->bDrawnElsewhere || CX < 0 || CY < 0 || CX >= ChunksX || CY >= ChunksY || View.level < 0 || View.level >= colony::Map::kLevels)
		{
			continue;
		}
		uint64 H = BldCombine(static_cast<uint64>(static_cast<uint32>(View.id)), View.def.value);
		H = BldCombine(H, static_cast<uint64>(FMath::RoundToInt32(View.x * 4.0f)) * 100003ULL + static_cast<uint64>(FMath::RoundToInt32(View.y * 4.0f)));
		// Its height too: levelled ground (docs/design/levelling.md) moves what stands on it.
		H = BldCombine(H, static_cast<uint64>(static_cast<int64>(FMath::RoundToInt32(View.z * 100.0f))));
		if (View.category == colony::ThingCategory::Item)
		{
			H = BldCombine(H, static_cast<uint64>(BldPieces(Look->MaxPieces, Look->StackLimit, View.stackCount)));
			++Items;
		}
		else
		{
			H = BldCombine(H, static_cast<uint64>(View.stage) * 16ULL + static_cast<uint64>(View.rotation));
			H = BldCombine(H, View.stuff.valid() ? View.stuff.value + 1ULL : 0ULL);
			switch (View.stage)
			{
				case colony::ConstructionStage::Built:
					++Built;
					H = BldCombine(H, 0xDA3AULL + static_cast<uint64>(BldDamageOf(View) * 8.0f));
					break;
				case colony::ConstructionStage::Frame:
					++Frames;
					H = BldCombine(H, static_cast<uint64>(BldFrameScale(View) * 1000.0f));
					break;
				case colony::ConstructionStage::Blueprint:
					++Blueprints;
					break;
			}
			if (Look->bDoor && View.stage == colony::ConstructionStage::Built)
			{
				++Doors;
				H = BldCombine(H, BldDoorAlongY(Map, View) ? 1ULL : 2ULL);
			}
			if (ScanLinks[I] >= 0)
			{
				H = BldCombine(H, 0x11C0ULL + static_cast<uint64>(ScanLinks[I]));
			}
			if (View.window != colony::WindowState::None)
			{
				H = BldCombine(H, 0x3D1AULL + static_cast<uint64>(View.window));
			}
		}
		const int32 Chunk = ChunkIndex(View.level, CX, CY);
		ScanSignatures[Chunk] += H;
		ScanLists[Chunk].Add(I);
	}
	Stats.Built = Built;
	Stats.Frames = Frames;
	Stats.Blueprints = Blueprints;
	Stats.Items = Items;
	Stats.Doors = Doors;

	bool bRebuilt = false;
	for (int32 Level = 0; Level < colony::Map::kLevels; ++Level)
	{
		for (int32 CY = 0; CY < ChunksY; ++CY)
		{
			for (int32 CX = 0; CX < ChunksX; ++CX)
			{
				const int32 Chunk = ChunkIndex(Level, CX, CY);
				if (ScanSignatures[Chunk] != Chunks[Chunk].Signature)
				{
					RebuildChunk(Level, CX, CY, ScanLists[Chunk], Snapshot);
					Chunks[Chunk].Signature = ScanSignatures[Chunk];
					bRebuilt = true;
				}
			}
		}
	}
	// A rebuilt chunk may have new components, which start shown.
	ApplyCut(bRebuilt);
}

void UBuildingSubsystem::ApplyCut(bool bForce)
{
	const int32 Top = ULevelsSubsystem::TopShownLevel(GetWorld());
	if (!bForce && Top == ShownTop)
	{
		return;
	}
	ShownTop = Top;
	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		const bool bShow = LevelOfChunk(Index) <= Top;
		for (TPair<int32, UInstancedStaticMeshComponent*>& Pair : Chunks[Index].Components)
		{
			if (Pair.Value && Pair.Value->IsVisible() != bShow)
			{
				Pair.Value->SetVisibility(bShow);
			}
		}
	}
}

void UBuildingSubsystem::RebuildChunk(int32 Level, int32 ChunkX, int32 ChunkY, const TArray<int32>& ThingIndexes, const colony::Snapshot& Snapshot)
{
	const double Start = FPlatformTime::Seconds();
	FChunk& Chunk = Chunks[ChunkIndex(Level, ChunkX, ChunkY)];
	const colony::Map& Map = BuiltSim->map();

	TMap<int32, FBldBatch> Batches;
	struct FPendingDoor
	{
		uint32 Id;
		int32 Key;
		int32 Index;
		FTransform Base;
		FVector Direction;
		float SlideCm;
	};
	TArray<FPendingDoor> Pending;
	struct FPendingRotor
	{
		uint32 Id;
		int32 Key;
		int32 Index;
		FTransform Base;
		FVector Axis;
		float MaxRpm;
		float FullWatts;
	};
	TArray<FPendingRotor> PendingRotors;

	const auto Add = [&Batches](int32 Look, int32 Variant, UStaticMesh* Mesh, bool bGhost, bool bItem, bool bShadow, const FTransform& Transform, float Damage = 0.0f) -> int32
	{
		FBldBatch& Batch = Batches.FindOrAdd(Look * BldVariants + Variant);
		Batch.Mesh = Mesh;
		Batch.bGhost = bGhost;
		Batch.bItem = bItem;
		Batch.bShadow = bShadow;
		Batch.Damage.Add(Damage);
		return Batch.Transforms.Add(Transform);
	};

	for (const int32 ThingIndex : ThingIndexes)
	{
		const colony::ThingView& View = Snapshot.things[static_cast<size_t>(ThingIndex)];
		const FDefLook* DefLook = LookOfDef(View.def);
		if (!DefLook)
		{
			continue;
		}
		if (View.category == colony::ThingCategory::Item)
		{
			const int32 ItemLook = GetLook(View.def, colony::DefId());
			if (ItemLook < 0 || !Looks[ItemLook].Fixed)
			{
				continue;
			}
			UStaticMesh* Mesh = Looks[ItemLook].Fixed;
			const int32 Pieces = BldPieces(DefLook->MaxPieces, DefLook->StackLimit, View.stackCount);
			const uint64 Seed = BldMix(static_cast<uint64>(static_cast<uint32>(View.id)) + 0x51ED27ULL);
			for (int32 P = 0; P < Pieces; ++P)
			{
				const uint64 H = BldMix(Seed + static_cast<uint64>(P) * 7919ULL);
				const float J1 = (static_cast<float>(H & 0xFFFF) / 65535.0f - 0.5f) * 10.0f;
				const float J2 = (static_cast<float>((H >> 16) & 0xFFFF) / 65535.0f - 0.5f) * 10.0f;
				const float Yaw = static_cast<float>((H >> 32) & 0xFFFF) / 65535.0f * 360.0f;
				// Pieces sit in a 2 x 2 square, then pile on top of it.
				const int32 Slot = P % 4;
				const float OffsetX = Pieces > 1 ? ((Slot & 1) ? 17.0f : -17.0f) : 0.0f;
				const float OffsetY = Pieces > 1 ? ((Slot & 2) ? 15.0f : -15.0f) : 0.0f;
				const float Lift = static_cast<float>(P / 4) * DefLook->LayerCm;
				const FVector Location(View.x * 100.0f + OffsetX + J1, View.y * 100.0f + OffsetY + J2, View.z * 100.0f + Lift);
				Add(ItemLook, BldFixed, Mesh, false, true, false, FTransform(FRotator(0.0, Yaw, 0.0), Location));
			}
			continue;
		}

		// A building, a blueprint or a frame: standing on the lowest ground under its footprint, so a slope never leaves a gap
		// (upstairs, on the floor 3 m a level above that ground).
		const int32 OriginX = FMath::RoundToInt32(View.x - static_cast<float>(View.sizeX) * 0.5f);
		const int32 OriginY = FMath::RoundToInt32(View.y - static_cast<float>(View.sizeY) * 0.5f);
		float Base = View.z;
		for (int32 Y = OriginY; Y <= OriginY + View.sizeY && !DefLook->bDeck; ++Y)
		{
			for (int32 X = OriginX; X <= OriginX + View.sizeX; ++X)
			{
				Base = FMath::Min(Base, Map.groundHeight(colony::CellPos{FMath::Clamp(X, 0, Map.width() - 1), FMath::Clamp(Y, 0, Map.height() - 1), Level}));
			}
		}
		const bool bDoorBuilt = DefLook->bDoor && View.stage == colony::ConstructionStage::Built;
		const int32 Link = ScanLinks.IsValidIndex(ThingIndex) ? ScanLinks[ThingIndex] : -1;
		// A linked building's arms point at its neighbours on the map, so it is never turned.
		const float Yaw = bDoorBuilt ? (BldDoorAlongY(Map, View) ? 90.0f : 0.0f) : (Link >= 0 ? 0.0f : BldYawOf(View.rotation));
		FVector Location(View.x * 100.0f, View.y * 100.0f, Base * 100.0f);
		// A finished wall's window (docs/design/windows.md); plans and frames are drawn whole.
		const int32 Window = View.stage == colony::ConstructionStage::Built ? static_cast<int32>(View.window) : 0;
		const int32 LookId = GetLook(View.def, View.stuff, Link, Window);
		if (LookId < 0)
		{
			continue;
		}
		const FLook Look = Looks[LookId];
		FTransform Placed(FRotator(0.0, Yaw, 0.0), Location);
		if (DefLook->bConform && Level == 0)
		{
			// Sandbags and the like lean with the slope (docs/design/earthworks.md): the plane through the footprint's four
			// outer corners, centred at their mean height.
			const auto H = [&Map, Level](int32 X, int32 Y) {
				return Map.groundHeight(colony::CellPos{FMath::Clamp(X, 0, Map.width() - 1), FMath::Clamp(Y, 0, Map.height() - 1), Level});
			};
			const int32 X0 = OriginX;
			const int32 X1 = OriginX + View.sizeX;
			const int32 Y0 = OriginY;
			const int32 Y1 = OriginY + View.sizeY;
			const float H00 = H(X0, Y0);
			const float H10 = H(X1, Y0);
			const float H01 = H(X0, Y1);
			const float H11 = H(X1, Y1);
			const float SlopeX = ((H10 + H11) - (H00 + H01)) * 0.5f / FMath::Max(static_cast<float>(View.sizeX), 1.0f);
			const float SlopeY = ((H01 + H11) - (H00 + H10)) * 0.5f / FMath::Max(static_cast<float>(View.sizeY), 1.0f);
			const FVector Up = FVector(-SlopeX, -SlopeY, 1.0).GetSafeNormal();
			const FVector Forward = FRotator(0.0, Yaw, 0.0).RotateVector(FVector::XAxisVector);
			Location.Z = (H00 + H10 + H01 + H11) * 25.0f;
			Placed = FTransform(FRotationMatrix::MakeFromZX(Up, Forward).Rotator(), Location);
		}
		switch (View.stage)
		{
			case colony::ConstructionStage::Built:
				if (Look.Fixed)
				{
					// Cracked and grimy as it loses hit points (the materials' damage look, -run=WetLook).
					Add(LookId, BldFixed, Look.Fixed, false, false, true, Placed, BldDamageOf(View));
				}
				if (Look.Glass)
				{
					// No shadow, so the sun comes in through it.
					Add(LookId, BldGlass, Look.Glass, false, false, false, Placed);
				}
				if (Look.Sliding)
				{
					const int32 Index = Add(LookId, BldSliding, Look.Sliding, false, false, true, Placed, BldDamageOf(View));
					Pending.Add({static_cast<uint32>(View.id), LookId * BldVariants + BldSliding, Index, Placed, FRotator(0.0, Yaw, 0.0).RotateVector(FVector::XAxisVector), DefLook->SlideCm});
				}
				if (DefLook->RotorMesh && Look.Fixed == DefLook->ArtMesh)
				{
					const FTransform Hub = FTransform(FQuat::Identity, DefLook->RotorAtCm) * Placed;
					const int32 Index = Add(LookId, BldRotor, DefLook->RotorMesh, false, false, true, Hub, BldDamageOf(View));
					PendingRotors.Add({static_cast<uint32>(View.id), LookId * BldVariants + BldRotor, Index, Hub,
						Placed.TransformVectorNoScale(DefLook->RotorAxis).GetSafeNormal(), DefLook->RotorRpm, DefLook->RotorFullWatts});
				}
				break;
			case colony::ConstructionStage::Frame:
			{
				// The part-built building inside the ghost of the whole.
				FTransform Part = Placed;
				Part.SetScale3D(FVector(1.0, 1.0, BldFrameScale(View)));
				if (Look.All)
				{
					Add(LookId, BldAll, Look.All, false, false, true, Part);
					Add(LookId, BldGhost, Look.All, true, false, false, Placed);
				}
				break;
			}
			case colony::ConstructionStage::Blueprint:
				if (Look.All)
				{
					Add(LookId, BldGhost, Look.All, true, false, false, Placed);
				}
				break;
		}
	}

	// Put the new instances in the chunk's components, making components for kinds the chunk has not had yet.
	for (const colony::Entity Door : Chunk.Doors)
	{
		DoorSlots.Remove(static_cast<uint32>(Door));
	}
	Chunk.Doors.Reset();
	// A rotor keeps its angle and speed through a rebuild of its chunk, so it does not jump back.
	TMap<uint32, FRotorSlot> OldRotors;
	for (const colony::Entity Rotor : Chunk.Rotors)
	{
		FRotorSlot Old;
		if (RotorSlots.RemoveAndCopyValue(static_cast<uint32>(Rotor), Old))
		{
			OldRotors.Add(static_cast<uint32>(Rotor), Old);
		}
	}
	Chunk.Rotors.Reset();
	Stats.Instances -= Chunk.Instances;
	int32 Total = 0;
	for (TPair<int32, UInstancedStaticMeshComponent*>& Pair : Chunk.Components)
	{
		if (Pair.Value && Pair.Value->GetInstanceCount() > 0)
		{
			--Stats.Components;
			Pair.Value->ClearInstances();
		}
	}
	for (TPair<int32, FBldBatch>& Pair : Batches)
	{
		FBldBatch& Batch = Pair.Value;
		UInstancedStaticMeshComponent* Component = GetComponent(Chunk, Pair.Key, Batch.Mesh, Batch.bGhost, Batch.bItem, Batch.bShadow);
		const int32 First = Component->GetInstanceCount();
		Component->AddInstances(Batch.Transforms, false, false, false);
		if (Component->NumCustomDataFloats > WetnessShared::DamageDataIndex)
		{
			for (int32 I = 0; I < Batch.Damage.Num(); ++I)
			{
				if (Batch.Damage[I] > 0.0f)
				{
					Component->SetCustomDataValue(First + I, WetnessShared::DamageDataIndex, Batch.Damage[I], false);
				}
			}
			Component->MarkRenderStateDirty();
		}
		++Stats.Components;
		Total += Batch.Transforms.Num();
	}
	for (const FPendingDoor& Door : Pending)
	{
		FDoorSlot Slot;
		Slot.Component = Chunk.Components.FindRef(Door.Key);
		Slot.Index = Door.Index;
		Slot.Base = Door.Base;
		Slot.Direction = Door.Direction;
		Slot.SlideCm = Door.SlideCm;
		Slot.Shown = 0.0f;
		DoorSlots.Add(Door.Id, Slot);
		Chunk.Doors.Add(static_cast<colony::Entity>(Door.Id));
	}
	for (const FPendingRotor& Rotor : PendingRotors)
	{
		FRotorSlot Slot;
		if (const FRotorSlot* Old = OldRotors.Find(Rotor.Id))
		{
			Slot.Rpm = Old->Rpm;
			Slot.AngleDeg = Old->AngleDeg;
		}
		Slot.Component = Chunk.Components.FindRef(Rotor.Key);
		Slot.Index = Rotor.Index;
		Slot.Base = Rotor.Base;
		Slot.Axis = Rotor.Axis;
		Slot.MaxRpm = Rotor.MaxRpm;
		Slot.FullWatts = Rotor.FullWatts;
		if (Slot.Component && Slot.AngleDeg != 0.0f)
		{
			FTransform Turned = Slot.Base;
			Turned.SetRotation(FQuat(Slot.Axis, FMath::DegreesToRadians(Slot.AngleDeg)) * Slot.Base.GetRotation());
			Slot.Component->UpdateInstanceTransform(Slot.Index, Turned, false, true, true);
		}
		RotorSlots.Add(Rotor.Id, Slot);
		Chunk.Rotors.Add(static_cast<colony::Entity>(Rotor.Id));
	}
	Chunk.Instances = Total;
	Stats.Instances += Total;
	++Stats.ChunkRebuilds;
	Stats.SlowestChunkMs = FMath::Max(Stats.SlowestChunkMs, (FPlatformTime::Seconds() - Start) * 1000.0);
}

void UBuildingSubsystem::AnimateDoors()
{
	USimSubsystem* Sim = Bridge.Get();
	if (!Sim || DoorSlots.IsEmpty())
	{
		return;
	}
	const colony::Snapshot& Current = Sim->GetCurrentSnapshot();
	const colony::Snapshot& Previous = Sim->GetPreviousSnapshot();
	const float Alpha = Sim->GetBlendAlpha();
	TSet<UInstancedStaticMeshComponent*> Touched;
	for (TPair<uint32, FDoorSlot>& Pair : DoorSlots)
	{
		FDoorSlot& Slot = Pair.Value;
		const colony::Entity Id = static_cast<colony::Entity>(Pair.Key);
		const colony::ThingView* Now = Current.find(Id);
		if (!Now || !Slot.Component)
		{
			continue;
		}
		const colony::ThingView* Before = Previous.find(Id);
		const float Open = FMath::Clamp(Before ? FMath::Lerp(Before->doorOpen, Now->doorOpen, Alpha) : Now->doorOpen, 0.0f, 1.0f);
		if (FMath::Abs(Open - Slot.Shown) < 0.002f)
		{
			continue;
		}
		Slot.Shown = Open;
		FTransform Moved = Slot.Base;
		Moved.AddToTranslation(Slot.Direction * (Open * Slot.SlideCm));
		Slot.Component->UpdateInstanceTransform(Slot.Index, Moved, false, false, true);
		Touched.Add(Slot.Component);
	}
	for (UInstancedStaticMeshComponent* Component : Touched)
	{
		Component->MarkRenderStateDirty();
	}
}

void UBuildingSubsystem::AnimateRotors(float DeltaTime)
{
	USimSubsystem* Sim = Bridge.Get();
	if (!Sim || RotorSlots.IsEmpty() || Sim->GetSpeed() <= 0)
	{
		return;
	}
	const colony::Snapshot& Current = Sim->GetCurrentSnapshot();
	// The wind itself, for a turbine on no grid: it makes no power then (the sim counts power only in a net), but its
	// blades still turn in the wind.
	const float Wind = Sim->GetSim() ? FMath::Clamp(static_cast<float>(Sim->GetSim()->weather().wind()), 0.0f, 1.0f) : 0.5f;
	// Real time, not game time: at 1x a game second passes in a thirtieth of a real one, and blades turning thirty
	// times too fast would blur. Speed eases over a few seconds, the way a rotor's weight spins up and down.
	const float Ease = FMath::Clamp(DeltaTime / 4.0f, 0.0f, 1.0f);
	TSet<UInstancedStaticMeshComponent*> Touched;
	for (TPair<uint32, FRotorSlot>& Pair : RotorSlots)
	{
		FRotorSlot& Slot = Pair.Value;
		const colony::ThingView* Now = Current.find(static_cast<colony::Entity>(Pair.Key));
		if (!Now || !Slot.Component)
		{
			continue;
		}
		// The share of full power it makes now (wind, and how open the ground round it is); on no grid, the wind. Below 2%
		// it stands still; above, a slow turn rising to full speed, so a light breeze still shows.
		const float Share = Now->powerNet == 0 ? Wind : Slot.FullWatts > 0.0f ? FMath::Clamp(Now->powerWatts / Slot.FullWatts, 0.0f, 1.0f) : 1.0f;
		const float Target = Share > 0.02f ? Slot.MaxRpm * (0.3f + 0.7f * Share) : 0.0f;
		Slot.Rpm += (Target - Slot.Rpm) * Ease;
		if (Slot.Rpm < 0.01f)
		{
			continue;
		}
		Slot.AngleDeg = FMath::Fmod(Slot.AngleDeg + Slot.Rpm * 6.0f * DeltaTime, 360.0f);
		FTransform Turned = Slot.Base;
		Turned.SetRotation(FQuat(Slot.Axis, FMath::DegreesToRadians(Slot.AngleDeg)) * Slot.Base.GetRotation());
		Slot.Component->UpdateInstanceTransform(Slot.Index, Turned, false, false, true);
		Touched.Add(Slot.Component);
	}
	for (UInstancedStaticMeshComponent* Component : Touched)
	{
		Component->MarkRenderStateDirty();
	}
}

void UBuildingSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Sim = Bridge.Get();
	if (!Sim || !Sim->GetSim() || GBuildingsEnable.GetValueOnGameThread() == 0)
	{
		if (bBuilt || DrawActor)
		{
			Teardown();
		}
		return;
	}
	if (!bBuilt || BuiltGeneration != Sim->GetSimGeneration() || BuiltSim != Sim->GetSim())
	{
		RebuildAll();
	}
	else if (Sim->GetCurrentSnapshot().gameSeconds != LastSeconds)
	{
		Refresh();
	}
	if (Sim->GetWorldOrigin() != BuiltOrigin && DrawActor)
	{
		BuiltOrigin = Sim->GetWorldOrigin();
		DrawActor->SetActorLocation(BuiltOrigin);
	}
	ApplyCut(false);
	AnimateDoors();
	AnimateRotors(DeltaTime);
}
