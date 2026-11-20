#include "WetnessSubsystem.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialParameterCollection.h"
#include "Math/Float16.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#if WITH_EDITOR
#include "TextureCompiler.h"
#endif

#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"
#include "SkySubsystem.h"
#include "TerrainDraw.h"
#include "WetnessShared.h"

namespace
{
	TAutoConsoleVariable<int32> GWetEnabled(
		TEXT("Wet.Enabled"), 1, TEXT("1: rain makes buildings, plants, rocks and people wet; 0: everything stays dry."), ECVF_Default);
	TAutoConsoleVariable<float> GWetOverride(
		TEXT("Wet.Override"), -1.0f, TEXT("A fixed surface wetness 0..1 for looking at (people too, when outdoors); -1 follows the weather."), ECVF_Default);

	// Game seconds for surfaces to soak through in rain, and to dry after it.
	constexpr float WetSurfaceSoakSeconds = 600.0f;
	constexpr float WetSurfaceDrySeconds = 7200.0f;
	// The same for a person: rain soaks hair and clothes faster than it soaks a wall, and they dry by the fire sooner.
	constexpr float WetPawnSoakSeconds = 300.0f;
	constexpr float WetPawnDrySeconds = 2400.0f;
	// How far over its feet a person is counted as under a roof (the middle of the body).
	constexpr float WetPawnBodyM = 1.0f;

	FAutoConsoleCommandWithWorld GWetDump(
		TEXT("Wet.Dump"), TEXT("Logs the wetness state: the cover texture, the cells under a roof, the surface wetness."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			const UWetnessSubsystem* Wetness = World ? World->GetSubsystem<UWetnessSubsystem>() : nullptr;
			UE_LOG(LogTemp, Display, TEXT("Wetness: %s"), Wetness ? *Wetness->Describe() : TEXT("no subsystem in this world"));
		}));

	float WetStepToward(float Value, float Target, float Elapsed, float SoakSeconds, float DrySeconds)
	{
		if (Value < Target)
		{
			return FMath::Min(Target, Value + Elapsed / SoakSeconds);
		}
		return FMath::Max(Target, Value - Elapsed / DrySeconds);
	}
}

bool UWetnessSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UWetnessSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWetnessSubsystem, STATGROUP_Tickables);
}

void UWetnessSubsystem::Initialize(FSubsystemCollectionBase& InCollection)
{
	Super::Initialize(InCollection);
	Bridge = InCollection.InitializeDependency<USimSubsystem>();
	InCollection.InitializeDependency<USkySubsystem>();
	InCollection.InitializeDependency<USimPawnSubsystem>();
}

void UWetnessSubsystem::LoadAssets()
{
	if (!bAssetsTried)
	{
		bAssetsTried = true;
		Collection = LoadObject<UMaterialParameterCollection>(nullptr, WetnessShared::CollectionPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		Cover = LoadObject<UTexture2D>(nullptr, WetnessShared::CoverPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Collection || !Cover)
		{
			UE_LOG(LogTemp, Display, TEXT("Wetness: no MPC_Weather or T_WeatherCover (run -run=WetLook); only people get wet"));
		}
	}
	if (!Cover || bCoverUsable)
	{
		return;
	}
#if WITH_EDITOR
	// In an editor build a loaded texture may still be building in the background, with a stand-in that has no format
	// yet: wait for it, once, so the cover isn't sent to the stand-in and lost.
	UTexture* Compiling = Cover.Get();
	FTextureCompilingManager::Get().FinishCompilation(MakeArrayView(&Compiling, 1));
#endif
	// Checked again every tick until it holds (the format can turn up a little after the load).
	const EPixelFormat Format = Cover->GetPixelFormat();
	const bool bSize = Cover->GetSizeX() == WetnessShared::CoverSize && Cover->GetSizeY() == WetnessShared::CoverSize;
	bCoverHalf = Format == PF_R16F;
	bCoverUsable = bSize && (Format == PF_R32_FLOAT || Format == PF_R16F);
	if (bCoverUsable)
	{
		bCoverDirty = true;
		UE_LOG(LogTemp, Display, TEXT("Wetness: T_WeatherCover ready, %dx%d %s"), Cover->GetSizeX(), Cover->GetSizeY(), bCoverHalf ? TEXT("halves") : TEXT("floats"));
	}
	else if (!bCoverFormatLogged && Format != PF_Unknown)
	{
		bCoverFormatLogged = true;
		UE_LOG(LogTemp, Warning, TEXT("Wetness: T_WeatherCover is %dx%d pixel format %d, not %dx%d single floats; roofs won't keep things dry"),
			Cover->GetSizeX(), Cover->GetSizeY(), static_cast<int32>(Format), WetnessShared::CoverSize, WetnessShared::CoverSize);
	}
}

FString UWetnessSubsystem::Describe() const
{
	return FString::Printf(TEXT("assets %s, cover %s (pixel format %d), %d cells covered, cover made %d times and sent %d times, surface wetness %.2f, %d wet pawns"),
		HasAssets() ? TEXT("found") : TEXT("missing"), bCoverUsable ? TEXT("usable") : TEXT("NOT usable"),
		Cover ? static_cast<int32>(Cover->GetPixelFormat()) : -1, CoveredCells, CoverWrites, CoverUploads, ShownSurfaceWet, WetPawns);
}

float UWetnessSubsystem::CoverAt(int32 X, int32 Y) const
{
	if (X < 0 || Y < 0 || X >= CoverWidth || Y >= CoverHeight)
	{
		return WetnessShared::OpenSky;
	}
	return CoverCells[Y * CoverWidth + X];
}

void UWetnessSubsystem::UpdateCover(const colony::Map& Map, bool bForce)
{
	const USimSubsystem* Source = Bridge.Get();
	const colony::Snapshot& Snapshot = Source->GetCurrentSnapshot();
	if (!bForce && Snapshot.roofChanges == LastRoofChanges && Snapshot.roofCollapses == LastCollapses &&
		Map.upperFloorChanges() == LastFloorChanges && Map.rockChanges() == LastRockChanges)
	{
		return;
	}
	LastRoofChanges = Snapshot.roofChanges;
	LastCollapses = Snapshot.roofCollapses;
	LastFloorChanges = Map.upperFloorChanges();
	LastRockChanges = Map.rockChanges();

	// The highest roof or floor over each cell, in metres over the world origin (as the terrain and levels draw them).
	CoverWidth = Map.width();
	CoverHeight = Map.height();
	CoverCells.Init(WetnessShared::OpenSky, CoverWidth * CoverHeight);
	CoveredCells = 0;
	for (int32 Y = 0; Y < CoverHeight; ++Y)
	{
		for (int32 X = 0; X < CoverWidth; ++X)
		{
			const float Ground = Map.groundHeight(X, Y);
			float Top = WetnessShared::OpenSky;
			const colony::CellPos Cell{X, Y, 0};
			const colony::Roof Roof = Map.roof(Cell);
			// Rock itself is open to the sky on top; the mountain's roof only counts over mined-out rock.
			if (Roof == colony::Roof::Thick && Map.terrain(Cell) != colony::Terrain::Rock)
			{
				Top = Ground + TerrainDraw::RockHeightM;
			}
			else if (Roof == colony::Roof::Thin)
			{
				Top = Ground + TerrainDraw::RoofHeightM;
			}
			for (int32 Level = 1; Level < colony::Map::kLevels; ++Level)
			{
				const colony::CellPos Upper{X, Y, Level};
				if (!Map.hasUpperChunk(Upper))
				{
					continue;
				}
				const float FloorZ = Ground + static_cast<float>(Level) * colony::Map::kLevelHeightM;
				if (Map.upperFloor(Upper))
				{
					Top = FMath::Max(Top, FloorZ);
				}
				if (Map.roof(Upper) != colony::Roof::None)
				{
					Top = FMath::Max(Top, FloorZ + TerrainDraw::RoofHeightM);
				}
			}
			if (Top > WetnessShared::OpenSky)
			{
				++CoveredCells;
			}
			CoverCells[Y * CoverWidth + X] = Top;
		}
	}
	++CoverWrites;
	bCoverDirty = true;
}

void UWetnessSubsystem::UploadCover()
{
	if (!bCoverUsable || CoverCells.IsEmpty())
	{
		return;
	}
	const FTextureResource* Resource = Cover->GetResource();
	if (!Resource)
	{
		return;
	}
	if (!bCoverDirty && Resource == UploadedResource)
	{
		return;
	}
	bCoverDirty = false;
	UploadedResource = Resource;
	++CoverUploads;
	// The texture is the map's size or less; a map bigger than it is covered as far as it reaches.
	const int32 W = FMath::Min(CoverWidth, WetnessShared::CoverSize);
	const int32 H = FMath::Min(CoverHeight, WetnessShared::CoverSize);
	const int32 TexelBytes = bCoverHalf ? static_cast<int32>(sizeof(FFloat16)) : static_cast<int32>(sizeof(float));
	TArray<uint8>* Texels = new TArray<uint8>();
	Texels->SetNumUninitialized(W * H * TexelBytes);
	for (int32 Y = 0; Y < H; ++Y)
	{
		const float* Row = CoverCells.GetData() + Y * CoverWidth;
		if (bCoverHalf)
		{
			FFloat16* Out = reinterpret_cast<FFloat16*>(Texels->GetData()) + Y * W;
			for (int32 X = 0; X < W; ++X)
			{
				Out[X] = FFloat16(Row[X]);
			}
		}
		else
		{
			FMemory::Memcpy(Texels->GetData() + Y * W * TexelBytes, Row, W * TexelBytes);
		}
	}
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, W, H);
	Cover->UpdateTextureRegions(0, 1, Region, W * TexelBytes, TexelBytes, Texels->GetData(),
		[Texels](uint8*, const FUpdateTextureRegion2D* Regions)
		{
			delete Texels;
			delete Regions;
		});
	if (CoverUploads == 1)
	{
		UE_LOG(LogTemp, Display, TEXT("Wetness: cover sent, %d of %dx%d cells under a roof or floor"), CoveredCells, CoverWidth, CoverHeight);
	}
}

void UWetnessSubsystem::UpdateSurfaces(float Elapsed)
{
	const USkySubsystem* Sky = GetWorld()->GetSubsystem<USkySubsystem>();
	const FSkyState* State = Sky ? &Sky->GetState() : nullptr;
	// Rain soaks surfaces as it soaks the ground (USkySubsystem::ApplyWetness): harder rain on wetter weather, wetter.
	const float Target = State && State->Rain > 0.01f ? FMath::Clamp(0.5f + State->Wetness, 0.0f, 1.0f) : 0.0f;
	SurfaceWet = WetStepToward(SurfaceWet, Target, Elapsed, WetSurfaceSoakSeconds, WetSurfaceDrySeconds);
	const float Override = GWetOverride.GetValueOnGameThread();
	const float Shown = GWetEnabled.GetValueOnGameThread() == 0 ? 0.0f : (Override >= 0.0f ? FMath::Clamp(Override, 0.0f, 1.0f) : SurfaceWet);
	ShownSurfaceWet = Shown;
	if (!Collection || FMath::Abs(Shown - WrittenSurfaceWet) < 0.002f)
	{
		return;
	}
	if (UMaterialParameterCollectionInstance* Instance = GetWorld()->GetParameterCollectionInstance(Collection))
	{
		Instance->SetScalarParameterValue(WetnessShared::WetnessParam, Shown);
		WrittenSurfaceWet = Shown;
	}
}

void UWetnessSubsystem::UpdatePawns(const colony::Map& Map, float Elapsed)
{
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	const USkySubsystem* Sky = GetWorld()->GetSubsystem<USkySubsystem>();
	if (!Pawns)
	{
		return;
	}
	const bool bRaining = Sky && Sky->GetState().Rain > 0.01f;
	const float RainTarget = Sky ? FMath::Clamp(0.5f + Sky->GetState().Wetness, 0.0f, 1.0f) : 0.0f;
	const float Override = GWetOverride.GetValueOnGameThread();
	const bool bEnabled = GWetEnabled.GetValueOnGameThread() != 0;
	const FVector Origin = Bridge->GetWorldOrigin();
	WetPawns = 0;
	WettestPawn = 0.0f;
	TSet<uint32> Seen;
	TArray<UPrimitiveComponent*> Components;
	for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Pair : Pawns->GetPawns())
	{
		ASimPawnActor* Actor = Pair.Value.Get();
		if (!Actor)
		{
			continue;
		}
		Seen.Add(Pair.Key);
		const FVector At = Actor->GetActorLocation();
		const int32 X = FMath::FloorToInt32((At.X - Origin.X) * 0.01);
		const int32 Y = FMath::FloorToInt32((At.Y - Origin.Y) * 0.01);
		const float Z = static_cast<float>((At.Z - Origin.Z) * 0.01);
		const bool bOutdoors = Z + WetPawnBodyM >= CoverAt(X, Y) - 0.15f;
		const colony::CellPos Cell{X, Y, 0};
		const bool bSwimming = Map.inBounds(Cell) && Map.terrain(Cell) == colony::Terrain::Water && !Map.hasDeck(Cell) &&
			Z < Map.groundHeight(X, Y) + 0.5f;
		float& Wet = PawnWet.FindOrAdd(Pair.Key);
		if (bSwimming)
		{
			Wet = 1.0f;
		}
		else
		{
			const float Target = bOutdoors ? (Override >= 0.0f ? FMath::Clamp(Override, 0.0f, 1.0f) : (bRaining ? RainTarget : 0.0f)) : 0.0f;
			Wet = WetStepToward(Wet, Target, Override >= 0.0f ? 1.0e6f : Elapsed, WetPawnSoakSeconds, WetPawnDrySeconds);
		}
		const float Shown = bEnabled ? Wet : 0.0f;
		WetPawns += Shown > 0.1f ? 1 : 0;
		WettestPawn = FMath::Max(WettestPawn, Shown);
		float& Written = PawnWritten.FindOrAdd(Pair.Key, -1.0f);
		if (FMath::Abs(Shown - Written) < 0.01f)
		{
			continue;
		}
		Written = Shown;
		// Every mesh of the pawn: the body, hair, clothes and anything worn read slot 12 if their material has a wet look.
		Components.Reset();
		Actor->GetComponents<UPrimitiveComponent>(Components, true);
		for (UPrimitiveComponent* Component : Components)
		{
			if (Component && Component->IsRegistered())
			{
				Component->SetCustomPrimitiveDataFloat(WetnessShared::WornDataIndex, Shown);
			}
		}
	}
	for (auto It = PawnWet.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key()))
		{
			PawnWritten.Remove(It.Key());
			It.RemoveCurrent();
		}
	}
}

void UWetnessSubsystem::Tick(float DeltaTime)
{
	(void)DeltaTime;
	USimSubsystem* Source = Bridge.Get();
	const colony::Sim* Sim = Source ? Source->GetSim() : nullptr;
	if (!Sim)
	{
		return;
	}
	LoadAssets();
	const colony::Map& Map = Sim->map();
	bool bForce = false;
	if (BuiltGeneration != Source->GetSimGeneration())
	{
		// A new game or a load: everything starts dry and the cover is written again.
		BuiltGeneration = Source->GetSimGeneration();
		SurfaceWet = 0.0f;
		LastGameSeconds = -1.0;
		PawnWet.Reset();
		PawnWritten.Reset();
		bForce = true;
	}
	const FVector Origin = Source->GetWorldOrigin();
	if (Collection && (bForce || Origin != WrittenOrigin))
	{
		// Where the cover texture lies: cell (0, 0) at the world origin, one texel a metre. Written again whenever the
		// origin moves (the map can be placed after the first tick), or every roof would mask the wrong cells.
		if (UMaterialParameterCollectionInstance* Instance = GetWorld()->GetParameterCollectionInstance(Collection))
		{
			Instance->SetVectorParameterValue(WetnessShared::CoverMapParam,
				FLinearColor(static_cast<float>(Origin.X), static_cast<float>(Origin.Y), static_cast<float>(Origin.Z), 1.0f / static_cast<float>(WetnessShared::CoverSize)));
			WrittenOrigin = Origin;
		}
		WrittenSurfaceWet = -1.0f;
	}
	UpdateCover(Map, bForce);
	UploadCover();
	const double Seconds = static_cast<double>(Source->GetGameSeconds());
	if (LastGameSeconds < 0.0)
	{
		LastGameSeconds = Seconds;
	}
	const float Elapsed = static_cast<float>(FMath::Clamp(Seconds - LastGameSeconds, 0.0, 7200.0));
	LastGameSeconds = Seconds;
	UpdateSurfaces(Elapsed);
	UpdatePawns(Map, Elapsed);
}
