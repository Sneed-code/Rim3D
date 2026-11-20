#include "WindowLightSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/RectLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "SimSubsystem.h"
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
	TAutoConsoleVariable<float> GWinDust(TEXT("Windows.Dust"), 0.25f,
		TEXT("Dust in the air of rooms with windows, as volumetric fog extinction per metre: sunbeams through windows show as shafts. 0 off."), ECVF_Default);
	TAutoConsoleVariable<int32> GWinDustRooms(TEXT("Windows.DustRooms"), 8, TEXT("How many rooms nearest the camera get dust in their air."), ECVF_Default);
	const TCHAR* const WinDustMaterialPath = TEXT("/Game/Rim3D/Generated/Buildings/M_RoomDust.M_RoomDust");
	const TCHAR* const WinDustCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
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
	DustBoxes.Reset();
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
		DustMaterial = LoadObject<UMaterialInterface>(nullptr, WinDustMaterialPath);
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, WinDustCubePath);
		if (DustMaterial && Cube)
		{
			for (int32 I = 0; I < WinDustPool; ++I)
			{
				UStaticMeshComponent* Box = NewObject<UStaticMeshComponent>(Holder);
				Box->SetMobility(EComponentMobility::Movable);
				Box->SetStaticMesh(Cube);
				Box->SetMaterial(0, UMaterialInstanceDynamic::Create(DustMaterial, Box));
				Box->SetCastShadow(false);
				Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Box->SetCanEverAffectNavigation(false);
				Box->SetupAttachment(Root);
				Box->RegisterComponent();
				Box->SetVisibility(false);
				DustBoxes.Add(Box);
			}
		}
		else if (!bDustMissingLogged)
		{
			bDustMissingLogged = true;
			UE_LOG(LogTemp, Warning, TEXT("WindowLights: no %s (run the BuildingMaterials commandlet); rooms get no dust, so no light shafts"), WinDustMaterialPath);
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

	// Dust in the rooms behind the windows, nearest the camera first. The box fills the room's bounds from the floor to the
	// roof, so its walls hold the haze in; the volumetric fog is lit by the sun only where it comes in through a window.
	const float Dust = FMath::Max(0.0f, GWinDust.GetValueOnGameThread());
	const int32 DustWanted = Dust > 0.0f ? FMath::Clamp(GWinDustRooms.GetValueOnGameThread(), 0, DustBoxes.Num()) : 0;
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
	for (int32 I = 0; I < DustBoxes.Num(); ++I)
	{
		UStaticMeshComponent* Box = DustBoxes[I];
		if (!Box)
		{
			continue;
		}
		if (I >= DustWanted || I >= DustRoomIds.Num())
		{
			if (Box->IsVisible())
			{
				Box->SetVisibility(false);
			}
			continue;
		}
		const colony::Room& Room = RoomList[static_cast<size_t>(DustRoomIds[I])];
		const colony::CellRect& R = Room.bounds;
		const double FloorZ = Origin.Z + static_cast<double>(Map.groundHeight(Room.anchor)) * 100.0;
		const double TallCm = TerrainDraw::RoofHeightM * 100.0;
		// The engine cube is 100 cm across and centred on its origin.
		Box->SetWorldLocation(Origin + FVector((R.minX + R.sizeX * 0.5) * 100.0, (R.minY + R.sizeY * 0.5) * 100.0, 0.0) + FVector(0.0, 0.0, FloorZ - Origin.Z + TallCm * 0.5));
		Box->SetWorldScale3D(FVector(R.sizeX, R.sizeY, TallCm / 100.0));
		if (UMaterialInstanceDynamic* DustMid = Cast<UMaterialInstanceDynamic>(Box->GetMaterial(0)))
		{
			DustMid->SetScalarParameterValue(TEXT("Extinction"), Dust);
		}
		if (!Box->IsVisible())
		{
			Box->SetVisibility(true);
		}
		++DustRooms;
	}
}
