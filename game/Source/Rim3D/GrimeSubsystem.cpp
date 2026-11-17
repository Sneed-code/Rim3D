#include "GrimeSubsystem.h"

#include "BloodSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"
#include "WeaponDrawSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

namespace
{
	TAutoConsoleVariable<int32> GGrimeEnabled(TEXT("Grime.Enabled"), 1, TEXT("1 draws the sim's dirt and blood on bodies, clothes and weapons."), ECVF_Default);
	TAutoConsoleVariable<float> GGrimeMaxDistance(TEXT("Grime.MaxDistance"), 60.0f, TEXT("Metres from the camera within which bodies show dirt and blood."), ECVF_Default);
	TAutoConsoleVariable<float> GGrimeStrength(TEXT("Grime.Strength"), 1.0f, TEXT("Multiplies how much dirt and blood shows (0 to 2)."), ECVF_Default);

	const TCHAR* const GrimeOverlayPath = TEXT("/Game/Rim3D/Generated/Blood/M_GrimeOverlay.M_GrimeOverlay");
	constexpr float GrimeUpdateSeconds = 0.25f;
	constexpr float GrimeShowLevel = 0.01f;
	// Dirt: dusty brown-grey (linear).
	const FLinearColor GrimeDirtColor(0.11f, 0.085f, 0.06f);

	/** The fluid's colour at an age in real seconds at 1x, and how wet it still is (0 to 1), as UBloodSubsystem dries stains. */
	FLinearColor GrimeBloodColor(const FBloodFluid& Fluid, float AgeSeconds, float& OutWet)
	{
		float DryMinutes = 5.0f;
		if (const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Blood.DryMinutes")))
		{
			DryMinutes = Var->GetFloat();
		}
		const float DrySeconds = FMath::Max(10.0f, DryMinutes * 60.0f);
		OutWet = FMath::Clamp(1.0f - AgeSeconds / DrySeconds, 0.0f, 1.0f);
		const float T = FMath::Clamp(1.0f - FMath::Exp(-2.6f * AgeSeconds / DrySeconds), 0.0f, 1.0f) / (1.0f - FMath::Exp(-2.6f));
		return FMath::Lerp(Fluid.Wet, Fluid.Dry, FMath::Clamp(T, 0.0f, 1.0f));
	}

	/** sneed: blood fades 3% every 10 s from when it was put on (Blood.FadeStep), as the stains do. */
	float GrimeBloodFade(float AgeSeconds)
	{
		float Step = 0.03f;
		if (const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Blood.FadeStep")))
		{
			Step = FMath::Max(0.0f, Var->GetFloat());
		}
		return FMath::Clamp(1.0f - Step * FMath::FloorToFloat(FMath::Max(0.0f, AgeSeconds) / 10.0f), 0.0f, 1.0f);
	}

	void GrimeCollectMeshes(AActor* Actor, TArray<TWeakObjectPtr<UMeshComponent>>& Out, float& OutMinZ, float& OutHeight)
	{
		if (!Actor)
		{
			return;
		}
		TArray<USkeletalMeshComponent*> Skinned;
		Actor->GetComponents<USkeletalMeshComponent>(Skinned);
		for (USkeletalMeshComponent* Mesh : Skinned)
		{
			if (!Mesh || !Mesh->GetSkeletalMeshAsset())
			{
				continue;
			}
			Out.Add(Mesh);
			// The tallest mesh (the body) sets feet and head for all: clothes share its bind pose.
			const FBox Box = Mesh->GetSkeletalMeshAsset()->GetImportedBounds().GetBox();
			const float Height = static_cast<float>(Box.Max.Z - Box.Min.Z);
			if (Height > OutHeight)
			{
				OutHeight = Height;
				OutMinZ = static_cast<float>(Box.Min.Z);
			}
		}
	}
}

bool UGrimeSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UGrimeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGrimeSubsystem, STATGROUP_Tickables);
}

void UGrimeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	BridgeSource = Collection.InitializeDependency<USimSubsystem>();
	Collection.InitializeDependency<USimPawnSubsystem>();
}

bool UGrimeSubsystem::EnsureMaterial()
{
	if (!bMaterialTried)
	{
		bMaterialTried = true;
		Overlay = LoadObject<UMaterialInterface>(nullptr, GrimeOverlayPath);
	}
	return Overlay != nullptr;
}

void UGrimeSubsystem::ClearPawn(FPawnGrime& Pawn)
{
	for (const TWeakObjectPtr<UMeshComponent>& Mesh : Pawn.Meshes)
	{
		if (Mesh.IsValid() && Mesh->GetOverlayMaterial() == Pawn.Material.Get())
		{
			Mesh->SetOverlayMaterial(nullptr);
		}
	}
	if (Pawn.Weapon.IsValid() && Pawn.Weapon->GetOverlayMaterial() == Pawn.WeaponMaterial.Get())
	{
		Pawn.Weapon->SetOverlayMaterial(nullptr);
	}
	Materials.Remove(Pawn.Material.Get());
	Materials.Remove(Pawn.WeaponMaterial.Get());
	Pawn = FPawnGrime();
}

void UGrimeSubsystem::Tick(float DeltaTime)
{
	Accumulated += DeltaTime;
	if (Accumulated < GrimeUpdateSeconds)
	{
		return;
	}
	Accumulated = 0.0f;
	USimSubsystem* Bridge = BridgeSource.Get();
	const USimPawnSubsystem* PawnDraw = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	const bool bOn = GGrimeEnabled.GetValueOnGameThread() != 0 && Bridge && Bridge->GetSim() && PawnDraw && EnsureMaterial();
	if (!bOn)
	{
		for (TPair<uint32, FPawnGrime>& Pair : Pawns)
		{
			ClearPawn(Pair.Value);
		}
		Pawns.Reset();
		GrimyPawns = 0;
		BloodyWeapons = 0;
		return;
	}
	const colony::Sim* Sim = Bridge->GetSim();
	const colony::Snapshot& Snapshot = Bridge->GetCurrentSnapshot();
	const UWeaponDrawSubsystem* Weapons = GetWorld()->GetSubsystem<UWeaponDrawSubsystem>();
	FVector Camera = FVector::ZeroVector;
	bool bCamera = false;
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
	{
		Camera = PC->PlayerCameraManager->GetCameraLocation();
		bCamera = true;
	}
	const float MaxDistance = FMath::Max(1.0f, GGrimeMaxDistance.GetValueOnGameThread()) * 100.0f;
	const float Strength = FMath::Clamp(GGrimeStrength.GetValueOnGameThread(), 0.0f, 2.0f);

	for (TPair<uint32, FPawnGrime>& Pair : Pawns)
	{
		Pair.Value.bSeen = false;
	}
	GrimyPawns = 0;
	BloodyWeapons = 0;
	for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Pair : PawnDraw->GetPawns())
	{
		ASimPawnActor* Actor = Pair.Value.Get();
		const colony::ThingView* View = Actor ? Snapshot.find(Actor->GetThing()) : nullptr;
		if (!View)
		{
			continue;
		}
		const bool bNear = !bCamera || FVector::Dist(Camera, Actor->GetActorLocation()) <= MaxDistance;
		const bool bBody = View->dirtLegs > GrimeShowLevel || View->dirtBody > GrimeShowLevel || View->blood > GrimeShowLevel;
		const bool bWeapon = View->weaponBlood > GrimeShowLevel;
		FPawnGrime* Grime = Pawns.Find(Pair.Key);
		if (!bNear || (!bBody && !bWeapon))
		{
			if (Grime)
			{
				ClearPawn(*Grime);
				Pawns.Remove(Pair.Key);
			}
			continue;
		}
		if (!Grime)
		{
			Grime = &Pawns.Add(Pair.Key);
		}
		Grime->bSeen = true;

		// Meshes: the actor's and its MetaHuman's (clothes come and go, so look again each time).
		Grime->Meshes.Reset();
		Grime->LocalMinZ = 0.0f;
		Grime->LocalHeight = 0.0f;
		GrimeCollectMeshes(Actor, Grime->Meshes, Grime->LocalMinZ, Grime->LocalHeight);
		GrimeCollectMeshes(Actor->GetMetaHuman(), Grime->Meshes, Grime->LocalMinZ, Grime->LocalHeight);
		if (Grime->LocalHeight < 10.0f)
		{
			Grime->LocalHeight = 180.0f;
		}

		const float Age = View->bloodAge / static_cast<float>(colony::kStepsPerRealSecondAt1x);
		if (bBody)
		{
			UMaterialInstanceDynamic* Material = Grime->Material.Get();
			if (!Material)
			{
				Material = UMaterialInstanceDynamic::Create(Overlay, this);
				Materials.Add(Material);
				Grime->Material = Material;
			}
			FBloodFluid Fluid = UBloodSubsystem::FluidForPawnDef(Sim, View->def);
			UBloodSubsystem::FluidForFilthDef(Sim, View->bloodFilth, Fluid);
			float Wet = 0.0f;
			const FLinearColor BloodColor = GrimeBloodColor(Fluid, Age, Wet);
			Material->SetScalarParameterValue(TEXT("DirtLegs"), FMath::Clamp(View->dirtLegs * Strength, 0.0f, 1.0f));
			Material->SetScalarParameterValue(TEXT("DirtBody"), FMath::Clamp(View->dirtBody * Strength, 0.0f, 1.0f));
			Material->SetScalarParameterValue(TEXT("Blood"), FMath::Clamp(View->blood * GrimeBloodFade(Age) * Strength, 0.0f, 1.0f));
			Material->SetScalarParameterValue(TEXT("BloodWet"), Wet);
			Material->SetScalarParameterValue(TEXT("LocalMinZ"), Grime->LocalMinZ);
			Material->SetScalarParameterValue(TEXT("LocalHeight"), Grime->LocalHeight);
			Material->SetVectorParameterValue(TEXT("BloodColor"), BloodColor);
			Material->SetVectorParameterValue(TEXT("DirtColor"), GrimeDirtColor);
			for (const TWeakObjectPtr<UMeshComponent>& Mesh : Grime->Meshes)
			{
				if (Mesh.IsValid() && Mesh->GetOverlayMaterial() != Material)
				{
					Mesh->SetOverlayMaterial(Material);
				}
			}
			++GrimyPawns;
		}
		else if (Grime->Material.IsValid())
		{
			for (const TWeakObjectPtr<UMeshComponent>& Mesh : Grime->Meshes)
			{
				if (Mesh.IsValid() && Mesh->GetOverlayMaterial() == Grime->Material.Get())
				{
					Mesh->SetOverlayMaterial(nullptr);
				}
			}
		}

		// The weapon in its hands: blood only, no height (the whole weapon is the "body").
		UMeshComponent* Weapon = Weapons ? Weapons->GetHandsComponent(Actor->GetThing()) : nullptr;
		if (Grime->Weapon.IsValid() && Grime->Weapon.Get() != Weapon && Grime->Weapon->GetOverlayMaterial() == Grime->WeaponMaterial.Get())
		{
			Grime->Weapon->SetOverlayMaterial(nullptr);  // put away or swapped: the holstered one stays as it is, unseen
		}
		Grime->Weapon = Weapon;
		if (Weapon && bWeapon)
		{
			UMaterialInstanceDynamic* Material = Grime->WeaponMaterial.Get();
			if (!Material)
			{
				Material = UMaterialInstanceDynamic::Create(Overlay, this);
				Materials.Add(Material);
				Grime->WeaponMaterial = Material;
			}
			FBloodFluid Fluid;
			UBloodSubsystem::FluidForFilthDef(Sim, View->weaponBloodFilth, Fluid);
			float Wet = 0.0f;
			const FLinearColor BloodColor = GrimeBloodColor(Fluid, View->weaponBloodAge / static_cast<float>(colony::kStepsPerRealSecondAt1x), Wet);
			Material->SetScalarParameterValue(TEXT("DirtLegs"), 0.0f);
			Material->SetScalarParameterValue(TEXT("DirtBody"), 0.0f);
			// A weapon shows its blood plainly: it is small and the eye goes to it.
			Material->SetScalarParameterValue(TEXT("Blood"), FMath::Clamp(View->weaponBlood * GrimeBloodFade(View->weaponBloodAge / static_cast<float>(colony::kStepsPerRealSecondAt1x)) * Strength * 1.3f, 0.0f, 1.0f));
			Material->SetScalarParameterValue(TEXT("BloodWet"), Wet);
			Material->SetScalarParameterValue(TEXT("NoiseScale"), 0.15f);
			Material->SetVectorParameterValue(TEXT("BloodColor"), BloodColor);
			if (Weapon->GetOverlayMaterial() != Material)
			{
				Weapon->SetOverlayMaterial(Material);
			}
			++BloodyWeapons;
		}
		else if (Weapon && Grime->WeaponMaterial.IsValid() && Weapon->GetOverlayMaterial() == Grime->WeaponMaterial.Get())
		{
			Weapon->SetOverlayMaterial(nullptr);  // washed clean
		}
	}
	// Pawns no longer drawn.
	for (auto It = Pawns.CreateIterator(); It; ++It)
	{
		if (!It.Value().bSeen)
		{
			ClearPawn(It.Value());
			It.RemoveCurrent();
		}
	}
}
