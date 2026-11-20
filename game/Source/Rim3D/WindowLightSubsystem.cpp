#include "WindowLightSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/RectLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/LocalFogVolumeComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "SimSubsystem.h"
#include "SkySubsystem.h"
#include "TerrainDraw.h"

#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Windows.h"

namespace
{
	TAutoConsoleVariable<int32> GWinLightsEnable(TEXT("Windows.Lights"), 1, TEXT("Daylight through windows as rect lights (docs/design/windows.md); 0 off."), ECVF_Default);
	TAutoConsoleVariable<int32> GWinMaxLights(TEXT("Windows.MaxLights"), 24, TEXT("How many of the windows nearest the camera get a daylight rect light."), ECVF_Default);
	TAutoConsoleVariable<int32> GWinShadowLights(TEXT("Windows.ShadowLights"), 8, TEXT("How many of those cast shadows (the nearest)."), ECVF_Default);
	TAutoConsoleVariable<float> GWinLightCd(TEXT("Windows.LightCd"), 15000.0f,
		TEXT("Candelas of a window's daylight at a clear noon: bright sky plus sunlit ground seen through the opening."), ECVF_Default);
	TAutoConsoleVariable<float> GWinDust(TEXT("Windows.Dust"), 0.3f,
		TEXT("Dust in the air of rooms with windows (a local fog volume's extinction per metre, drawn into the volumetric fog): sunbeams through windows show as shafts. 0 off."), ECVF_Default);
	TAutoConsoleVariable<int32> GWinDustRooms(TEXT("Windows.DustRooms"), 8, TEXT("How many rooms nearest the camera get dust in their air."), ECVF_Default);
	// A local fog volume is a sphere of this radius at scale 1; scaled per axis it becomes the ellipsoid round a room. PC round 7
	// showed 500 made it a 1 m blob, so it is about 50 cm (a 1 m sphere at scale 1); the console variable is there to check.
	TAutoConsoleVariable<float> GWinDustBaseCm(TEXT("Windows.DustBaseCm"), 50.0f, TEXT("Radius of a local fog volume at scale 1, cm (sizes the rooms' dust)."), ECVF_Default);
	constexpr double WinDustOversize = 1.3;
	constexpr int32 WinDustPool = 16;
	constexpr int32 WinLightPool = 48;
	constexpr float WinLightReachCm = 1000.0f;
	constexpr float WinWallHalfCm = 25.0f;
	const FLinearColor WinSkyColor(0.92f, 0.96f, 1.0f);
}

bool UWindowLightSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UWindowLightSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWindowLightSubsystem, STATGROUP_Tickables);
}

void UWindowLightSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Bridge = Collection.InitializeDependency<USimSubsystem>();
}

void UWindowLightSubsystem::Deinitialize()
{
	Teardown();
	Super::Deinitialize();
}

void UWindowLightSubsystem::Teardown()
{
	if (Holder)
	{
		Holder->Destroy();
	}
	Holder = nullptr;
	Lights.Reset();
	DustVolumes.Reset();
	DustSet.Reset();
	Shining = 0;
	DustRooms = 0;
}

void UWindowLightSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim() || !GetWorld() || GWinLightsEnable.GetValueOnGameThread() == 0)
	{
		if (Holder)
		{
			Teardown();
		}
		return;
	}
	const colony::Sim& Sim = *Source->GetSim();
	if (!Holder)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
		USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("WindowLightRoot"));
		Holder->SetRootComponent(Root);
		Root->RegisterComponent();
#if WITH_EDITOR
		Holder->SetActorLabel(TEXT("WindowLights"));
#endif
		for (int32 I = 0; I < WinLightPool; ++I)
		{
			URectLightComponent* Light = NewObject<URectLightComponent>(Holder);
			Light->SetMobility(EComponentMobility::Movable);
			Light->SetIntensityUnits(ELightUnits::Candelas);
			Light->SetIntensity(0.0f);
			Light->SetLightColor(WinSkyColor);
			Light->SetAttenuationRadius(WinLightReachCm);
			Light->SetBarnDoorAngle(70.0f);
			Light->SetBarnDoorLength(20.0f);
			Light->SetCastShadows(false);
			Light->SetupAttachment(Root);
			Light->RegisterComponent();
			Light->SetVisibility(false);
			Lights.Add(Light);
		}
		for (int32 I = 0; I < WinDustPool; ++I)
		{
			ULocalFogVolumeComponent* Volume = NewObject<ULocalFogVolumeComponent>(Holder);
			Volume->SetMobility(EComponentMobility::Movable);
			Volume->SetRadialFogExtinction(0.0f);
			Volume->SetHeightFogExtinction(0.0f);
			Volume->SetHeightFogFalloff(1000.0f);
			Volume->SetHeightFogOffset(0.0f);
			Volume->SetFogPhaseG(0.2f);
			Volume->SetFogAlbedo(FLinearColor(0.85f, 0.82f, 0.76f));
			Volume->SetFogEmissive(FLinearColor::Black);
			Volume->SetupAttachment(Root);
			Volume->RegisterComponent();
			Volume->SetVisibility(false);
			DustVolumes.Add(Volume);
			DustSet.Add(0.0f);
		}
	}

	// The windows that let the sky in: which side is the room is the side the sim lights through it.
	struct FWinCandidate
	{
		FVector Position;
		FVector Inward;
		double Distance;
	};
	TArray<FWinCandidate> Candidates;
	const colony::Map& Map = Sim.map();
	const colony::Lights& SimLights = Sim.lights();
	const colony::WindowNumbers& Numbers = Sim.windows().numbers();
	const float MidCm = (static_cast<float>(Numbers.sillDm) + static_cast<float>(Numbers.lintelDm)) * 5.0f;
	FVector Camera = FVector::ZeroVector;
	if (const APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0))
	{
		Camera = CameraManager->GetCameraLocation();
	}
	const float Sky = SimLights.skyGlow();
	const std::vector<colony::Room>& RoomList = Sim.rooms().rooms();
	TArray<int32> DustRoomIds;
	if (Sky > 0.01f)
	{
		constexpr int32 Sides[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
		for (const colony::ThingView& View : Source->GetCurrentSnapshot().things)
		{
			if (View.window == colony::WindowState::None || View.level != 0)
			{
				continue;
			}
			const int32 X = FMath::FloorToInt32(View.x);
			const int32 Y = FMath::FloorToInt32(View.y);
			int32 Best = -1;
			float BestShare = 0.0f;
			for (int32 S = 0; S < 4; ++S)
			{
				const colony::CellPos In{X + Sides[S][0], Y + Sides[S][1], 0};
				const colony::CellPos Out{X - Sides[S][0], Y - Sides[S][1], 0};
				const float Share = Map.inBounds(In) && SimLights.underOpenSky(Out) ? SimLights.windowShareAt(In) : 0.0f;
				if (Share > BestShare)
				{
					BestShare = Share;
					Best = S;
				}
			}
			if (Best < 0)
			{
				continue;
			}
			// The room behind the window, for its dust: a closed room with a roof all over.
			const colony::CellPos Inside{X + Sides[Best][0], Y + Sides[Best][1], 0};
			for (int32 R = 0; R < static_cast<int32>(RoomList.size()); ++R)
			{
				const colony::Room& Room = RoomList[static_cast<size_t>(R)];
				if (Room.enclosed() && Room.openRoofCount == 0 && Room.bounds.contains(Inside))
				{
					DustRoomIds.AddUnique(R);
					break;
				}
			}
			const FVector Inward(static_cast<double>(Sides[Best][0]), static_cast<double>(Sides[Best][1]), 0.0);
			const FVector Centre = Source->SimToWorld(static_cast<float>(X) + 0.5f, static_cast<float>(Y) + 0.5f, 0);
			const FVector Base(Centre.X, Centre.Y, Source->GetWorldOrigin().Z + static_cast<double>(Map.groundHeight(colony::CellPos{X, Y, 0})) * 100.0);
			FWinCandidate Candidate;
			Candidate.Position = Base + FVector(0.0, 0.0, MidCm) + Inward * (WinWallHalfCm + 2.0);
			Candidate.Inward = Inward;
			Candidate.Distance = FVector::DistSquared(Candidate.Position, Camera);
			Candidates.Add(Candidate);
		}
		Candidates.Sort([](const FWinCandidate& A, const FWinCandidate& B) { return A.Distance < B.Distance; });
	}

	const int32 Wanted = FMath::Clamp(GWinMaxLights.GetValueOnGameThread(), 0, Lights.Num());
	const int32 ShadowCount = FMath::Max(0, GWinShadowLights.GetValueOnGameThread());
	const float Opening = 70.0f;
	const float Tall = FMath::Max(20.0f, (static_cast<float>(Numbers.lintelDm) - static_cast<float>(Numbers.sillDm)) * 10.0f - 10.0f);
	const float Candelas = FMath::Max(0.0f, GWinLightCd.GetValueOnGameThread()) * Sky;
	Shining = 0;
	for (int32 I = 0; I < Lights.Num(); ++I)
	{
		URectLightComponent* Light = Lights[I];
		if (!Light)
		{
			continue;
		}
		if (I >= Wanted || I >= Candidates.Num())
		{
			if (Light->IsVisible())
			{
				Light->SetVisibility(false);
			}
			continue;
		}
		const FWinCandidate& Candidate = Candidates[I];
		Light->SetWorldLocationAndRotation(Candidate.Position, Candidate.Inward.Rotation());
		Light->SetSourceWidth(Opening);
		Light->SetSourceHeight(Tall);
		Light->SetIntensity(Candelas);
		Light->SetCastShadows(I < ShadowCount);
		if (!Light->IsVisible())
		{
			Light->SetVisibility(true);
		}
		++Shining;
	}

	// Dust in the rooms behind the windows, nearest the camera first, while the camera is indoors (outdoors none of it is
	// wanted, and the overview camera never sees it). Each room gets a local fog volume: the ellipsoid inside its bounds from the
	// floor to the roof, a little haze of even density, which the volumetric fog lights where the sun comes in through a window.
	// (A volume material on a box does nothing: the volumetric fog takes volume materials from particles only.)
	const USkySubsystem* SkyNow = GetWorld()->GetSubsystem<USkySubsystem>();
	const float Indoors = SkyNow ? SkyNow->GetIndoorShare() : 0.0f;
	const float Dust = FMath::Max(0.0f, GWinDust.GetValueOnGameThread()) * Indoors;
	const int32 DustWanted = Dust > 0.001f ? FMath::Clamp(GWinDustRooms.GetValueOnGameThread(), 0, DustVolumes.Num()) : 0;
	const FVector Origin = Source->GetWorldOrigin();
	DustRoomIds.Sort([&RoomList, &Origin, &Camera](int32 A, int32 B)
	{
		const auto Far = [&RoomList, &Origin, &Camera](int32 Id)
		{
			const colony::CellRect& R = RoomList[static_cast<size_t>(Id)].bounds;
			const FVector RoomMid = Origin + FVector((R.minX + R.sizeX * 0.5) * 100.0, (R.minY + R.sizeY * 0.5) * 100.0, 0.0);
			return FVector::DistSquared2D(RoomMid, Camera);
		};
		return Far(A) < Far(B);
	});
	DustRooms = 0;
	for (int32 I = 0; I < DustVolumes.Num(); ++I)
	{
		ULocalFogVolumeComponent* Volume = DustVolumes[I];
		if (!Volume)
		{
			continue;
		}
		if (I >= DustWanted || I >= DustRoomIds.Num())
		{
			if (Volume->IsVisible())
			{
				Volume->SetVisibility(false);
			}
			continue;
		}
		const colony::Room& Room = RoomList[static_cast<size_t>(DustRoomIds[I])];
		const colony::CellRect& R = Room.bounds;
		const double FloorZ = Origin.Z + static_cast<double>(Map.groundHeight(Room.anchor)) * 100.0;
		const double TallCm = TerrainDraw::RoofHeightM * 100.0;
		const FVector RoomCentre(Origin.X + (R.minX + R.sizeX * 0.5) * 100.0, Origin.Y + (R.minY + R.sizeY * 0.5) * 100.0, FloorZ + TallCm * 0.5);
		const FVector RoomScale = FVector(R.sizeX * 50.0, R.sizeY * 50.0, TallCm * 0.5) * WinDustOversize / FMath::Max(1.0, static_cast<double>(GWinDustBaseCm.GetValueOnGameThread()));
		if (!Volume->GetComponentLocation().Equals(RoomCentre, 1.0) || !Volume->GetComponentScale().Equals(RoomScale, 0.001))
		{
			Volume->SetWorldLocationAndRotation(RoomCentre, FRotator::ZeroRotator);
			Volume->SetWorldScale3D(RoomScale);
		}
		// Radial fog: its density falls off towards the ellipsoid's edge, so the ellipsoid is a little bigger than the room. (Height
		// fog with no falloff drew nothing, PC round 6.)
		if (DustSet.IsValidIndex(I) && !FMath::IsNearlyEqual(DustSet[I], Dust, 0.002f))
		{
			DustSet[I] = Dust;
			Volume->SetRadialFogExtinction(Dust);
		}
		if (!Volume->IsVisible())
		{
			Volume->SetVisibility(true);
		}
		++DustRooms;
	}
}

FString UWindowLightSubsystem::DescribeDust() const
{
	FString Out;
	for (const ULocalFogVolumeComponent* Volume : DustVolumes)
	{
		if (Volume && Volume->IsVisible())
		{
			const FVector At = Volume->GetComponentLocation();
			const FVector Scale = Volume->GetComponentScale();
			Out += FString::Printf(TEXT("[at %.0f %.0f %.0f cm, scale %.2f %.2f %.2f, bounds %.0f cm, radial %.2f, height %.2f, registered %d] "), At.X, At.Y, At.Z,
				Scale.X, Scale.Y, Scale.Z, Volume->Bounds.SphereRadius, Volume->RadialFogExtinction, Volume->HeightFogExtinction, Volume->IsRegistered() ? 1 : 0);
		}
	}
	return Out.IsEmpty() ? FString(TEXT("none")) : Out;
}
