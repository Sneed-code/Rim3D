#include "ShuttleSubsystem.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "SimSubsystem.h"
#include "TerrainSubsystem.h"
#include "VisualMesh.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

DEFINE_LOG_CATEGORY_STATIC(LogShuttle, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GShuttleEnable(
		TEXT("Shuttle.Enable"), 1, TEXT("1 draws the sim's shuttles (model, flight, flames, dust), 0 draws none."), ECVF_Default);
	TAutoConsoleVariable<int32> GShuttleDust(
		TEXT("Shuttle.Dust"), 1, TEXT("1 blows dust out from under a low shuttle's jets, 0 none."), ECVF_Default);
	TAutoConsoleVariable<int32> GShuttleDebug(
		TEXT("Shuttle.Debug"), 0, TEXT("1 logs each shuttle's phase, progress and pose once a second."), ECVF_Default);

	const TCHAR* const ShuttleSolidMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");
	const TCHAR* const ShuttleFlameMaterialPath = TEXT("/Game/Rim3D/Generated/Vehicles/M_JetFlame.M_JetFlame");
	const TCHAR* const ShuttleDustMaterialPath = TEXT("/Game/Rim3D/Generated/Vehicles/M_JetDust.M_JetDust");
	const TCHAR* const ShuttleDownwashPath = TEXT("/Game/Rim3D/Generated/Foliage/MPC_Downwash.MPC_Downwash");

	// The shared flame cone (radius 50 cm, 100 cm long, along +Z) and dust puff (a 50 cm sphere), scaled per instance.
	constexpr float ShuttleConeRadiusCm = 50.0f;
	constexpr float ShuttleConeLengthCm = 100.0f;
	constexpr float ShuttlePuffRadiusCm = 50.0f;

	float ShuttleSmooth(float X)
	{
		X = FMath::Clamp(X, 0.0f, 1.0f);
		return X * X * (3.0f - 2.0f * X);
	}

	float ShuttleSpan(float P, float From, float Length)
	{
		return ShuttleSmooth((P - From) / FMath::Max(Length, 1e-4f));
	}

	double ShuttleNum(const TSharedPtr<FJsonObject>& J, const TCHAR* Key, double Fallback)
	{
		double Value = Fallback;
		return J.IsValid() && J->TryGetNumberField(Key, Value) ? Value : Fallback;
	}

	FVector ShuttleVec(const TArray<TSharedPtr<FJsonValue>>& A, const FVector& Fallback)
	{
		return A.Num() >= 3 ? FVector(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber()) : Fallback;
	}

	FVector ShuttleVecField(const TSharedPtr<FJsonObject>& J, const TCHAR* Key, const FVector& Fallback)
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		return J.IsValid() && J->TryGetArrayField(Key, A) ? ShuttleVec(*A, Fallback) : Fallback;
	}

	FRotator ShuttleRotField(const TSharedPtr<FJsonObject>& J, const TCHAR* Key, const FRotator& Fallback)
	{
		const FVector V = ShuttleVecField(J, Key, FVector(Fallback.Pitch, Fallback.Yaw, Fallback.Roll));
		return FRotator(V.X, V.Y, V.Z);
	}

	FLinearColor ShuttleColorField(const TSharedPtr<FJsonObject>& J, const TCHAR* Key, const FLinearColor& Fallback)
	{
		const FVector V = ShuttleVecField(J, Key, FVector(Fallback.R, Fallback.G, Fallback.B));
		return FLinearColor(V.X, V.Y, V.Z);
	}

	TArray<FVector> ShuttlePoints(const TSharedPtr<FJsonObject>& J, const TCHAR* Key)
	{
		TArray<FVector> Out;
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (J.IsValid() && J->TryGetArrayField(Key, A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
				if (V.IsValid() && V->TryGetArray(P))
				{
					Out.Add(ShuttleVec(*P, FVector::ZeroVector));
				}
			}
		}
		return Out;
	}

	UStaticMeshComponent* ShuttleMeshComponent(AActor* Owner, USceneComponent* Parent, UStaticMesh* Mesh)
	{
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Owner);
		Component->SetStaticMesh(Mesh);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCanEverAffectNavigation(false);
		Component->SetupAttachment(Parent);
		Component->RegisterComponent();
		return Component;
	}
}

void UShuttleSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Bridge = Collection.InitializeDependency<USimSubsystem>();
}

void UShuttleSubsystem::Deinitialize()
{
	ClearAll();
	if (Holder)
	{
		Holder->Destroy();
		Holder = nullptr;
	}
	Super::Deinitialize();
}

TStatId UShuttleSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UShuttleSubsystem, STATGROUP_Tickables);
}

bool UShuttleSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UShuttleSubsystem::ReadConfig()
{
	bConfigRead = true;
	Config = FConfig();
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../assets/shuttle.json")));
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *File) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogShuttle, Warning, TEXT("Cannot read %s: shuttles are drawn from their def's box parts with the built-in numbers"), *File);
		return;
	}
	const TSharedPtr<FJsonObject>* ShuttlePtr = nullptr;
	if (!Root->TryGetObjectField(TEXT("shuttle"), ShuttlePtr))
	{
		return;
	}
	const TSharedPtr<FJsonObject> J = *ShuttlePtr;
	J->TryGetStringField(TEXT("art"), Config.Art);
	J->TryGetStringField(TEXT("artRoot"), Config.ArtRoot);
	Config.ModelCentre = ShuttleVecField(J, TEXT("modelCentre"), FVector::ZeroVector);
	Config.GroundLift = static_cast<float>(ShuttleNum(J, TEXT("groundLift"), 0.0));
	const TSharedPtr<FJsonObject>* PartsPtr = nullptr;
	if (J->TryGetObjectField(TEXT("parts"), PartsPtr))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PartsPtr)->Values)
		{
			const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
			if (Pair.Value.IsValid() && Pair.Value->TryGetArray(A))
			{
				Config.Parts.Emplace(Pair.Key, ShuttleVec(*A, FVector::ZeroVector));
			}
		}
	}
	Config.LiftJets = ShuttlePoints(J, TEXT("liftJets"));
	Config.RearJets = ShuttlePoints(J, TEXT("rearJets"));
	const TSharedPtr<FJsonObject>* GearPtr = nullptr;
	if (J->TryGetObjectField(TEXT("gearUp"), GearPtr))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*GearPtr)->Values)
		{
			Config.GearUp.Add(Pair.Key, ShuttleRotField(*GearPtr, *Pair.Key, FRotator::ZeroRotator));
		}
	}
	Config.NacelleHover = ShuttleRotField(J, TEXT("nacelleHover"), Config.NacelleHover);
	Config.VaneTilt = static_cast<float>(ShuttleNum(J, TEXT("liftVaneTilt"), Config.VaneTilt));
	Config.DoorOpen = ShuttleVecField(J, TEXT("doorOpen"), Config.DoorOpen);
	Config.StepStowed = ShuttleRotField(J, TEXT("stepStowed"), Config.StepStowed);

	const TSharedPtr<FJsonObject>* Sub = nullptr;
	if (J->TryGetObjectField(TEXT("flight"), Sub))
	{
		const TSharedPtr<FJsonObject> F = *Sub;
		Config.ApproachM = static_cast<float>(ShuttleNum(F, TEXT("approachM"), Config.ApproachM));
		Config.CruiseM = static_cast<float>(ShuttleNum(F, TEXT("cruiseHeightM"), Config.CruiseM));
		Config.HoverM = static_cast<float>(ShuttleNum(F, TEXT("hoverHeightM"), Config.HoverM));
		Config.FlareDeg = static_cast<float>(ShuttleNum(F, TEXT("flareDeg"), Config.FlareDeg));
		Config.BobCm = static_cast<float>(ShuttleNum(F, TEXT("hoverBobCm"), Config.BobCm));
		Config.WobbleDeg = static_cast<float>(ShuttleNum(F, TEXT("hoverWobbleDeg"), Config.WobbleDeg));
		Config.DoorAfterS = static_cast<float>(ShuttleNum(F, TEXT("doorOpenAfterS"), Config.DoorAfterS));
		Config.SpoolDownS = static_cast<float>(ShuttleNum(F, TEXT("spoolDownS"), Config.SpoolDownS));
	}
	if (J->TryGetObjectField(TEXT("flames"), Sub))
	{
		const TSharedPtr<FJsonObject> F = *Sub;
		Config.LiftLengthCm = static_cast<float>(ShuttleNum(F, TEXT("liftLengthCm"), Config.LiftLengthCm));
		Config.LiftRadiusCm = static_cast<float>(ShuttleNum(F, TEXT("liftRadiusCm"), Config.LiftRadiusCm));
		Config.RearLengthCm = static_cast<float>(ShuttleNum(F, TEXT("rearLengthCm"), Config.RearLengthCm));
		Config.RearRadiusCm = static_cast<float>(ShuttleNum(F, TEXT("rearRadiusCm"), Config.RearRadiusCm));
		Config.CoreColor = ShuttleColorField(F, TEXT("coreColor"), Config.CoreColor);
		Config.OuterColor = ShuttleColorField(F, TEXT("outerColor"), Config.OuterColor);
		Config.Brightness = static_cast<float>(ShuttleNum(F, TEXT("brightness"), Config.Brightness));
		Config.LightCandelas = static_cast<float>(ShuttleNum(F, TEXT("lightCandelas"), Config.LightCandelas));
		Config.LightRadiusCm = static_cast<float>(ShuttleNum(F, TEXT("lightRadiusCm"), Config.LightRadiusCm));
	}
	if (J->TryGetObjectField(TEXT("dust"), Sub))
	{
		const TSharedPtr<FJsonObject> F = *Sub;
		Config.DustFromM = static_cast<float>(ShuttleNum(F, TEXT("fromHeightM"), Config.DustFromM));
		Config.DustPerSecond = static_cast<float>(ShuttleNum(F, TEXT("perSecond"), Config.DustPerSecond));
		Config.DustLife = static_cast<float>(ShuttleNum(F, TEXT("lifeS"), Config.DustLife));
		Config.DustSpeed = static_cast<float>(ShuttleNum(F, TEXT("speedCmS"), Config.DustSpeed));
		Config.DustSize = static_cast<float>(ShuttleNum(F, TEXT("sizeCm"), Config.DustSize));
		Config.DustGrow = static_cast<float>(ShuttleNum(F, TEXT("growCm"), Config.DustGrow));
		Config.DustOpacity = static_cast<float>(ShuttleNum(F, TEXT("opacity"), Config.DustOpacity));
		Config.DustColor = ShuttleColorField(F, TEXT("color"), Config.DustColor);
		Config.DustMax = static_cast<int32>(ShuttleNum(F, TEXT("max"), Config.DustMax));
	}
	if (J->TryGetObjectField(TEXT("grass"), Sub))
	{
		Config.GrassRadiusM = static_cast<float>(ShuttleNum(*Sub, TEXT("radiusM"), Config.GrassRadiusM));
		Config.GrassFromM = static_cast<float>(ShuttleNum(*Sub, TEXT("fromHeightM"), Config.GrassFromM));
	}
}

void UShuttleSubsystem::ReloadConfig()
{
	ClearAll();
	ReadConfig();
	UE_LOG(LogShuttle, Display, TEXT("Read assets/shuttle.json again"));
}

void UShuttleSubsystem::EnsureScene()
{
	if (Holder || !GetWorld())
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("ShuttleEffectsRoot"));
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	Holder->SetActorLabel(TEXT("ShuttleEffects"));
#endif
	UMaterialInterface* Solid = LoadObject<UMaterialInterface>(nullptr, ShuttleSolidMaterialPath);
	UMaterialInterface* FlameMaterial = LoadObject<UMaterialInterface>(nullptr, ShuttleFlameMaterialPath);
	UMaterialInterface* DustMaterial = LoadObject<UMaterialInterface>(nullptr, ShuttleDustMaterialPath);
	if (!FlameMaterial || !DustMaterial)
	{
		UE_LOG(LogShuttle, Warning, TEXT("%s or %s not found: jet flames and dust are drawn plain. Run the ShuttleArt commandlet."), ShuttleFlameMaterialPath, ShuttleDustMaterialPath);
	}
	Downwash = LoadObject<UMaterialParameterCollection>(nullptr, ShuttleDownwashPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Downwash)
	{
		UE_LOG(LogShuttle, Warning, TEXT("%s not found: the grass does not lean away from shuttles. Run the FoliageMaterials commandlet."), ShuttleDownwashPath);
	}
	const auto MakeShape = [this, Solid](VisualMesh::EShape Shape, float Radius, float Height, const FLinearColor& Color) -> UStaticMesh*
	{
		TArray<VisualMesh::FPart> Parts;
		VisualMesh::FPart& Part = Parts.AddDefaulted_GetRef();
		Part.Shape = Shape;
		Part.Size = FVector3f(Radius, Radius, Height);
		Part.At = FVector3f(0.0f, 0.0f, Shape == VisualMesh::EShape::Sphere ? -Height * 0.5f : 0.0f);
		Part.Color = Color;
		UStaticMesh* Mesh = VisualMesh::MakeMesh(this, Solid, Parts, VisualMesh::EPartSet::All, nullptr, false);
		if (Mesh)
		{
			Keep.Add(Mesh);
		}
		return Mesh;
	};
	const auto MakeInstanced = [this, Root](UStaticMesh* Mesh, UMaterialInterface* Material, const TCHAR* Name) -> UInstancedStaticMeshComponent*
	{
		UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(Holder, Name);
		Component->SetStaticMesh(Mesh);
		if (Material)
		{
			Component->SetMaterial(0, Material);
		}
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCanEverAffectNavigation(false);
		Component->SetCastShadow(false);
		Component->bReceivesDecals = false;
		Component->bUseAsOccluder = false;
		Component->NumCustomDataFloats = 1;
		Component->SetupAttachment(Root);
		Component->RegisterComponent();
		return Component;
	};
	FlameCore = MakeInstanced(MakeShape(VisualMesh::EShape::Cone, ShuttleConeRadiusCm, ShuttleConeLengthCm, Config.CoreColor), FlameMaterial, TEXT("FlameCore"));
	FlameOuter = MakeInstanced(MakeShape(VisualMesh::EShape::Cone, ShuttleConeRadiusCm, ShuttleConeLengthCm, Config.OuterColor), FlameMaterial, TEXT("FlameOuter"));
	DustMesh = MakeInstanced(MakeShape(VisualMesh::EShape::Sphere, ShuttlePuffRadiusCm, ShuttlePuffRadiusCm * 2.0f, Config.DustColor), DustMaterial, TEXT("Dust"));
}

FVector UShuttleSubsystem::PartPivot(const FShuttle& S, const TCHAR* Name) const
{
	for (const FPart& Part : S.Parts)
	{
		if (Part.Name == Name)
		{
			return Part.PivotCm;
		}
	}
	return FVector::ZeroVector;
}

void UShuttleSubsystem::MakeShuttle(FShuttle& S, const USimSubsystem& Source, uint32 DefValue)
{
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	S.Actor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	USceneComponent* Root = NewObject<USceneComponent>(S.Actor, TEXT("ShuttleRoot"));
	Root->SetMobility(EComponentMobility::Movable);
	S.Actor->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	S.Actor->SetActorLabel(TEXT("Shuttle"));
#endif
	// The model's frame: the middle of its footprint over the actor, its skids on the ground.
	S.Model = NewObject<USceneComponent>(S.Actor, TEXT("ShuttleModel"));
	S.Model->SetMobility(EComponentMobility::Movable);
	S.Model->SetupAttachment(Root);
	S.Model->SetRelativeLocation(FVector(-Config.ModelCentre.X, -Config.ModelCentre.Y, Config.GroundLift - Config.ModelCentre.Z));
	S.Model->RegisterComponent();

	// The parts: each mesh's vertices are in the model's frame, so it sits under a pivot at its pivot point, offset back.
	TArray<FAssetData> Assets;
	if (!Config.Art.IsEmpty())
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.GetAssetsByPath(FName(Config.ArtRoot / Config.Art), Assets, true);
	}
	int32 Found = 0;
	for (const TPair<FString, FVector>& Entry : Config.Parts)
	{
		UStaticMesh* Mesh = nullptr;
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName() && Asset.AssetName.ToString().EndsWith(Entry.Key))
			{
				Mesh = Cast<UStaticMesh>(Asset.GetAsset());
				break;
			}
		}
		if (!Mesh)
		{
			continue;
		}
		FPart& Part = S.Parts.AddDefaulted_GetRef();
		Part.Name = Entry.Key;
		Part.PivotCm = Entry.Value;
		Part.Pivot = NewObject<USceneComponent>(S.Actor);
		Part.Pivot->SetMobility(EComponentMobility::Movable);
		Part.Pivot->SetupAttachment(S.Model);
		Part.Pivot->SetRelativeLocation(Entry.Value);
		Part.Pivot->RegisterComponent();
		Part.Mesh = ShuttleMeshComponent(S.Actor, Part.Pivot, Mesh);
		Part.Mesh->SetRelativeLocation(-Entry.Value);
		++Found;
	}
	bHasArt = Found > 0;
	if (!bHasArt)
	{
		// No model: the def's box parts (made facing north, nose to +y), turned to the model's frame (nose to +x).
		UE_LOG(LogShuttle, Warning, TEXT("No shuttle model in %s/%s (run GunArt -List=assets/shuttle.json): its box parts stand in"), *Config.ArtRoot, *Config.Art);
		const colony::Sim* Sim = Source.GetSim();
		TArray<VisualMesh::FPart> Parts;
		if (Sim && DefValue < Sim->defs().size())
		{
			const colony::Def& Def = Sim->defs().get(colony::DefId{DefValue});
			const FVector2f Footprint(static_cast<float>(Def.numberAt("size", 0, 6.0)) * 100.0f, static_cast<float>(Def.numberAt("size", 1, 12.0)) * 100.0f);
			VisualMesh::ReadParts(Def, "visual", Footprint, Parts);
		}
		if (UStaticMesh* Mesh = VisualMesh::MakeMesh(this, LoadObject<UMaterialInterface>(nullptr, ShuttleSolidMaterialPath), Parts, VisualMesh::EPartSet::All, nullptr, false))
		{
			Keep.Add(Mesh);
			FPart& Part = S.Parts.AddDefaulted_GetRef();
			Part.Name = TEXT("Body");
			Part.Pivot = NewObject<USceneComponent>(S.Actor);
			Part.Pivot->SetMobility(EComponentMobility::Movable);
			Part.Pivot->SetupAttachment(Root);
			Part.Pivot->RegisterComponent();
			Part.Mesh = ShuttleMeshComponent(S.Actor, Part.Pivot, Mesh);
			Part.Mesh->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));
		}
	}

	// One light under the hull, lit by the jets.
	S.Light = NewObject<UPointLightComponent>(S.Actor);
	S.Light->SetMobility(EComponentMobility::Movable);
	S.Light->SetIntensityUnits(ELightUnits::Candelas);
	S.Light->SetIntensity(0.0f);
	S.Light->SetAttenuationRadius(Config.LightRadiusCm);
	S.Light->SetSourceRadius(60.0f);
	S.Light->SetCastShadows(false);
	S.Light->SetLightColor(FMath::Lerp(Config.CoreColor, Config.OuterColor, 0.5f));
	S.Light->SetupAttachment(S.Model);
	S.Light->SetRelativeLocation(FVector(0.0f, 0.0f, 60.0f));
	S.Light->RegisterComponent();
	S.FlickerSeed = static_cast<float>(Shuttles.Num()) * 3.7f + 0.5f;
}

void UShuttleSubsystem::DestroyShuttle(FShuttle& S)
{
	if (S.Actor)
	{
		S.Actor->Destroy();
		S.Actor = nullptr;
	}
	S.Parts.Reset();
}

void UShuttleSubsystem::ClearAll()
{
	for (TPair<uint32, FShuttle>& Pair : Shuttles)
	{
		DestroyShuttle(Pair.Value);
	}
	Shuttles.Reset();
	Dust.Reset();
	for (UInstancedStaticMeshComponent* Component : {FlameCore.Get(), FlameOuter.Get(), DustMesh.Get()})
	{
		if (Component)
		{
			Component->ClearInstances();
		}
	}
	if (Downwash && GetWorld())
	{
		if (UMaterialParameterCollectionInstance* Instance = GetWorld()->GetParameterCollectionInstance(Downwash))
		{
			Instance->SetVectorParameterValue(TEXT("Wash0"), FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
			Instance->SetVectorParameterValue(TEXT("Wash1"), FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
		}
	}
	WashStrength = 0.0f;
}

FShuttlePose UShuttleSubsystem::PoseAt(int32 Phase, float P, float LandedSeconds, float HoverSeconds) const
{
	const FConfig& C = Config;
	FShuttlePose Pose;
	P = FMath::Clamp(P, 0.0f, 1.0f);
	// Tiny corrective moves while it hangs on its jets (the model's HoverIdle).
	const float Wobble = FMath::Sin(HoverSeconds * 1.7f) * 0.6f + FMath::Sin(HoverSeconds * 2.9f + 1.3f) * 0.4f;
	const float Bob = FMath::Sin(HoverSeconds * 1.3f) * C.BobCm / 100.0f;
	if (Phase == 0)
	{
		// In from behind in forward flight, braking (nose up) to a hover over the spot, then straight down; the rear
		// engines swing down as it slows, the gear comes down near the ground.
		constexpr float Approach = 0.45f;
		Pose.Nacelles = ShuttleSpan(P, 0.2f, 0.25f);
		if (P < Approach)
		{
			const float S = P / Approach;
			Pose.BackM = C.ApproachM * FMath::Square(1.0f - S);
			Pose.UpM = C.HoverM + (C.CruiseM - C.HoverM) * (1.0f - ShuttleSmooth(S)) + Bob * S;
			Pose.PitchDeg = C.FlareDeg * FMath::Sin(PI * S);
			Pose.VaneDeg = -C.VaneTilt * FMath::Sin(PI * S);
			Pose.Lift = 0.3f + 0.7f * ShuttleSmooth(S);
			Pose.Rear = 1.0f - 0.55f * Pose.Nacelles;
			Pose.Gear = 0.0f;
		}
		else
		{
			const float T = (P - Approach) / (1.0f - Approach);
			Pose.UpM = C.HoverM * (1.0f - ShuttleSmooth(T)) + Bob * (1.0f - T);
			Pose.PitchDeg = C.WobbleDeg * Wobble * (1.0f - T);
			Pose.RollDeg = C.WobbleDeg * 0.7f * FMath::Sin(HoverSeconds * 2.1f) * (1.0f - T);
			Pose.VaneDeg = 2.5f * Wobble * (1.0f - T);
			Pose.Lift = 1.0f - 0.15f * ShuttleSpan(P, 0.95f, 0.05f);
			Pose.Rear = 0.45f;
			Pose.Gear = ShuttleSpan(P, 0.62f, 0.167f);
		}
	}
	else if (Phase == 1)
	{
		// Down: the jets spool down, then the door slides open and the step folds out.
		const float Spool = 1.0f - FMath::Clamp(LandedSeconds / FMath::Max(C.SpoolDownS, 0.1f), 0.0f, 1.0f);
		Pose.Lift = 0.85f * Spool;
		Pose.Rear = 0.45f * Spool;
		Pose.Door = ShuttleSmooth((LandedSeconds - C.DoorAfterS) / 1.5f);
	}
	else
	{
		// Up: door shut, jets up, straight up to a hover, gear in, rear engines swing back, then off forward, climbing.
		constexpr float Climb = 0.5f;
		Pose.Door = 1.0f - ShuttleSmooth(P * 10.0f / 1.5f);
		const float Spool = ShuttleSpan(P, 0.03f, 0.12f);
		Pose.Gear = 1.0f - ShuttleSpan(P, 0.28f, 0.2f);
		Pose.Nacelles = 1.0f - ShuttleSpan(P, 0.48f, 0.3f);
		if (P < Climb)
		{
			const float T = ShuttleSpan(P, 0.15f, Climb - 0.15f);
			Pose.UpM = C.HoverM * T + Bob * T;
			Pose.PitchDeg = C.WobbleDeg * Wobble * T;
			Pose.VaneDeg = 2.5f * Wobble * T;
		}
		else
		{
			const float S = (P - Climb) / (1.0f - Climb);
			Pose.BackM = -C.ApproachM * S * S;
			Pose.UpM = C.HoverM + (C.CruiseM - C.HoverM) * ShuttleSmooth(S) + Bob * (1.0f - S);
			Pose.PitchDeg = -C.FlareDeg * 0.6f * FMath::Sin(PI * S);
			Pose.VaneDeg = C.VaneTilt * FMath::Sin(PI * S);
		}
		Pose.Lift = Spool * (1.0f - 0.7f * (1.0f - Pose.Nacelles));
		Pose.Rear = Spool * (0.45f + 0.55f * (1.0f - Pose.Nacelles));
	}
	return Pose;
}

void UShuttleSubsystem::PoseShuttle(FShuttle& S, const colony::ShuttleView& View, float Progress, const USimSubsystem& Source, float GameDelta, float RealTime)
{
	if (S.Phase != static_cast<int32>(View.phase))
	{
		// A shuttle first seen already down (a loaded game) does not play its landing again.
		S.LandedSeconds = S.Phase < 0 && View.phase == 1 ? 1000.0f : 0.0f;
		S.Phase = View.phase;
	}
	S.LandedSeconds += GameDelta;
	S.HoverSeconds += GameDelta;
	S.Pose = PoseAt(S.Phase, Progress, S.LandedSeconds, S.HoverSeconds);
	const FShuttlePose& Pose = S.Pose;

	// Rot4 0 (north) has the nose to +y, 1 (east) to +x, and so on clockwise; the model's nose is +x.
	const float Yaw = 90.0f - 90.0f * static_cast<float>(View.rot);
	const FVector Forward(FMath::Cos(FMath::DegreesToRadians(Yaw)), FMath::Sin(FMath::DegreesToRadians(Yaw)), 0.0);
	const FVector Spot = Source.GetWorldOrigin() + FVector(View.x * 100.0, View.y * 100.0, View.ground * 100.0);
	const FVector Location = Spot - Forward * (Pose.BackM * 100.0) + FVector(0.0, 0.0, Pose.UpM * 100.0);
	S.Actor->SetActorLocationAndRotation(Location, FRotator(Pose.PitchDeg, Yaw, Pose.RollDeg));

	// The moving parts.
	UTerrainSubsystem* Terrain = GetWorld()->GetSubsystem<UTerrainSubsystem>();
	const bool bHideRoof = S.Phase == 1 && Terrain && !Terrain->AreRoofsShown();
	for (FPart& Part : S.Parts)
	{
		if (!Part.Pivot)
		{
			continue;
		}
		FRotator Rotation = FRotator::ZeroRotator;
		FVector Offset = FVector::ZeroVector;
		if (const FRotator* Up = Config.GearUp.Find(Part.Name))
		{
			Rotation = *Up * (1.0f - Pose.Gear);
		}
		else if (Part.Name.StartsWith(TEXT("PIVOT_Rear")))
		{
			Rotation = Config.NacelleHover * Pose.Nacelles;
			// They hunt a few degrees in a hover.
			Rotation.Pitch += 3.0f * Pose.Nacelles * FMath::Sin(S.HoverSeconds * 1.1f + (Part.Name.EndsWith(TEXT("L")) ? 0.0f : 1.9f)) * (Pose.Lift > 0.05f ? 1.0f : 0.0f);
		}
		else if (Part.Name.StartsWith(TEXT("PIVOT_Lift")))
		{
			const int32 Index = FCString::Atoi(*Part.Name.RightChop(10));
			Rotation.Pitch = Pose.VaneDeg + (Pose.Lift > 0.05f ? 1.5f * FMath::Sin(S.HoverSeconds * 2.3f + static_cast<float>(Index) * 0.8f) : 0.0f);
		}
		else if (Part.Name == TEXT("PIVOT_Door"))
		{
			// Out a little, then back along the hull.
			const float Out = FMath::Clamp(Pose.Door / 0.25f, 0.0f, 1.0f);
			const float Slide = FMath::Clamp((Pose.Door - 0.25f) / 0.75f, 0.0f, 1.0f);
			Offset = FVector(Config.DoorOpen.X * Slide, Config.DoorOpen.Y * Out, Config.DoorOpen.Z * Slide);
		}
		else if (Part.Name == TEXT("PIVOT_Step"))
		{
			Rotation = Config.StepStowed * (1.0f - Pose.Door);
		}
		Part.Pivot->SetRelativeLocationAndRotation(Part.PivotCm + Offset, Rotation);
		if (Part.Name == TEXT("PIVOT_Roof") && Part.Mesh)
		{
			Part.Mesh->SetVisibility(!bHideRoof);
		}
	}

	if (S.Light)
	{
		const float Flicker = 0.9f + 0.1f * FMath::Sin(RealTime * 31.0f + S.FlickerSeed) * FMath::Sin(RealTime * 17.0f + S.FlickerSeed * 2.0f);
		S.Light->SetIntensity(Config.LightCandelas * FMath::Max(Pose.Lift, Pose.Rear * 0.5f) * Flicker);
	}
}

void UShuttleSubsystem::EmitDust(const FShuttle& S, const USimSubsystem& Source, float GameDelta, float Strength)
{
	if (Strength <= 0.0f || GameDelta <= 0.0f || !DustMesh || GShuttleDust.GetValueOnGameThread() == 0)
	{
		return;
	}
	const colony::Sim* Sim = Source.GetSim();
	const FVector Centre = S.Actor->GetActorLocation();
	const FVector Origin = Source.GetWorldOrigin();
	DustCarry += Config.DustPerSecond * Strength * GameDelta;
	while (DustCarry >= 1.0f && Dust.Num() < Config.DustMax)
	{
		DustCarry -= 1.0f;
		const auto Next = [this]()
		{
			RandomState = RandomState * 1664525u + 1013904223u;
			return static_cast<float>(RandomState >> 8) / 16777216.0f;
		};
		// From under the hull, outward in every direction, faster when the jets are close.
		const float Angle = Next() * 2.0f * PI;
		const float Radius = 150.0f + Next() * 450.0f;
		const FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0);
		FVector At = Centre + Dir * Radius;
		const float SimX = static_cast<float>((At.X - Origin.X) / 100.0);
		const float SimY = static_cast<float>((At.Y - Origin.Y) / 100.0);
		At.Z = Sim ? Source.SimToWorld(SimX, SimY, 0).Z + 20.0 : Origin.Z;
		FDustPuff& Puff = Dust.AddDefaulted_GetRef();
		Puff.Position = At;
		Puff.Velocity = Dir * (Config.DustSpeed * (0.5f + 0.5f * Strength) * (0.6f + 0.6f * Next())) + FVector(0.0, 0.0, 40.0f + 80.0f * Next());
		Puff.Life = Config.DustLife * (0.7f + 0.6f * Next());
		Puff.SizeCm = Config.DustSize * (0.6f + 0.8f * Next());
		Puff.Spin = Next() * 360.0f;
	}
	if (Dust.Num() >= Config.DustMax)
	{
		DustCarry = 0.0f;
	}
}

void UShuttleSubsystem::DrawEffects(float GameDelta, float RealTime)
{
	// Flames from every jet, along its nozzle, as long as it burns hard.
	TArray<FTransform> Core, Outer;
	TArray<float> CoreGlow, OuterGlow;
	for (const TPair<uint32, FShuttle>& Pair : Shuttles)
	{
		const FShuttle& S = Pair.Value;
		if (!S.Model)
		{
			continue;
		}
		const FTransform ModelTM = S.Model->GetComponentTransform();
		const auto AddJet = [&](const FTransform& PivotTM, const FVector& LocalPos, const FVector& LocalDir, float Thrust, float LengthCm, float RadiusCm, int32 Seed)
		{
			if (Thrust <= 0.02f)
			{
				return;
			}
			const float Flicker = 0.85f + 0.15f * FMath::Sin(RealTime * 37.0f + static_cast<float>(Seed) * 1.7f + S.FlickerSeed) * FMath::Sin(RealTime * 23.0f + static_cast<float>(Seed) * 2.3f);
			const FVector Pos = PivotTM.TransformPosition(LocalPos);
			const FVector Dir = PivotTM.TransformVectorNoScale(LocalDir).GetSafeNormal();
			const FQuat Rot = FRotationMatrix::MakeFromZ(Dir).ToQuat();
			const float Length = LengthCm * (0.35f + 0.65f * Thrust) * Flicker;
			const float Radius = RadiusCm * (0.7f + 0.3f * Thrust);
			Outer.Add(FTransform(Rot, Pos, FVector(Radius / ShuttleConeRadiusCm, Radius / ShuttleConeRadiusCm, Length / ShuttleConeLengthCm)));
			OuterGlow.Add(Thrust * 0.6f);
			Core.Add(FTransform(Rot, Pos, FVector(Radius * 0.55f / ShuttleConeRadiusCm, Radius * 0.55f / ShuttleConeRadiusCm, Length * 0.55f / ShuttleConeLengthCm)));
			CoreGlow.Add(Thrust * Flicker);
		};
		// A jet's point in its pivot's frame: the part's pivot transform is the model's times the pivot's relative one.
		const auto PivotTransform = [&](const TCHAR* Name, FVector& OutPivot) -> FTransform
		{
			for (const FPart& Part : S.Parts)
			{
				if (Part.Name == Name && Part.Pivot)
				{
					OutPivot = Part.PivotCm;
					return Part.Pivot->GetComponentTransform();
				}
			}
			// No such part (the box stand-in): the jet hangs off the model's frame.
			OutPivot = FVector::ZeroVector;
			return ModelTM;
		};
		for (int32 I = 0; I < Config.LiftJets.Num(); ++I)
		{
			FVector Pivot;
			const FTransform TM = PivotTransform(*FString::Printf(TEXT("PIVOT_Lift%d"), I + 1), Pivot);
			AddJet(TM, Config.LiftJets[I] - Pivot, FVector(0.0, 0.0, -1.0), S.Pose.Lift, Config.LiftLengthCm, Config.LiftRadiusCm, I);
		}
		for (int32 I = 0; I < Config.RearJets.Num(); ++I)
		{
			FVector Pivot;
			const FTransform TM = PivotTransform(I == 0 ? TEXT("PIVOT_RearL") : TEXT("PIVOT_RearR"), Pivot);
			AddJet(TM, Config.RearJets[I] - Pivot, FVector(-1.0, 0.0, 0.0), S.Pose.Rear, Config.RearLengthCm, Config.RearRadiusCm, 20 + I);
		}
	}
	const auto Fill = [](UInstancedStaticMeshComponent* Component, const TArray<FTransform>& Transforms, const TArray<float>& Values)
	{
		if (!Component)
		{
			return;
		}
		if (Component->GetInstanceCount() > 0)
		{
			Component->ClearInstances();
		}
		if (Transforms.IsEmpty())
		{
			return;
		}
		Component->AddInstances(Transforms, false, true);
		for (int32 I = 0; I < Values.Num(); ++I)
		{
			Component->SetCustomDataValue(I, 0, Values[I], I == Values.Num() - 1);
		}
	};
	Fill(FlameCore, Core, CoreGlow);
	Fill(FlameOuter, Outer, OuterGlow);
	FlamesDrawn = Core.Num();

	// Dust: drifts out, slows, grows and thins away.
	TArray<FTransform> Puffs;
	TArray<float> PuffAlpha;
	for (int32 I = Dust.Num() - 1; I >= 0; --I)
	{
		FDustPuff& Puff = Dust[I];
		Puff.Age += GameDelta;
		if (Puff.Age >= Puff.Life)
		{
			Dust.RemoveAtSwap(I);
			continue;
		}
		Puff.Position += Puff.Velocity * GameDelta;
		Puff.Velocity *= FMath::Max(0.0f, 1.0f - 1.6f * GameDelta);
		const float T = Puff.Age / Puff.Life;
		const float Size = Puff.SizeCm + Config.DustGrow * T;
		Puffs.Add(FTransform(FRotator(0.0f, Puff.Spin, 0.0f), Puff.Position, FVector(Size / ShuttlePuffRadiusCm, Size / ShuttlePuffRadiusCm, Size * 0.6f / ShuttlePuffRadiusCm)));
		PuffAlpha.Add(Config.DustOpacity * FMath::Min(T * 6.0f, 1.0f) * (1.0f - T));
	}
	Fill(DustMesh, Puffs, PuffAlpha);
}

void UShuttleSubsystem::Tick(float DeltaTime)
{
	USimSubsystem* Source = Bridge.Get();
	const colony::Sim* Sim = Source ? Source->GetSim() : nullptr;
	if (!Source || !Sim || GShuttleEnable.GetValueOnGameThread() == 0)
	{
		if (!Shuttles.IsEmpty())
		{
			ClearAll();
		}
		return;
	}
	if (!bConfigRead)
	{
		ReadConfig();
	}
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->WasInputKeyJustPressed(EKeys::F6))
	{
		ReloadConfig();
	}
	if (Source->GetSimGeneration() != BuiltGeneration)
	{
		ClearAll();
		BuiltGeneration = Source->GetSimGeneration();
	}
	EnsureScene();

	const float GameDelta = Source->GetSpeed() > 0 ? FMath::Min(DeltaTime, 0.1f) * static_cast<float>(Source->GetSpeed()) : 0.0f;
	RealClock += DeltaTime;
	const colony::Snapshot& Current = Source->GetCurrentSnapshot();
	const colony::Snapshot& Previous = Source->GetPreviousSnapshot();
	const float Alpha = Source->GetBlendAlpha();

	for (TPair<uint32, FShuttle>& Pair : Shuttles)
	{
		Pair.Value.bSeen = false;
	}
	float Washes[2][4] = {{0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}};
	int32 WashCount = 0;
	WashStrength = 0.0f;
	for (const colony::ShuttleView& View : Current.shuttles)
	{
		// The progress between the last two snapshots, so the flight is smooth at any speed.
		float Progress = View.progress;
		for (const colony::ShuttleView& Before : Previous.shuttles)
		{
			if (Before.id == View.id && Before.phase == View.phase)
			{
				Progress = FMath::Lerp(Before.progress, View.progress, Alpha);
				break;
			}
		}
		FShuttle& S = Shuttles.FindOrAdd(View.id);
		S.bSeen = true;
		if (!S.Actor)
		{
			uint32 DefValue = ~0u;
			for (const colony::ThingView& Thing : Current.things)
			{
				if (Thing.id == View.building)
				{
					DefValue = Thing.def.value;
					break;
				}
			}
			MakeShuttle(S, *Source, DefValue);
		}
		PoseShuttle(S, View, Progress, *Source, GameDelta, RealClock);

		// Low over the ground the jets blow dust out and push the grass flat.
		const float Height = S.Pose.UpM;
		const float DustStrength = S.Pose.Lift * FMath::Clamp(1.0f - Height / FMath::Max(Config.DustFromM, 1.0f), 0.0f, 1.0f);
		EmitDust(S, *Source, GameDelta, DustStrength);
		const float Wash = S.Pose.Lift * FMath::Clamp(1.0f - Height / FMath::Max(Config.GrassFromM, 1.0f), 0.0f, 1.0f);
		if (Wash > 0.0f && WashCount < 2)
		{
			const FVector At = S.Actor->GetActorLocation();
			Washes[WashCount][0] = static_cast<float>(At.X);
			Washes[WashCount][1] = static_cast<float>(At.Y);
			Washes[WashCount][2] = Config.GrassRadiusM * 100.0f * (0.6f + 0.4f * Wash);
			Washes[WashCount][3] = Wash;
			++WashCount;
			WashStrength = FMath::Max(WashStrength, Wash);
		}

		if (GShuttleDebug.GetValueOnGameThread() != 0 && DebugClock <= 0.0f)
		{
			UE_LOG(LogShuttle, Display, TEXT("Shuttle %u: phase %d, progress %.3f, back %.1f m, up %.1f m, gear %.2f, nacelles %.2f, lift %.2f, rear %.2f, door %.2f"),
				View.id, static_cast<int32>(View.phase), Progress, S.Pose.BackM, S.Pose.UpM, S.Pose.Gear, S.Pose.Nacelles, S.Pose.Lift, S.Pose.Rear, S.Pose.Door);
		}
	}
	DebugClock = DebugClock <= 0.0f ? 1.0f : DebugClock - DeltaTime;
	for (auto It = Shuttles.CreateIterator(); It; ++It)
	{
		if (!It.Value().bSeen)
		{
			DestroyShuttle(It.Value());
			It.RemoveCurrent();
		}
	}
	if (Downwash)
	{
		if (UMaterialParameterCollectionInstance* Instance = GetWorld()->GetParameterCollectionInstance(Downwash))
		{
			Instance->SetVectorParameterValue(TEXT("Wash0"), FLinearColor(Washes[0][0], Washes[0][1], Washes[0][2], Washes[0][3]));
			Instance->SetVectorParameterValue(TEXT("Wash1"), FLinearColor(Washes[1][0], Washes[1][1], Washes[1][2], Washes[1][3]));
		}
	}
	DrawEffects(GameDelta, RealClock);
}

bool UShuttleSubsystem::GetPose(uint32 Id, FShuttlePose& Out) const
{
	if (const FShuttle* S = Shuttles.Find(Id))
	{
		Out = S->Pose;
		return true;
	}
	return false;
}

bool UShuttleSubsystem::GetLocation(uint32 Id, FVector& Out) const
{
	if (const FShuttle* S = Shuttles.Find(Id); S && S->Actor)
	{
		Out = S->Actor->GetActorLocation();
		return true;
	}
	return false;
}
