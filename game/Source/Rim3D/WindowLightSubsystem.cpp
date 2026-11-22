#include "WindowLightSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/RectLightComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "SimSubsystem.h"

#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
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
	Shining = 0;
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
			// Not in the volumetric fog: the room's dust is lit by the sun's beams, not by a glow at every window.
			Light->SetVolumetricScatteringIntensity(0.0f);
			Light->SetupAttachment(Root);
			Light->RegisterComponent();
			Light->SetVisibility(false);
			Lights.Add(Light);
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
}
