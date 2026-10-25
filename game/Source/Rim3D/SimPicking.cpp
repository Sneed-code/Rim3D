#include "SimPicking.h"

#include "BuildingSubsystem.h"
#include "Engine/World.h"
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

	// How far above the sim's ground a world point is (negative: below it), or false off the map.
	bool SimPickHeightAbove(const USimSubsystem& Bridge, const FVector& Point, double& OutAbove)
	{
		const colony::Map& Map = Bridge.GetSim()->map();
		const FVector Local = (Point - Bridge.GetWorldOrigin()) / 100.0;
		if (Local.X < 0.0 || Local.Y < 0.0 || Local.X >= Map.width() || Local.Y >= Map.height())
		{
			return false;
		}
		// The surface is the ground, plus the rock block where the cell is rock (TerrainDraw.h).
		OutAbove = Point.Z - Bridge.SimToWorld(static_cast<float>(Local.X), static_cast<float>(Local.Y), 0).Z - TerrainDraw::SurfaceBonusCm(Map, Local.X, Local.Y);
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

	bool RayHitGround(const USimSubsystem& Bridge, const FVector& Origin, const FVector& Direction, double& OutDistance)
	{
		if (!Bridge.GetSim())
		{
			return false;
		}
		double PreviousT = 0.0;
		double PreviousAbove = 0.0;
		bool bHavePrevious = false;
		for (double T = 0.0; T <= SimPickMaxDistance;)
		{
			double Above = 0.0;
			if (SimPickHeightAbove(Bridge, Origin + Direction * T, Above))
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
						if (SimPickHeightAbove(Bridge, Origin + Direction * Mid, MidAbove) && MidAbove <= 0.0)
						{
							High = Mid;
						}
						else
						{
							Low = Mid;
						}
					}
					OutDistance = High;
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

	FSimPick PickGroundFromRay(const USimSubsystem& Bridge, const FVector& Origin, const FVector& InDirection)
	{
		FSimPick Result;
		FVector Direction = InDirection;
		double GroundT = 0.0;
		if (!Bridge.GetSim() || !Direction.Normalize() || !RayHitGround(Bridge, Origin, Direction, GroundT))
		{
			return Result;
		}
		Result.Location = Origin + Direction * GroundT;
		const FVector Local = (Result.Location - Bridge.GetWorldOrigin()) / 100.0;
		Result.Cell = colony::CellPos{FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), 0};
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

		double BestThingT = SimPickMaxDistance;
		for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
		{
			FBox Box;
			double T = 0.0;
			if (SimPicking::ThingBox(Bridge, View, Box) && SimPickRayBox(Origin, Direction, Box, T) && T < BestThingT)
			{
				BestThingT = T;
				Result.Kind = ESimPickKind::Thing;
				Result.Thing = View.id;
			}
		}

		double GroundT = 0.0;
		const bool bGround = RayHitGround(Bridge, Origin, Direction, GroundT);
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
			Result.Cell = colony::CellPos{FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), 0};
			if (Bridge.GetSim()->map().inBounds(Result.Cell))
			{
				Result.Kind = ESimPickKind::Cell;
			}
		}
		return Result;
	}
}
