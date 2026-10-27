#include "SimPicking.h"

#include "BuildingSubsystem.h"
#include "Engine/World.h"
#include "LevelsSubsystem.h"
#include "SimSubsystem.h"
#include "TerrainDraw.h"

#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"

namespace
{
	// Heights in cm of the click boxes. A building is as tall as it is drawn; this is for one that has no look.
	constexpr double SimPickPawnHeight = 190.0;
	constexpr double SimPickPawnHalfWidth = 45.0;
	constexpr double SimPickItemHeight = 40.0;
	constexpr double SimPickItemHalfWidth = 35.0;
	constexpr double SimPickBuildingHeight = 250.0;
	// The ground march never goes further than this along the ray (a 1 km map is 100,000 cm wide).
	constexpr double SimPickMaxDistance = 400000.0;

	// Slab test. The nearest entry distance along the ray (0 if the ray starts inside).
	bool SimPickRayBox(const FVector& Origin, const FVector& Direction, const FBox& Box, double& OutDistance)
	{
		double Near = 0.0;
		double Far = SimPickMaxDistance;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (FMath::Abs(Direction[Axis]) < 1e-9)
			{
				if (Origin[Axis] < Box.Min[Axis] || Origin[Axis] > Box.Max[Axis])
				{
					return false;
				}
				continue;
			}
			double A = (Box.Min[Axis] - Origin[Axis]) / Direction[Axis];
			double B = (Box.Max[Axis] - Origin[Axis]) / Direction[Axis];
			if (A > B)
			{
				Swap(A, B);
			}
			Near = FMath::Max(Near, A);
			Far = FMath::Min(Far, B);
			if (Near > Far)
			{
				return false;
			}
		}
		OutDistance = Near;
		return true;
	}

	// How far above the surface a world point is (negative: below it), or false off the map. The surface is level `Level`'s
	// height, or (Level -1) the highest upper floor at most TopLevel over the point, else the ground; OutLevel says which.
	bool SimPickHeightAbove(const USimSubsystem& Bridge, const FVector& Point, double& OutAbove, int32 Level, int32 TopLevel, int32& OutLevel)
	{
		const colony::Map& Map = Bridge.GetSim()->map();
		const FVector Local = (Point - Bridge.GetWorldOrigin()) / 100.0;
		if (Local.X < 0.0 || Local.Y < 0.0 || Local.X >= Map.width() || Local.Y >= Map.height())
		{
			return false;
		}
		OutLevel = FMath::Max(Level, 0);
		if (Level < 0)
		{
			for (int32 Upper = FMath::Min(TopLevel, colony::Map::kLevels - 1); Upper > 0; --Upper)
			{
				if (Map.upperFloor(colony::CellPos{FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), Upper}))
				{
					OutLevel = Upper;
					break;
				}
			}
		}
		// The ground has the rock block where the cell is rock (TerrainDraw.h); a level above it is 3 m a level over the ground.
		const double Bonus = OutLevel == 0 ? TerrainDraw::SurfaceBonusCm(Map, Local.X, Local.Y) : 0.0;
		OutAbove = Point.Z - Bridge.SimToWorld(static_cast<float>(Local.X), static_cast<float>(Local.Y), OutLevel).Z - Bonus;
		return true;
	}
}

namespace SimPicking
{
	bool ThingBox(const USimSubsystem& Bridge, const colony::ThingView& View, FBox& OutBox)
	{
		FVector Location;
		if (!Bridge.GetBlendedLocation(View.id, Location))
		{
			return false;
		}
		double HalfX = SimPickItemHalfWidth;
		double HalfY = SimPickItemHalfWidth;
		double Height = SimPickItemHeight;
		switch (View.category)
		{
			case colony::ThingCategory::Pawn:
				HalfX = HalfY = SimPickPawnHalfWidth;
				Height = SimPickPawnHeight;
				break;
			case colony::ThingCategory::Building:
				HalfX = View.sizeX * 50.0;
				HalfY = View.sizeY * 50.0;
				// As tall as it is drawn (BuildingSubsystem), else a default.
				Height = SimPickBuildingHeight;
				if (const UWorld* World = Bridge.GetWorld())
				{
					if (const UBuildingSubsystem* Drawn = World->GetSubsystem<UBuildingSubsystem>())
					{
						const float DrawnHeight = Drawn->GetDrawnHeightCm(View.def);
						Height = DrawnHeight > 0.0f ? DrawnHeight : Height;
					}
				}
				break;
			case colony::ThingCategory::Item:
				break;
		}
		OutBox = FBox(Location - FVector(HalfX, HalfY, 0.0), Location + FVector(HalfX, HalfY, Height));
		return true;
	}

	bool RayHitGround(const USimSubsystem& Bridge, const FVector& Origin, const FVector& Direction, double& OutDistance, int32 Level, int32 TopLevel, int32* OutLevel)
	{
		if (!Bridge.GetSim())
		{
			return false;
		}
		double PreviousT = 0.0;
		double PreviousAbove = 0.0;
		bool bHavePrevious = false;
		int32 HitLevel = 0;
		for (double T = 0.0; T <= SimPickMaxDistance;)
		{
			double Above = 0.0;
			if (SimPickHeightAbove(Bridge, Origin + Direction * T, Above, Level, TopLevel, HitLevel))
			{
				if (bHavePrevious && PreviousAbove > 0.0 && Above <= 0.0)
				{
					// Crossed the surface between the last two samples: narrow it down.
					double Low = PreviousT;
					double High = T;
					for (int32 Iteration = 0; Iteration < 16; ++Iteration)
					{
						const double Mid = 0.5 * (Low + High);
						double MidAbove = 0.0;
						int32 MidLevel = 0;
						if (SimPickHeightAbove(Bridge, Origin + Direction * Mid, MidAbove, Level, TopLevel, MidLevel) && MidAbove <= 0.0)
						{
							High = Mid;
						}
						else
						{
							Low = Mid;
						}
					}
					OutDistance = High;
					if (OutLevel)
					{
						double Unused = 0.0;
						SimPickHeightAbove(Bridge, Origin + Direction * High, Unused, Level, TopLevel, *OutLevel);
					}
					return true;
				}
				PreviousT = T;
				PreviousAbove = Above;
				bHavePrevious = true;
			}
			else
			{
				bHavePrevious = false;
			}
			// Fine steps close up, coarser far away: about half a cell per step at 100 m.
			T += FMath::Clamp(T * 0.005, 20.0, 300.0);
		}
		return false;
	}

	FSimPick PickGroundFromRay(const USimSubsystem& Bridge, const FVector& Origin, const FVector& InDirection, int32 Level)
	{
		FSimPick Result;
		FVector Direction = InDirection;
		double GroundT = 0.0;
		Level = FMath::Clamp(Level, 0, colony::Map::kLevels - 1);
		if (!Bridge.GetSim() || !Direction.Normalize() || !RayHitGround(Bridge, Origin, Direction, GroundT, Level, Level))
		{
			return Result;
		}
		Result.Location = Origin + Direction * GroundT;
		const FVector Local = (Result.Location - Bridge.GetWorldOrigin()) / 100.0;
		Result.Cell = colony::CellPos{FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), Level};
		Result.Kind = Bridge.GetSim()->map().inBounds(Result.Cell) ? ESimPickKind::Cell : ESimPickKind::None;
		return Result;
	}

	FSimPick PickFromRay(const USimSubsystem& Bridge, const FVector& Origin, const FVector& InDirection)
	{
		FSimPick Result;
		FVector Direction = InDirection;
		if (!Bridge.GetSim() || !Direction.Normalize())
		{
			return Result;
		}

		const int32 TopLevel = ULevelsSubsystem::TopShownLevel(Bridge.GetWorld());
		double BestThingT = SimPickMaxDistance;
		for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
		{
			FBox Box;
			double T = 0.0;
			if (View.level <= TopLevel && SimPicking::ThingBox(Bridge, View, Box) && SimPickRayBox(Origin, Direction, Box, T) && T < BestThingT)
			{
				BestThingT = T;
				Result.Kind = ESimPickKind::Thing;
				Result.Thing = View.id;
			}
		}

		double GroundT = 0.0;
		int32 GroundLevel = 0;
		const bool bGround = RayHitGround(Bridge, Origin, Direction, GroundT, -1, TopLevel, &GroundLevel);
		if (Result.Kind == ESimPickKind::Thing && (!bGround || BestThingT <= GroundT))
		{
			Result.Location = Origin + Direction * BestThingT;
			return Result;
		}

		Result = FSimPick();
		if (bGround)
		{
			Result.Location = Origin + Direction * GroundT;
			const FVector Local = (Result.Location - Bridge.GetWorldOrigin()) / 100.0;
			Result.Cell = colony::CellPos{FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), GroundLevel};
			if (Bridge.GetSim()->map().inBounds(Result.Cell))
			{
				Result.Kind = ESimPickKind::Cell;
			}
		}
		return Result;
	}
}
