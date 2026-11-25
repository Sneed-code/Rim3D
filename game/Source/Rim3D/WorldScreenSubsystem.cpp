#include "WorldScreenSubsystem.h"

#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "TextureResource.h"

#include <imgui.h>

#include "ColonyHudStyle.h"

#include "OverseerSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
	FAutoConsoleCommandWithWorldAndArgs GWorldScreenCommand(
		TEXT("World.Screen"), TEXT("World.Screen 0|1 closes or opens the world map screen (no argument toggles it)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UWorldScreenSubsystem* Screen = World ? World->GetSubsystem<UWorldScreenSubsystem>() : nullptr)
			{
				Screen->SetOpen(Args.Num() > 0 ? FCString::Atoi(*Args[0]) != 0 : !Screen->IsOpen());
			}
		}));

	std::string Lower(const std::string& Text)
	{
		std::string Out;
		for (const char C : Text)
		{
			if (C != ' ' && C != '_')
			{
				Out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(C))));
			}
		}
		return Out;
	}

	bool Has(const std::string& Name, const char* Part)
	{
		return Name.find(Part) != std::string::npos;
	}

	// ---- Hexagons ----------------------------------------------------------------------------------------------
	// Pointy-top hexagons one unit wide; row y's middles sit kWmRowStep apart, odd rows shifted half a unit right.
	constexpr float kWmRowStep = 0.8660254f;  // sqrt(3) / 2
	constexpr float kWmRadius = 0.5773503f;   // corner distance, 1 / sqrt(3)

	struct FWmHex
	{
		int32 X = -1;
		int32 Y = -1;
		float Dx = 0.0f;  // the point from the hexagon's middle, in hexagon widths
		float Dy = 0.0f;
	};

	float WmCentreX(int32 X, int32 Y)
	{
		return static_cast<float>(X) + 0.5f + ((Y & 1) != 0 ? 0.5f : 0.0f);
	}

	float WmCentreY(int32 Y)
	{
		return kWmRadius + static_cast<float>(Y) * kWmRowStep;
	}

	/** The hexagon holding map point (U, V) (hexagon widths from the map's top left); false off the map. */
	bool WmHexAt(int32 Width, int32 Height, float U, float V, FWmHex& Out)
	{
		const int32 Row0 = static_cast<int32>(std::floor((V - kWmRadius) / kWmRowStep));
		float Best = 1e9f;
		for (int32 Y = Row0; Y <= Row0 + 1; ++Y)
		{
			if (Y < 0 || Y >= Height)
			{
				continue;
			}
			const int32 X = static_cast<int32>(std::floor(U - ((Y & 1) != 0 ? 0.5f : 0.0f)));
			if (X < 0 || X >= Width)
			{
				continue;
			}
			const float Dx = U - WmCentreX(X, Y);
			const float Dy = V - WmCentreY(Y);
			const float D2 = Dx * Dx + Dy * Dy;
			if (D2 < Best)
			{
				Best = D2;
				Out.X = X;
				Out.Y = Y;
				Out.Dx = Dx;
				Out.Dy = Dy;
			}
		}
		if (Best > 1e8f)
		{
			return false;
		}
		// The nearest middle's hexagon holds every point nearer to it than to any other, except past the map's edge.
		const float Ax = std::fabs(Out.Dx);
		const float Ay = std::fabs(Out.Dy);
		return Ax <= 0.5f + 1e-4f && Ay <= kWmRadius * (1.0f - Ax) + 1e-4f;
	}

	/** The neighbour across side Side of hexagon (X, Y): 0 east, then clockwise on screen (y down) every 60 degrees:
	 *  1 south-east, 2 south-west, 3 west, 4 north-west, 5 north-east. -1 past a pole; east and west wrap. */
	int32 WmAcross(int32 Width, int32 Height, int32 X, int32 Y, int32 Side)
	{
		const int32 Odd = Y & 1;
		int32 Nx = X;
		int32 Ny = Y;
		switch (Side)
		{
		case 0: Nx = X + 1; break;
		case 1: Nx = X + Odd; Ny = Y + 1; break;
		case 2: Nx = X - 1 + Odd; Ny = Y + 1; break;
		case 3: Nx = X - 1; break;
		case 4: Nx = X - 1 + Odd; Ny = Y - 1; break;
		default: Nx = X + Odd; Ny = Y - 1; break;
		}
		if (Ny < 0 || Ny >= Height)
		{
			return -1;
		}
		Nx = ((Nx % Width) + Width) % Width;
		return Ny * Width + Nx;
	}

	/** How far (hexagon widths) a point Dx, Dy from a middle is inside its hexagon, and which side is nearest. */
	float WmEdgeDistance(float Dx, float Dy, int32& OutSide)
	{
		static const float Cos[6] = {1.0f, 0.5f, -0.5f, -1.0f, -0.5f, 0.5f};
		static const float Sin[6] = {0.0f, 0.8660254f, 0.8660254f, 0.0f, -0.8660254f, -0.8660254f};
		float Most = -1e9f;
		OutSide = 0;
		for (int32 Side = 0; Side < 6; ++Side)
		{
			const float Along = Dx * Cos[Side] + Dy * Sin[Side];
			if (Along > Most)
			{
				Most = Along;
				OutSide = Side;
			}
		}
		return 0.5f - Most;
	}

	/** The six corners of a hexagon on screen, its middle at C, Size wide (pointy top, clockwise from the top). */
	void WmCorners(const ImVec2& C, float Size, ImVec2 (&Out)[6])
	{
		const float R = kWmRadius * Size;
		const float Hw = 0.5f * Size;
		Out[0] = ImVec2(C.x, C.y - R);
		Out[1] = ImVec2(C.x + Hw, C.y - R * 0.5f);
		Out[2] = ImVec2(C.x + Hw, C.y + R * 0.5f);
		Out[3] = ImVec2(C.x, C.y + R);
		Out[4] = ImVec2(C.x - Hw, C.y + R * 0.5f);
		Out[5] = ImVec2(C.x - Hw, C.y - R * 0.5f);
	}

	// ---- Painting the map picture ---------------------------------------------------------------------------------

	/** A smooth value noise in the plane (0..1), from a seeded hash; fractal sums of it. */
	float WmHash(int32 X, int32 Y, uint32 Seed)
	{
		uint32 H = static_cast<uint32>(X) * 374761393u + static_cast<uint32>(Y) * 668265263u + Seed * 2246822519u;
		H = (H ^ (H >> 13)) * 1274126177u;
		H ^= H >> 16;
		return static_cast<float>(H & 0xFFFFFFu) / 16777215.0f;
	}

	float WmValue(float X, float Y, uint32 Seed)
	{
		const float Fx = std::floor(X);
		const float Fy = std::floor(Y);
		const int32 Ix = static_cast<int32>(Fx);
		const int32 Iy = static_cast<int32>(Fy);
		float Tx = X - Fx;
		float Ty = Y - Fy;
		Tx = Tx * Tx * (3.0f - 2.0f * Tx);
		Ty = Ty * Ty * (3.0f - 2.0f * Ty);
		const float A = WmHash(Ix, Iy, Seed);
		const float B = WmHash(Ix + 1, Iy, Seed);
		const float C = WmHash(Ix, Iy + 1, Seed);
		const float D = WmHash(Ix + 1, Iy + 1, Seed);
		return FMath::Lerp(FMath::Lerp(A, B, Tx), FMath::Lerp(C, D, Tx), Ty);
	}

	float WmFbm(float X, float Y, uint32 Seed, int32 Octaves)
	{
		float Sum = 0.0f;
		float Amp = 1.0f;
		float Norm = 0.0f;
		for (int32 O = 0; O < Octaves; ++O)
		{
			Sum += Amp * WmValue(X, Y, Seed + static_cast<uint32>(O) * 101u);
			Norm += Amp;
			Amp *= 0.5f;
			X = X * 2.03f + 17.1f;
			Y = Y * 2.03f - 9.3f;
		}
		return Sum / Norm;
	}

	/** Sharp crests for mountains (0..1). */
	float WmRidged(float X, float Y, uint32 Seed, int32 Octaves)
	{
		float Sum = 0.0f;
		float Amp = 1.0f;
		float Norm = 0.0f;
		float Weight = 1.0f;
		for (int32 O = 0; O < Octaves; ++O)
		{
			float N = 1.0f - std::fabs(WmValue(X, Y, Seed + static_cast<uint32>(O) * 131u) * 2.0f - 1.0f);
			N = N * N * Weight;
			Weight = FMath::Clamp(N * 1.8f, 0.0f, 1.0f);
			Sum += Amp * N;
			Norm += Amp;
			Amp *= 0.5f;
			X = X * 2.1f + 3.7f;
			Y = Y * 2.1f - 5.1f;
		}
		return Sum / Norm;
	}

	float WmSmooth(float E0, float E1, float X)
	{
		const float T = FMath::Clamp((X - E0) / (E1 - E0), 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	/** What a tile's ground looks like up close. */
	enum class EWmGround : uint8 { Plain, Forest, Jungle, Desert, Ice, Swamp, Tundra };

	struct FWmTileLook
	{
		FLinearColor Colour = FLinearColor::Gray;  // sRGB 0..1
		EWmGround Ground = EWmGround::Plain;
		bool bWater = false;
		bool bDim = false;
	};

	/** A biome's colour (sRGB 0..255) and ground; unknown biomes a grey plain. */
	void WmBiomeLook(const std::string& DefName, FLinearColor& Colour, EWmGround& Ground)
	{
		const std::string N = Lower(DefName);
		const auto Rgb = [&Colour](int32 R, int32 G, int32 B) { Colour = FLinearColor(R / 255.0f, G / 255.0f, B / 255.0f, 1.0f); };
		Ground = EWmGround::Plain;
		if (Has(N, "seaice")) { Rgb(196, 218, 232); Ground = EWmGround::Ice; }
		else if (Has(N, "icesheet")) { Rgb(232, 238, 245); Ground = EWmGround::Ice; }
		else if (Has(N, "tundra")) { Rgb(146, 146, 118); Ground = EWmGround::Tundra; }
		else if (Has(N, "boreal")) { Rgb(62, 94, 72); Ground = EWmGround::Forest; }
		else if (Has(N, "coldbog")) { Rgb(92, 106, 86); Ground = EWmGround::Swamp; }
		else if (Has(N, "temperateforest")) { Rgb(84, 128, 60); Ground = EWmGround::Forest; }
		else if (Has(N, "temperateswamp")) { Rgb(86, 110, 64); Ground = EWmGround::Swamp; }
		else if (Has(N, "tropicalrain")) { Rgb(38, 110, 46); Ground = EWmGround::Jungle; }
		else if (Has(N, "tropicalswamp")) { Rgb(48, 96, 72); Ground = EWmGround::Swamp; }
		else if (Has(N, "extremedesert")) { Rgb(226, 198, 142); Ground = EWmGround::Desert; }
		else if (Has(N, "desert")) { Rgb(212, 184, 122); Ground = EWmGround::Desert; }
		else if (Has(N, "aridshrub")) { Rgb(170, 152, 94); }
		else { Rgb(140, 140, 140); }
	}

	float WmHills(colony::Hilliness H)
	{
		switch (H)
		{
		case colony::Hilliness::Flat: return 0.0f;
		case colony::Hilliness::SmallHills: return 0.25f;
		case colony::Hilliness::LargeHills: return 0.5f;
		case colony::Hilliness::Mountainous: return 0.78f;
		default: return 1.0f;
		}
	}

	/** A field of per-tile values read smoothly between hexagon middles (along each row, then between rows). */
	float WmSmoothField(const TArray<float>& Field, int32 Width, int32 Height, float U, float V)
	{
		const float RowF = FMath::Clamp((V - kWmRadius) / kWmRowStep, 0.0f, static_cast<float>(Height - 1));
		const int32 R0 = FMath::Min(static_cast<int32>(RowF), Height - 1);
		const int32 R1 = FMath::Min(R0 + 1, Height - 1);
		float T = RowF - static_cast<float>(R0);
		T = T * T * (3.0f - 2.0f * T);
		const auto AlongRow = [&Field, Width, U](int32 Row)
		{
			const float Xf = U - 0.5f - ((Row & 1) != 0 ? 0.5f : 0.0f);
			const float Fx = std::floor(Xf);
			const int32 C0 = ((static_cast<int32>(Fx) % Width) + Width) % Width;
			const int32 C1 = (C0 + 1) % Width;
			float S = Xf - Fx;
			S = S * S * (3.0f - 2.0f * S);
			return FMath::Lerp(Field[Row * Width + C0], Field[Row * Width + C1], S);
		};
		return FMath::Lerp(AlongRow(R0), AlongRow(R1), T);
	}

	/** Halves a BGRA picture, colour weighted by alpha (the map's ragged edge is see-through). */
	TArray<uint8> WmHalve(const TArray<uint8>& Src, int32 Width, int32 Height, int32& OutW, int32& OutH)
	{
		OutW = FMath::Max(1, Width / 2);
		OutH = FMath::Max(1, Height / 2);
		TArray<uint8> Out;
		Out.SetNumUninitialized(OutW * OutH * 4);
		for (int32 Y = 0; Y < OutH; ++Y)
		{
			for (int32 X = 0; X < OutW; ++X)
			{
				float Sum[3] = {0.0f, 0.0f, 0.0f};
				float AlphaSum = 0.0f;
				for (int32 Dy = 0; Dy < 2; ++Dy)
				{
					for (int32 Dx = 0; Dx < 2; ++Dx)
					{
						const uint8* P = &Src[(FMath::Min(Y * 2 + Dy, Height - 1) * Width + FMath::Min(X * 2 + Dx, Width - 1)) * 4];
						const float A = P[3] / 255.0f;
						Sum[0] += P[0] * A;
						Sum[1] += P[1] * A;
						Sum[2] += P[2] * A;
						AlphaSum += A;
					}
				}
				uint8* Q = &Out[(Y * OutW + X) * 4];
				for (int32 C = 0; C < 3; ++C)
				{
					Q[C] = AlphaSum > 0.0f ? static_cast<uint8>(FMath::Clamp(Sum[C] / AlphaSum, 0.0f, 255.0f)) : 0;
				}
				Q[3] = static_cast<uint8>(FMath::Clamp(AlphaSum / 4.0f * 255.0f + 0.5f, 0.0f, 255.0f));
			}
		}
		return Out;
	}

	UTexture2D* WmTexture(const TArray<uint8>& Bgra, int32 Width, int32 Height)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		FTexturePlatformData* Platform = Texture ? Texture->GetPlatformData() : nullptr;
		if (!Platform || Platform->Mips.IsEmpty())
		{
			return nullptr;
		}
		FTexture2DMipMap& Mip = Platform->Mips[0];
		void* Pixels = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Pixels, Bgra.GetData(), FMath::Min<int64>(Bgra.Num(), Mip.BulkData.GetBulkDataSize()));
		Mip.BulkData.Unlock();
		Texture->SRGB = true;
		Texture->Filter = TF_Bilinear;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->NeverStream = true;
		Texture->UpdateResource();
		return Texture;
	}

	ImU32 RoadColor(colony::RoadKind Kind)
	{
		switch (Kind)
		{
		case colony::RoadKind::Dirt: return IM_COL32(150, 100, 50, 255);
		case colony::RoadKind::Stone: return IM_COL32(90, 70, 50, 255);
		default: return IM_COL32(30, 25, 25, 255);
		}
	}

	FString Words(const std::string& DefName)
	{
		// "TemperateForest" -> "temperate forest".
		FString Out;
		for (const char C : DefName)
		{
			if (std::isupper(static_cast<unsigned char>(C)) && !Out.IsEmpty())
			{
				Out.AppendChar(TEXT(' '));
			}
			Out.AppendChar(static_cast<TCHAR>(std::tolower(static_cast<unsigned char>(C))));
		}
		return Out;
	}

	FString HillsWords(colony::Hilliness H)
	{
		switch (H)
		{
		case colony::Hilliness::Flat: return TEXT("flat");
		case colony::Hilliness::SmallHills: return TEXT("small hills");
		case colony::Hilliness::LargeHills: return TEXT("large hills");
		case colony::Hilliness::Mountainous: return TEXT("mountainous");
		default: return TEXT("impassable mountains");
		}
	}

	void WText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}
}

void UWorldScreenSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<USimSubsystem>();
}

bool UWorldScreenSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UWorldScreenSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWorldScreenSubsystem, STATGROUP_Tickables);
}

USimSubsystem* UWorldScreenSubsystem::GetBridge() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<USimSubsystem>() : nullptr;
}

void UWorldScreenSubsystem::SetOpen(bool bNewOpen)
{
	bOpen = bNewOpen;
}

void UWorldScreenSubsystem::SetNewGame(const TArray<FString>& Biomes)
{
	NewGameBiomes = Biomes;
	bNewGame = Biomes.Num() > 0;
	bLanded = false;
	bCancelled = false;
}

bool UWorldScreenSubsystem::ConsumeLanding()
{
	const bool bWas = bLanded;
	bLanded = false;
	return bWas;
}

bool UWorldScreenSubsystem::ConsumeCancel()
{
	const bool bWas = bCancelled;
	bCancelled = false;
	return bWas;
}

bool UWorldScreenSubsystem::NewGameAllows(const colony::World& World, int32 Tile) const
{
	if (!bNewGame)
	{
		return true;
	}
	const colony::WorldTile& T = World.tile(Tile);
	return T.biome != colony::kNoBiome && NewGameBiomes.Contains(FString(UTF8_TO_TCHAR(World.biomeName(T).c_str())));
}

bool UWorldScreenSubsystem::PickRandomSite()
{
	const colony::World* World = GetShownWorld();
	if (!World)
	{
		return false;
	}
	TArray<int32> Good;
	for (int32 Tile = 0; Tile < World->tileCount(); ++Tile)
	{
		if (World->canStartAt(Tile) && NewGameAllows(*World, Tile))
		{
			Good.Add(Tile);
		}
	}
	return Good.Num() > 0 && SelectTile(Good[FMath::RandRange(0, Good.Num() - 1)]);
}

const colony::World* UWorldScreenSubsystem::GetShownWorld() const
{
	const USimSubsystem* Bridge = GetBridge();
	const colony::World* Shown = nullptr;
	if (!bPreview && Bridge && Bridge->GetSim())
	{
		Shown = Bridge->GetSim()->world();
	}
	else if (Preview)
	{
		Shown = &*Preview;
	}
	return Shown && !Shown->empty() ? Shown : nullptr;
}

void UWorldScreenSubsystem::MakeWorld(int32 NewSeed)
{
	const USimSubsystem* Bridge = GetBridge();
	Selected = -1;
	MapZoom = 1.0f;
	MapViewCentre = FVector2D(-1.0, -1.0);
	Seed = NewSeed;
	SeedInput = NewSeed;
	bPreview = true;
	Preview.reset();
	if (Bridge && Bridge->GetDefs())
	{
		Preview = colony::World::generate(*Bridge->GetDefs(), static_cast<uint64>(NewSeed));
	}
}

bool UWorldScreenSubsystem::SelectTile(int32 Tile)
{
	const colony::World* World = GetShownWorld();
	if (!World || Tile < 0 || Tile >= World->tileCount())
	{
		return false;
	}
	Selected = Tile;
	++Stats.TilesPicked;
	return true;
}

int32 UWorldScreenSubsystem::TileAt(const colony::World& World, float MapLeft, float MapTop, float TileSize, float X, float Y)
{
	if (TileSize <= 0.0f || World.empty())
	{
		return -1;
	}
	FWmHex Hex;
	if (!WmHexAt(World.width(), World.height(), (X - MapLeft) / TileSize, (Y - MapTop) / TileSize, Hex))
	{
		return -1;
	}
	return World.index(Hex.X, Hex.Y);
}

FVector2D UWorldScreenSubsystem::TileCentre(const colony::World& World, float MapLeft, float MapTop, float TileSize, int32 Tile)
{
	if (World.empty() || Tile < 0 || Tile >= World.tileCount())
	{
		return FVector2D(MapLeft, MapTop);
	}
	const int32 X = World.tileX(Tile);
	const int32 Y = World.tileY(Tile);
	return FVector2D(MapLeft + WmCentreX(X, Y) * TileSize, MapTop + WmCentreY(Y) * TileSize);
}

FVector2D UWorldScreenSubsystem::MapSizeInTiles(const colony::World& World)
{
	if (World.empty())
	{
		return FVector2D(1.0, 1.0);
	}
	return FVector2D(World.width() + 0.5, 2.0 * kWmRadius + (World.height() - 1) * kWmRowStep);
}

void UWorldScreenSubsystem::BuildMapPicture(const colony::World& World)
{
	FString Key = FString::Printf(TEXT("%llu %d %d %d|"), static_cast<unsigned long long>(World.seed()), World.width(), World.height(), bNewGame ? 1 : 0);
	if (bNewGame)
	{
		Key += FString::Join(NewGameBiomes, TEXT(","));
	}
	if (Key == MapPictureKey && MapPictures.Num() > 0)
	{
		return;
	}
	MapPictureKey = Key;
	MapPictures.Reset();

	// Each tile's look, and the fields read smoothly between hexagon middles: relief (land 0..1 of 4500 m, sea -1..0 of
	// 4000 m), hills (0 flat .. 1 impassable) and the yearly temperature.
	const int32 Width = World.width();
	const int32 Height = World.height();
	const int32 Count = World.tileCount();
	TArray<FWmTileLook> Looks;
	Looks.SetNum(Count);
	TArray<float> Relief;
	TArray<float> Hills;
	TArray<float> Warmth;
	Relief.SetNumZeroed(Count);
	Hills.SetNumZeroed(Count);
	Warmth.SetNumZeroed(Count);
	for (int32 Tile = 0; Tile < Count; ++Tile)
	{
		const colony::WorldTile& T = World.tile(Tile);
		FWmTileLook& Look = Looks[Tile];
		// Open water has no biome; sea ice is water with a biome, drawn as ice.
		Look.bWater = T.biome == colony::kNoBiome;
		if (!Look.bWater)
		{
			WmBiomeLook(World.biomeName(T), Look.Colour, Look.Ground);
		}
		Look.bDim = bNewGame && !T.water && !NewGameAllows(World, Tile);
		Relief[Tile] = T.elevation >= 0 ? FMath::Clamp(static_cast<float>(T.elevation) / 4500.0f, 0.0f, 1.0f)
			: FMath::Clamp(static_cast<float>(T.elevation) / 4000.0f, -1.0f, 0.0f);
		Hills[Tile] = T.water ? 0.0f : WmHills(T.hilliness);
		Warmth[Tile] = T.temperature;
	}

	// Pixels per hexagon width: about 4800 pixels across the map, from 12 to 48 a hexagon, no side over 8192.
	const FVector2D Units = MapSizeInTiles(World);
	int32 PerHex = FMath::Clamp(static_cast<int32>(4800.0 / Units.X), 12, 48);
	while (PerHex > 8 && (Units.X * PerHex > 8192.0 || Units.Y * PerHex > 8192.0))
	{
		--PerHex;
	}
	const int32 PicW = FMath::Max(4, static_cast<int32>(std::ceil(Units.X * PerHex)));
	const int32 PicH = FMath::Max(4, static_cast<int32>(std::ceil(Units.Y * PerHex)));
	const float Step = 1.0f / static_cast<float>(PerHex);
	const uint32 NoiseSeed = static_cast<uint32>(World.seed() & 0xFFFFFFFFu) ^ 0x9E3779B9u;

	// 1. Heights for the shading: the smooth relief plus hills and ridged mountains where the tiles are rough.
	TArray<float> Lift;
	Lift.SetNumZeroed(PicW * PicH);
	ParallelFor(PicH, [&](int32 Py)
	{
		const float V = (static_cast<float>(Py) + 0.5f) * Step;
		for (int32 Px = 0; Px < PicW; ++Px)
		{
			const float U = (static_cast<float>(Px) + 0.5f) * Step;
			const float Base = WmSmoothField(Relief, Width, Height, U, V);
			const float Rough = WmSmoothField(Hills, Width, Height, U, V);
			float H = Base * 0.8f;
			if (Base > 0.0f)
			{
				const float Bumps = WmFbm(U * 1.7f, V * 1.7f, NoiseSeed + 7u, 4) - 0.5f;
				const float Crests = WmRidged(U * 1.1f, V * 1.1f, NoiseSeed + 31u, 5);
				H += Rough * (0.22f * Bumps + 0.55f * Rough * Crests);
			}
			else
			{
				H += 0.02f * (WmFbm(U * 3.0f, V * 3.0f, NoiseSeed + 3u, 3) - 0.5f);  // the sea floor, barely
			}
			Lift[Py * PicW + Px] = H;
		}
	});

	// 2. Colour: the hexagon's biome with its ground's texture, shaded, the sea by depth, coasts, the grid.
	TArray<uint8> Pixels;
	Pixels.SetNumZeroed(PicW * PicH * 4);
	const FVector LightDir = FVector(-1.0, -1.0, 1.25).GetSafeNormal();  // from the top left, as maps are lit
	ParallelFor(PicH, [&](int32 Py)
	{
		const float V = (static_cast<float>(Py) + 0.5f) * Step;
		for (int32 Px = 0; Px < PicW; ++Px)
		{
			const float U = (static_cast<float>(Px) + 0.5f) * Step;
			uint8* Out = &Pixels[(Py * PicW + Px) * 4];
			FWmHex Hex;
			if (!WmHexAt(Width, Height, U, V, Hex))
			{
				continue;  // past the ragged edge: see-through
			}
			const int32 Tile = Hex.Y * Width + Hex.X;
			const FWmTileLook& Look = Looks[Tile];
			int32 Side = 0;
			const float Edge = WmEdgeDistance(Hex.Dx, Hex.Dy, Side);
			const int32 Across = WmAcross(Width, Height, Hex.X, Hex.Y, Side);
			const bool bAcrossWater = Across >= 0 && Looks[Across].bWater;

			// The slope from the heights around this pixel.
			const int32 L = FMath::Max(Px - 1, 0);
			const int32 R = FMath::Min(Px + 1, PicW - 1);
			const int32 Up = FMath::Max(Py - 1, 0);
			const int32 Dn = FMath::Min(Py + 1, PicH - 1);
			const float Gx = (Lift[Py * PicW + R] - Lift[Py * PicW + L]) / (static_cast<float>(R - L) * Step);
			const float Gy = (Lift[Dn * PicW + Px] - Lift[Up * PicW + Px]) / (static_cast<float>(Dn - Up) * Step);
			const FVector Normal = FVector(-Gx * 0.9f, -Gy * 0.9f, 1.0f).GetSafeNormal();
			const float Lit = static_cast<float>(FVector::DotProduct(Normal, LightDir));
			const float Flat = static_cast<float>(LightDir.Z);
			const float Temp = WmSmoothField(Warmth, Width, Height, U, V);
			const float Grain = WmFbm(U * 9.0f, V * 9.0f, NoiseSeed + 11u, 3);
			const float Fine = WmValue(U * 34.0f, V * 34.0f, NoiseSeed + 13u);

			float Rc = 0.0f;
			float Gc = 0.0f;
			float Bc = 0.0f;
			if (Look.bWater)
			{
				// Sea: shallow teal by the shore to deep blue, read smoothly so the coasts shade off; faint swell.
				const float Depth = FMath::Clamp(-WmSmoothField(Relief, Width, Height, U, V), 0.0f, 1.0f);
				const float Shelf = WmSmooth(0.0f, 0.18f, Depth);
				const float Abyss = WmSmooth(0.2f, 0.9f, Depth);
				Rc = FMath::Lerp(FMath::Lerp(0.30f, 0.12f, Shelf), 0.05f, Abyss);
				Gc = FMath::Lerp(FMath::Lerp(0.56f, 0.32f, Shelf), 0.13f, Abyss);
				Bc = FMath::Lerp(FMath::Lerp(0.62f, 0.52f, Shelf), 0.30f, Abyss);
				const float Swell = 0.94f + 0.08f * Grain + 0.03f * (Lit - Flat);
				Rc *= Swell;
				Gc *= Swell;
				Bc *= Swell;
				if (Temp < -6.0f)
				{
					const float Floe = WmSmooth(-6.0f, -14.0f, Temp) * WmSmooth(0.42f, 0.62f, WmFbm(U * 2.5f, V * 2.5f, NoiseSeed + 23u, 3));
					Rc = FMath::Lerp(Rc, 0.80f, Floe);
					Gc = FMath::Lerp(Gc, 0.87f, Floe);
					Bc = FMath::Lerp(Bc, 0.93f, Floe);
				}
				if (Across >= 0 && !bAcrossWater && Edge < 0.16f)
				{
					// Surf along the coast.
					const float Surf = FMath::Square(1.0f - Edge / 0.16f) * (0.55f + 0.45f * Fine);
					Rc = FMath::Lerp(Rc, 0.62f, Surf * 0.7f);
					Gc = FMath::Lerp(Gc, 0.80f, Surf * 0.7f);
					Bc = FMath::Lerp(Bc, 0.82f, Surf * 0.7f);
				}
			}
			else
			{
				Rc = Look.Colour.R;
				Gc = Look.Colour.G;
				Bc = Look.Colour.B;
				// Each hexagon a touch lighter or darker than its neighbours, the way RimWorld's tiles read.
				const float Jitter = 0.96f + 0.08f * WmHash(Hex.X, Hex.Y, NoiseSeed + 5u);
				float Texture = 1.0f;
				switch (Look.Ground)
				{
				case EWmGround::Forest:
				case EWmGround::Jungle:
				{
					// Tree crowns: dark dots on the green.
					const float Crowns = WmValue(U * 26.0f, V * 26.0f, NoiseSeed + 17u);
					Texture = 0.80f + 0.30f * WmSmooth(0.35f, 0.75f, Crowns) + 0.06f * Grain;
					break;
				}
				case EWmGround::Desert:
				{
					// Dunes: soft diagonal streaks.
					const float Dune = std::sin((U * 0.8f + V * 1.6f) * 18.0f + Grain * 6.0f);
					Texture = 0.95f + 0.06f * Dune + 0.05f * Grain;
					break;
				}
				case EWmGround::Ice:
					Texture = 0.96f + 0.06f * Grain;
					break;
				case EWmGround::Swamp:
				{
					// Pools of standing water.
					const float Pool = WmSmooth(0.62f, 0.7f, WmFbm(U * 7.0f, V * 7.0f, NoiseSeed + 19u, 3));
					Texture = 0.92f + 0.10f * Grain;
					Rc = FMath::Lerp(Rc, 0.20f, Pool * 0.6f);
					Gc = FMath::Lerp(Gc, 0.30f, Pool * 0.6f);
					Bc = FMath::Lerp(Bc, 0.32f, Pool * 0.6f);
					break;
				}
				case EWmGround::Tundra:
					Texture = 0.92f + 0.12f * Grain;
					break;
				default:
					Texture = 0.92f + 0.12f * Grain + 0.04f * (Fine - 0.5f);
					break;
				}
				const float Tone = Jitter * Texture;
				Rc *= Tone;
				Gc *= Tone;
				Bc *= Tone;
				// Rock showing on the steep and high ground; snow on the cold peaks.
				const float Rough = WmSmoothField(Hills, Width, Height, U, V);
				const float Height01 = Lift[Py * PicW + Px];
				const float Rock = WmSmooth(0.45f, 0.95f, Rough) * WmSmooth(0.15f, 0.55f, Height01) * 0.7f;
				Rc = FMath::Lerp(Rc, 0.46f, Rock);
				Gc = FMath::Lerp(Gc, 0.42f, Rock);
				Bc = FMath::Lerp(Bc, 0.38f, Rock);
				const float SnowLine = 0.55f + FMath::Clamp(Temp, -20.0f, 30.0f) * 0.02f;
				const float Snow = WmSmooth(SnowLine, SnowLine + 0.2f, Height01) * WmSmooth(0.3f, 0.7f, Rough);
				Rc = FMath::Lerp(Rc, 0.93f, Snow);
				Gc = FMath::Lerp(Gc, 0.95f, Snow);
				Bc = FMath::Lerp(Bc, 0.97f, Snow);
				// Light and shade of the relief (flat ground keeps its colour).
				const float Shade = FMath::Clamp(1.0f + (Lit - Flat) * 1.25f, 0.35f, 1.45f);
				Rc *= Shade;
				Gc *= Shade;
				Bc *= Shade;
				if (bAcrossWater && Edge < 0.07f)
				{
					// A beach (or a rocky shore where it is cold) along the sea.
					const float Sand = FMath::Square(1.0f - Edge / 0.07f);
					const bool bCold = Temp < 0.0f;
					Rc = FMath::Lerp(Rc, bCold ? 0.55f : 0.86f, Sand * 0.75f);
					Gc = FMath::Lerp(Gc, bCold ? 0.55f : 0.80f, Sand * 0.75f);
					Bc = FMath::Lerp(Bc, bCold ? 0.52f : 0.62f, Sand * 0.75f);
				}
			}

			// The grid: a fine dark line round every hexagon; the coastline a little stronger.
			const bool bCoast = Across >= 0 && bAcrossWater != Look.bWater;
			const float LineWidth = bCoast ? 0.045f : 0.032f;
			const float Line = 1.0f - WmSmooth(0.0f, LineWidth, Edge);
			const float LineDark = Line * (bCoast ? 0.45f : (Look.bWater ? 0.12f : 0.22f));
			Rc *= 1.0f - LineDark;
			Gc *= 1.0f - LineDark;
			Bc *= 1.0f - LineDark;

			if (Look.bDim)
			{
				// Not settleable yet (new game): greyed and darkened, so the open land stands out.
				const float Grey = Rc * 0.3f + Gc * 0.59f + Bc * 0.11f;
				Rc = FMath::Lerp(Grey, Rc, 0.35f) * 0.55f;
				Gc = FMath::Lerp(Grey, Gc, 0.35f) * 0.55f;
				Bc = FMath::Lerp(Grey, Bc, 0.35f) * 0.55f;
			}
			Out[0] = static_cast<uint8>(FMath::Clamp(Bc, 0.0f, 1.0f) * 255.0f + 0.5f);
			Out[1] = static_cast<uint8>(FMath::Clamp(Gc, 0.0f, 1.0f) * 255.0f + 0.5f);
			Out[2] = static_cast<uint8>(FMath::Clamp(Rc, 0.0f, 1.0f) * 255.0f + 0.5f);
			Out[3] = 255;
		}
	});

	// The picture and its halves (each level is drawn when it is at least as big on screen: sharp, no shimmer).
	int32 LevelW = PicW;
	int32 LevelH = PicH;
	for (int32 Level = 0; Level < 4 && LevelW >= 64 && LevelH >= 32; ++Level)
	{
		if (UTexture2D* Texture = WmTexture(Pixels, LevelW, LevelH))
		{
			MapPictures.Add(Texture);
		}
		int32 HalfW = 0;
		int32 HalfH = 0;
		Pixels = WmHalve(Pixels, LevelW, LevelH, HalfW, HalfH);
		LevelW = HalfW;
		LevelH = HalfH;
	}
	UE_LOG(LogTemp, Display, TEXT("World map picture: %d x %d (%d px a hexagon), %d levels"), PicW, PicH, PerHex, MapPictures.Num());
}

FString UWorldScreenSubsystem::WhyNotStart(int32 Tile) const
{
	const colony::World* World = GetShownWorld();
	if (!World || Tile < 0 || Tile >= World->tileCount())
	{
		return TEXT("No tile is picked.");
	}
	if (World->canStartAt(Tile) && !NewGameAllows(*World, Tile))
	{
		return TEXT("Only tropical land can be settled for now: the other biomes are not finished yet.");
	}
	if (World->canStartAt(Tile))
	{
		return FString();
	}
	const colony::WorldTile& T = World->tile(Tile);
	if (T.water || T.biome == colony::kNoBiome)
	{
		return TEXT("A colony can't start on water.");
	}
	if (T.hilliness == colony::Hilliness::Impassable)
	{
		return TEXT("Impassable mountains.");
	}
	if (T.settlement != colony::kNoSettlement)
	{
		return TEXT("A settlement is already here.");
	}
	return TEXT("Too steep for a colony.");
}

TArray<FString> UWorldScreenSubsystem::DescribeTile(int32 Tile) const
{
	TArray<FString> Lines;
	const colony::World* World = GetShownWorld();
	if (!World || Tile < 0 || Tile >= World->tileCount())
	{
		return Lines;
	}
	const colony::WorldTile& T = World->tile(Tile);
	Lines.Add(FString::Printf(TEXT("Tile %d (%d, %d)"), Tile, World->tileX(Tile), World->tileY(Tile)));
	if (T.biome == colony::kNoBiome)
	{
		Lines.Add(TEXT("Biome: open water"));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("Biome: %s%s"), *Words(World->biomeName(T)), T.water ? TEXT(" (frozen water)") : TEXT("")));
	}
	if (!T.water)
	{
		Lines.Add(FString::Printf(TEXT("Terrain: %s"), *HillsWords(T.hilliness)));
	}
	Lines.Add(FString::Printf(TEXT("Elevation: %d m"), static_cast<int32>(T.elevation)));
	Lines.Add(FString::Printf(TEXT("Temperature: %.1f C a year"), T.temperature));
	Lines.Add(FString::Printf(TEXT("Rainfall: %.0f mm a year"), T.rainfall));
	if (T.river != colony::RiverSize::None)
	{
		Lines.Add(FString::Printf(TEXT("River: %s"), UTF8_TO_TCHAR(colony::riverSizeName(T.river))));
	}
	if (T.road != colony::RoadKind::None)
	{
		Lines.Add(FString::Printf(TEXT("Road: %s"), UTF8_TO_TCHAR(colony::roadKindName(T.road))));
	}
	if (T.settlement != colony::kNoSettlement)
	{
		const colony::WorldSettlement& S = World->settlements()[static_cast<size_t>(T.settlement)];
		Lines.Add(FString::Printf(TEXT("Settlement: %s (%s)"), UTF8_TO_TCHAR(S.name.c_str()), *Words(S.faction)));
	}
	return Lines;
}

bool UWorldScreenSubsystem::StartHere()
{
	USimSubsystem* Bridge = GetBridge();
	const colony::World* World = GetShownWorld();
	if (!Bridge || !World || Selected < 0 || !World->canStartAt(Selected) || !NewGameAllows(*World, Selected))
	{
		return false;
	}
	FSimStartOptions Options = Bridge->GetStartOptions();
	Options.bWorld = true;
	Options.WorldSeed = World->seed();
	Options.WorldWidth = World->width();
	Options.WorldHeight = World->height();
	Options.WorldStartTile = Selected;
	if (Options.Scenario.IsEmpty())
	{
		Options.Scenario = TEXT("Crashlanded");
	}
	const int32 Wanted = Selected;
	// A new game waits paused at its first second for the character screen (its edits are refused once it has run).
	if (bNewGame)
	{
		Bridge->SetSpeed(0);
	}
	Bridge->StartSim(Options);
	const colony::World* Made = Bridge->GetSim() ? Bridge->GetSim()->world() : nullptr;
	if (!Made || Made->startTile() != Wanted)
	{
		return false;
	}
	// The sim's world is now the one on screen; the picked tile stays so the colony's tile is marked.
	bPreview = false;
	Preview.reset();
	SimGeneration = Bridge->GetSimGeneration();
	bOpen = false;
	++Stats.ColoniesStarted;
	if (bNewGame)
	{
		bNewGame = false;
		bLanded = true;
	}
	return true;
}

void UWorldScreenSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	if (SimGeneration != Bridge->GetSimGeneration())
	{
		// The sim was replaced (new start, load): show its world, or a preview of the same seed without one.
		SimGeneration = Bridge->GetSimGeneration();
		const colony::World* SimWorld = Bridge->GetSim()->world();
		bPreview = !SimWorld || SimWorld->empty();
		Preview.reset();
		Selected = SimWorld ? SimWorld->startTile() : -1;
		if (bPreview)
		{
			MakeWorld(static_cast<int32>(Bridge->GetStartOptions().Seed & 0x7fffffff));
		}
	}
	else if (bPreview && !Preview && Bridge->GetDefs())
	{
		MakeWorld(Seed);
	}
	const UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
	if (!bOpen && !(Overseer && Overseer->IsOverseerActive()))
	{
		return;
	}
	DrawScreen(*Bridge);
}

void UWorldScreenSubsystem::DrawScreen(USimSubsystem& Bridge)
{
	const ImGui::FScopedContext ScopedContext;
	if (!ScopedContext)
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	if (Viewport->Size.x <= 0.0f || Viewport->Size.y <= 0.0f)
	{
		return;
	}
	const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
	ColonyHud::ApplyFrameStyle(Scale);
	const float Margin = 12.0f * Scale;

	ImGuiIO& IO = ImGui::GetIO();
	if (!bNewGame && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_M, false))
	{
		bOpen = !bOpen;
	}
	if (bOpen && !IO.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		bOpen = false;
		if (bNewGame)
		{
			bNewGame = false;
			bCancelled = true;
		}
	}
	// Opened from the bottom bar's World tab (UColonyHudSubsystem) or the M key.
	if (!bOpen)
	{
		return;
	}

	const colony::World* World = GetShownWorld();
	ImGui::SetNextWindowPos(Viewport->Pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(Viewport->Size, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.96f);
	const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	if (ImGui::Begin("##WorldScreen", nullptr, Flags))
	{
		const float PanelWidth = 360.0f * Scale;
		if (bNewGame)
		{
			// The new game's second step (docs/design/new-game.md): pick where the shuttle comes down.
			if (ImGui::Button("Back"))
			{
				bOpen = false;
				bNewGame = false;
				bCancelled = true;
			}
			ImGui::SameLine();
			ImGui::TextColored(ColonyHud::Accent(), "Choose your landing site");
			ImGui::SameLine();
			ImGui::TextColored(ColonyHud::TextDim(), "(Scenario  >  Landing site  >  People)");
			ImGui::SameLine();
			if (ImGui::Button("Pick a random site"))
			{
				PickRandomSite();
			}
		}
		else
		{
			ImGui::TextUnformatted("World map");
			ImGui::SameLine();
			if (ImGui::Button("Close"))
			{
				bOpen = false;
			}
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(110.0f * Scale);
		ImGui::InputInt("##seed", &SeedInput, 0, 0);
		ImGui::SameLine();
		if (ImGui::Button("Make world"))
		{
			MakeWorld(FMath::Max(SeedInput, 1));
			World = GetShownWorld();
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(bPreview ? "(preview - the colony is not on it yet)" : "(the colony's world)");

		const ImVec2 Origin = ImGui::GetCursorScreenPos();
		const float MapW = FMath::Max(1.0f, Viewport->Pos.x + Viewport->Size.x - PanelWidth - Margin - Origin.x);
		const float MapH = FMath::Max(1.0f, Viewport->Pos.y + Viewport->Size.y - Margin - Origin.y);
		Stats.TilesDrawn = 0;
		if (!World)
		{
			ImGui::TextUnformatted("No world: the data folder has no WorldGen def.");
		}
		else
		{
			BuildMapPicture(*World);
			ImDrawList* Draw = ImGui::GetWindowDrawList();
			const ImVec2 ViewMin = Origin;
			const ImVec2 ViewMax(Origin.x + MapW, Origin.y + MapH);

			// The view: zoom 1 fits the whole map; the middle of the view is a map point (hexagon widths), kept so the map
			// never slides off.
			const FVector2D Units = MapSizeInTiles(*World);
			const float FitSize = std::min(MapW / static_cast<float>(Units.X), MapH / static_cast<float>(Units.Y));
			MapZoom = FMath::Clamp(MapZoom, 1.0f, 8.0f);
			float TileSize = FitSize * MapZoom;
			const auto ClampView = [&]()
			{
				const double HalfW = MapW * 0.5 / TileSize;
				const double HalfH = MapH * 0.5 / TileSize;
				MapViewCentre.X = Units.X <= 2.0 * HalfW ? Units.X * 0.5 : FMath::Clamp(MapViewCentre.X, HalfW, Units.X - HalfW);
				MapViewCentre.Y = Units.Y <= 2.0 * HalfH ? Units.Y * 0.5 : FMath::Clamp(MapViewCentre.Y, HalfH, Units.Y - HalfH);
			};
			if (MapViewCentre.X < 0.0)
			{
				MapViewCentre = Units * 0.5;
			}
			ClampView();

			// The map takes the mouse: the wheel zooms at the pointer, dragging (any button) moves it, a click picks.
			ImGui::SetCursorScreenPos(ViewMin);
			const bool bMapClicked = ImGui::InvisibleButton("##map", ImVec2(MapW, MapH),
				ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
			const bool bMapHovered = ImGui::IsItemHovered();
			const bool bMapActive = ImGui::IsItemActive();
			const ImVec2 Mouse = ImGui::GetMousePos();
			if (bMapHovered && IO.MouseWheel != 0.0f)
			{
				const double PointerU = MapViewCentre.X + (Mouse.x - (ViewMin.x + MapW * 0.5f)) / TileSize;
				const double PointerV = MapViewCentre.Y + (Mouse.y - (ViewMin.y + MapH * 0.5f)) / TileSize;
				MapZoom = FMath::Clamp(MapZoom * FMath::Pow(1.2f, IO.MouseWheel), 1.0f, 8.0f);
				TileSize = FitSize * MapZoom;
				MapViewCentre.X = PointerU - (Mouse.x - (ViewMin.x + MapW * 0.5f)) / TileSize;
				MapViewCentre.Y = PointerV - (Mouse.y - (ViewMin.y + MapH * 0.5f)) / TileSize;
				ClampView();
			}
			if (ImGui::IsItemActivated())
			{
				bMapDragged = false;
			}
			if (bMapActive && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f) ||
				ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 2.0f)))
			{
				bMapDragged = true;
				MapViewCentre.X -= IO.MouseDelta.x / TileSize;
				MapViewCentre.Y -= IO.MouseDelta.y / TileSize;
				ClampView();
			}
			const float Left = ViewMin.x + MapW * 0.5f - static_cast<float>(MapViewCentre.X) * TileSize;
			const float Top = ViewMin.y + MapH * 0.5f - static_cast<float>(MapViewCentre.Y) * TileSize;
			const auto Center = [&](int32 Tile)
			{
				const FVector2D C = TileCentre(*World, Left, Top, TileSize, Tile);
				return ImVec2(static_cast<float>(C.X), static_cast<float>(C.Y));
			};
			const int32 Hover = bMapHovered ? TileAt(*World, Left, Top, TileSize, Mouse.x, Mouse.y) : -1;
			if (Hover >= 0 && bMapClicked && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !bMapDragged)
			{
				SelectTile(Hover);
			}

			Draw->PushClipRect(ViewMin, ViewMax, true);
			Draw->AddRectFilled(ViewMin, ViewMax, IM_COL32(6, 10, 16, 255));
			const float PicW = static_cast<float>(Units.X) * TileSize;
			const float PicH = static_cast<float>(Units.Y) * TileSize;
			if (MapPictures.Num() > 0)
			{
				// The smallest level still at least as wide as it is drawn.
				int32 Level = 0;
				while (Level + 1 < MapPictures.Num() && MapPictures[Level + 1] && static_cast<float>(MapPictures[Level + 1]->GetSizeX()) >= PicW)
				{
					++Level;
				}
				if (UTexture2D* Picture = MapPictures[Level])
				{
					Draw->AddImage(ImTextureRef(Picture), ImVec2(Left, Top), ImVec2(Left + PicW, Top + PicH));
				}
			}

			// Which hexagons are on screen (all of them at zoom 1: the smoke test counts them).
			const auto OnScreen = [&](const ImVec2& C)
			{
				return C.x >= ViewMin.x - TileSize && C.x <= ViewMax.x + TileSize && C.y >= ViewMin.y - TileSize && C.y <= ViewMax.y + TileSize;
			};
			const bool bCrispGrid = MapPictures.Num() > 0 && TileSize > static_cast<float>(MapPictures[0]->GetSizeX()) / static_cast<float>(Units.X) * 1.4f;
			ImVec2 Corners[6];
			for (int32 Tile = 0; Tile < World->tileCount(); ++Tile)
			{
				const ImVec2 C = Center(Tile);
				if (!OnScreen(C))
				{
					continue;
				}
				++Stats.TilesDrawn;
				if (bCrispGrid)
				{
					// Zoomed in past the picture's own detail: draw the grid sharp over it.
					WmCorners(C, TileSize, Corners);
					Draw->AddPolyline(Corners, 6, IM_COL32(0, 0, 0, 46), 1.0f, ImDrawFlags_Closed);
				}
			}

			// Rivers: smooth curves through the hexagons' middles, wider downstream (not across the wrap).
			const auto RiverFlow = [&](int32 Tile)
			{
				const colony::WorldTile& T = World->tile(Tile);
				if (T.river == colony::RiverSize::None || T.riverFlowsTo == colony::kNoDirection || T.water)
				{
					return -1;
				}
				const int32 To = World->neighbour(Tile, T.riverFlowsTo);
				return To >= 0 && std::abs(World->tileX(To) - World->tileX(Tile)) <= 1 ? To : -1;
			};
			TArray<uint8> HasInflow;
			HasInflow.SetNumZeroed(World->tileCount());
			for (int32 Tile = 0; Tile < World->tileCount(); ++Tile)
			{
				const int32 To = RiverFlow(Tile);
				if (To >= 0)
				{
					HasInflow[To] = 1;
				}
			}
			const auto Mid = [](const ImVec2& A, const ImVec2& B) { return ImVec2((A.x + B.x) * 0.5f, (A.y + B.y) * 0.5f); };
			for (int32 Tile = 0; Tile < World->tileCount(); ++Tile)
			{
				const int32 To = RiverFlow(Tile);
				if (To < 0)
				{
					continue;
				}
				const ImVec2 From = Center(Tile);
				const ImVec2 Into = Center(To);
				if (!OnScreen(From) && !OnScreen(Into))
				{
					continue;
				}
				const float Size = static_cast<float>(static_cast<int32>(World->tile(Tile).river));
				const float Thick = FMath::Max(1.0f, TileSize * (0.05f + 0.035f * Size));
				const ImU32 Blue = IM_COL32(64, 128, 214, 235);
				const ImVec2 Start = Mid(From, Into);
				if (HasInflow[Tile] == 0)
				{
					Draw->AddLine(From, Start, Blue, Thick * 0.8f);  // the spring
				}
				const int32 Next = RiverFlow(To);
				if (Next >= 0)
				{
					Draw->AddBezierQuadratic(Start, Into, Mid(Into, Center(Next)), Blue, Thick, 10);
				}
				else
				{
					Draw->AddLine(Start, Into, Blue, Thick);  // into the sea or a lake
				}
			}
			for (const colony::WorldRoad& Road : World->roads())
			{
				if (std::abs(World->tileX(Road.tileA) - World->tileX(Road.tileB)) > 1)
				{
					continue;
				}
				const ImVec2 A = Center(Road.tileA);
				const ImVec2 B = Center(Road.tileB);
				if (!OnScreen(A) && !OnScreen(B))
				{
					continue;
				}
				const float Thick = FMath::Max(1.0f, TileSize * 0.07f);
				Draw->AddLine(A, B, IM_COL32(20, 14, 10, 120), Thick + 1.5f);
				Draw->AddLine(A, B, RoadColor(Road.kind), Thick);
			}
			// Settlements: a dark-ringed red marker with a light middle; named once there is room.
			const bool bNames = TileSize >= 30.0f * Scale;
			for (const colony::WorldSettlement& Settlement : World->settlements())
			{
				const ImVec2 C = Center(Settlement.tile);
				if (!OnScreen(C))
				{
					continue;
				}
				const float Radius = FMath::Max(3.0f, TileSize * 0.24f);
				Draw->AddCircleFilled(C, Radius + 1.5f, IM_COL32(20, 8, 8, 230));
				Draw->AddCircleFilled(C, Radius, IM_COL32(214, 52, 44, 255));
				Draw->AddCircleFilled(C, Radius * 0.4f, IM_COL32(255, 214, 190, 255));
				if (bNames)
				{
					const char* Name = Settlement.name.c_str();
					const ImVec2 TextSize = ImGui::CalcTextSize(Name);
					const ImVec2 At(C.x - TextSize.x * 0.5f, C.y + Radius + 2.0f);
					Draw->AddText(ImVec2(At.x + 1.0f, At.y + 1.0f), IM_COL32(0, 0, 0, 200), Name);
					Draw->AddText(At, IM_COL32(245, 235, 220, 255), Name);
				}
			}
			const float Stroke = FMath::Max(1.0f, Scale);
			if (World->startTile() >= 0 && !bPreview)
			{
				WmCorners(Center(World->startTile()), TileSize, Corners);
				Draw->AddPolyline(Corners, 6, IM_COL32(0, 0, 0, 160), 4.0f * Stroke, ImDrawFlags_Closed);
				Draw->AddPolyline(Corners, 6, IM_COL32(255, 222, 40, 255), 2.5f * Stroke, ImDrawFlags_Closed);
			}
			if (Hover >= 0 && Hover != Selected)
			{
				WmCorners(Center(Hover), TileSize, Corners);
				Draw->AddConvexPolyFilled(Corners, 6, IM_COL32(255, 255, 255, 38));
				Draw->AddPolyline(Corners, 6, IM_COL32(255, 255, 255, 170), 1.5f * Stroke, ImDrawFlags_Closed);
			}
			if (Selected >= 0)
			{
				// The picked hexagon: a bright jade outline with a soft glow that breathes.
				const ImVec2 C = Center(Selected);
				const float Pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
				WmCorners(C, TileSize * 1.18f, Corners);
				Draw->AddPolyline(Corners, 6, IM_COL32(94, 212, 168, static_cast<int32>(40.0f + 50.0f * Pulse)), 3.0f * Stroke, ImDrawFlags_Closed);
				WmCorners(C, TileSize, Corners);
				Draw->AddConvexPolyFilled(Corners, 6, IM_COL32(94, 212, 168, 40));
				Draw->AddPolyline(Corners, 6, IM_COL32(0, 0, 0, 170), 4.5f * Stroke, ImDrawFlags_Closed);
				Draw->AddPolyline(Corners, 6, IM_COL32(150, 255, 214, 255), 2.5f * Stroke, ImDrawFlags_Closed);
			}
			Draw->PopClipRect();

			if (Hover >= 0)
			{
				const colony::WorldTile& T = World->tile(Hover);
				const FString Name = T.biome == colony::kNoBiome ? FString(TEXT("open water")) : Words(World->biomeName(T));
				ImGui::SetTooltip("%s", TCHAR_TO_UTF8(*FString::Printf(TEXT("%s, %s"), *Name, T.water ? TEXT("water") : *HillsWords(T.hilliness))));
			}

			// The facts of the picked tile, and the button that starts a colony on it.
			ImGui::SetCursorScreenPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - PanelWidth, Origin.y));
			ImGui::BeginChild("##facts", ImVec2(PanelWidth - Margin, MapH), ImGuiChildFlags_Borders);
			if (Selected < 0)
			{
				ImGui::TextUnformatted("Click a tile to see what is there.");
			}
			else
			{
				for (const FString& Line : DescribeTile(Selected))
				{
					WText(Line);
				}
				ImGui::Separator();
				const FString Why = WhyNotStart(Selected);
				if (Why.IsEmpty())
				{
					if (ImGui::Button(bNewGame ? "Land here" : "Start colony here"))
					{
						StartHere();
					}
					if (!bNewGame && !bPreview && Selected != World->startTile())
					{
						ImGui::TextUnformatted("This starts a new colony.");
					}
				}
				else
				{
					WText(Why);
				}
			}
			ImGui::Separator();
			ImGui::TextUnformatted("Red markers: settlements. Blue: rivers.");
			ImGui::TextUnformatted("Brown to black lines: roads.");
			if (World->startTile() >= 0 && !bPreview)
			{
				ImGui::TextUnformatted("Yellow hexagon: the colony's tile.");
			}
			ImGui::TextColored(ColonyHud::TextDim(), "Mouse wheel zooms, drag to move the map.");
			ImGui::EndChild();
		}
	}
	ImGui::End();
	++Stats.FramesDrawn;
}
