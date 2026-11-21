#include "SkinBlood.h"

#include "MakeHumanArt.h"
#include "SkinBloodArt.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

namespace
{
	TAutoConsoleVariable<int32> GSkinBloodOn(TEXT("Blood.SkinPaint"), 1,
		TEXT("1 paints wounds and blood into MakeHuman bodies' skin; 0 uses the old decals on bodies."), ECVF_Default);
	TAutoConsoleVariable<int32> GSkinBloodMasks(TEXT("Blood.SkinMasks"), 16,
		TEXT("How many bodies can carry blood in their skin at once (each takes 16 MB of video memory); the one farthest from the camera gives way (8 made a fight's blood vanish and come back: sneed's red flashing, 2026-11-20)."),
		ECVF_Default);

	float SkinBloodConsole(const TCHAR* Name, float Default)
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
		return Var ? Var->GetFloat() : Default;
	}

	/** The bone a point on the body moves with: the one whose span (to its children) is nearest. Only bones with a
	 *  physics body when the mesh has them (not face, breast or finger bones that move on their own). */
	int32 SkinBloodBone(const USkeletalMeshComponent* Mesh, const FVector& At)
	{
		const FReferenceSkeleton& Ref = Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();
		const int32 Num = Ref.GetNum();
		TArray<int32> Candidates;
		if (const UPhysicsAsset* Physics = Mesh->GetPhysicsAsset())
		{
			for (const TObjectPtr<USkeletalBodySetup>& Body : Physics->SkeletalBodySetups)
			{
				const int32 Index = Body ? Ref.FindBoneIndex(Body->BoneName) : INDEX_NONE;
				if (Index != INDEX_NONE)
				{
					Candidates.Add(Index);
				}
			}
		}
		if (Candidates.IsEmpty())
		{
			for (int32 Index = 0; Index < Num; ++Index)
			{
				Candidates.Add(Index);
			}
		}
		TArray<FVector> World;
		World.SetNumUninitialized(Num);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			World[Index] = Mesh->GetBoneTransform(Index).GetLocation();
		}
		int32 Best = INDEX_NONE;
		double BestDistance = TNumericLimits<double>::Max();
		for (const int32 Bone : Candidates)
		{
			double Distance = FVector::Dist(At, World[Bone]);
			for (int32 Child = Bone + 1; Child < Num; ++Child)
			{
				if (Ref.GetParentIndex(Child) == Bone)
				{
					Distance = FMath::Min(Distance, static_cast<double>(FMath::PointDistToSegment(At, World[Bone], World[Child])));
				}
			}
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Bone;
			}
		}
		return Best;
	}

	bool SkinBloodIsHairSlot(const FString& Slot)
	{
		if (Slot.Contains(TEXT("eyebrow")) || Slot.Contains(TEXT("lash")) || Slot.Contains(TEXT("Pubic")))
		{
			return false;
		}
		for (const TCHAR* Key : {TEXT("long01"), TEXT("bob02"), TEXT("ponytail01"), TEXT("braid01"), TEXT("short02"), TEXT("short04"), TEXT("short_messy"), TEXT("afro")})
		{
			if (Slot.Contains(Key))
			{
				return true;
			}
		}
		return false;
	}

	FString SkinBloodBaseOf(const USkeletalMeshComponent* Mesh)
	{
		const USkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
		const FString Name = Asset ? Asset->GetName() : FString();
		return Name.StartsWith(TEXT("SK_MH_")) ? Name.RightChop(6) : FString();
	}

	/** Blood on skin needs some light in every channel (pure red reads black in shade). */
	FLinearColor SkinBloodFloor(const FLinearColor& Color, float Scale, float Floor)
	{
		return FLinearColor(FMath::Max(Color.R * Scale, Floor), FMath::Max(Color.G * Scale, Floor * 0.6f), FMath::Max(Color.B * Scale, Floor * 0.5f), 1.0f);
	}
}

bool USkinBlood::EnsureAssets()
{
	if (!bTried)
	{
		bTried = true;
		PaintBase = LoadObject<UMaterialInterface>(nullptr, *MakeHumanAssetPath(SkinBloodFolder, SkinBloodPaintName), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (PaintBase)
		{
			PaintMaterial = UMaterialInstanceDynamic::Create(PaintBase, this);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Skin blood: no %s (run -run=BloodArt): bodies keep the blood decals"), SkinBloodPaintName);
		}
	}
	return PaintMaterial != nullptr;
}

UTexture2D* USkinBlood::PositionsFor(const USkeletalMeshComponent* Mesh)
{
	const USkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	if (!Asset)
	{
		return nullptr;
	}
	const FName Key = Asset->GetFName();
	if (const TObjectPtr<UTexture2D>* Found = PositionMaps.Find(Key))
	{
		return Found->Get();
	}
	if (NoPositions.Contains(Key))
	{
		return nullptr;
	}
	UTexture2D* Positions = nullptr;
	const FString Base = SkinBloodBaseOf(Mesh);
	if (!Base.IsEmpty() && Asset->GetRefBasesInvMatrix().Num() > 0)
	{
		// The skin material must read the mask (BloodArt adds it); without it the blood would vanish.
		bool bSkinReads = false;
		for (const FSkeletalMaterial& Slot : Asset->GetMaterials())
		{
			float Default = 0.0f;
			if (Slot.MaterialInterface && Slot.MaterialInterface->GetName().StartsWith(MakeHumanSkinName(Base)) &&
				Slot.MaterialInterface->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(TEXT("BloodOn")), Default))
			{
				bSkinReads = true;
			}
		}
		if (bSkinReads)
		{
			Positions = LoadObject<UTexture2D>(nullptr, *MakeHumanAssetPath(SkinBloodFolder, SkinBloodPositionName(Base)), nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		UE_LOG(LogTemp, Display, TEXT("Skin blood for %s: %s"), *Asset->GetName(),
			Positions ? TEXT("painted into the skin") : (bSkinReads ? TEXT("no position map (run -run=BloodArt): decals") : TEXT("skin material has no blood mask (run -run=BloodArt): decals")));
	}
	if (Positions)
	{
		PositionMaps.Add(Key, Positions);
	}
	else
	{
		NoPositions.Add(Key);
	}
	return Positions;
}

bool USkinBlood::CanPaint(const USkeletalMeshComponent* Mesh)
{
	return GSkinBloodOn.GetValueOnGameThread() != 0 && Mesh && EnsureAssets() && PositionsFor(Mesh) != nullptr;
}

UTextureRenderTarget2D* USkinBlood::TakeTarget()
{
	UTextureRenderTarget2D* Target = FreeTargets.Num() > 0 ? FreeTargets.Pop() : nullptr;
	if (!Target)
	{
		Target = NewObject<UTextureRenderTarget2D>(this);
		Target->RenderTargetFormat = RTF_RGBA16f;
		Target->ClearColor = FLinearColor::Black;
		Target->bAutoGenerateMips = false;
		Target->AddressX = TA_Clamp;
		Target->AddressY = TA_Clamp;
		Target->InitAutoFormat(SkinBloodResolution, SkinBloodResolution);
		AllTargets.Add(Target);
	}
	Target->UpdateResourceImmediate(true);  // cleared
	return Target;
}

USkinBlood::FMask* USkinBlood::FindOrAddMask(USkeletalMeshComponent* Mesh, float Clock)
{
	for (FMask& Mask : Masks)
	{
		if (Mask.Mesh.Get() == Mesh)
		{
			return &Mask;
		}
	}
	const int32 Cap = FMath::Clamp(GSkinBloodMasks.GetValueOnGameThread(), 1, 64);
	while (Masks.Num() >= Cap)
	{
		// The one farthest from the camera gives way (a body off in the distance, not one in the fight in front of you),
		// counting a minute since its last wound as 10 m further.
		FVector Camera = Mesh->GetComponentLocation();
		if (const UWorld* World = Mesh->GetWorld())
		{
			if (const APlayerController* Player = World->GetFirstPlayerController(); Player && Player->PlayerCameraManager)
			{
				Camera = Player->PlayerCameraManager->GetCameraLocation();
			}
		}
		const auto Score = [&](const FMask& M)
		{
			const USkeletalMeshComponent* Other = M.Mesh.Get();
			const double Away = Other ? FVector::Dist(Other->GetComponentLocation(), Camera) : 1e9;
			return Away + (Clock - M.LastPaint) / 60.0 * 1000.0;
		};
		int32 Farthest = 0;
		for (int32 Index = 1; Index < Masks.Num(); ++Index)
		{
			if (Score(Masks[Index]) > Score(Masks[Farthest]))
			{
				Farthest = Index;
			}
		}
		DropMask(Farthest);
	}
	FMask& Mask = Masks.AddDefaulted_GetRef();
	Mask.Mesh = Mesh;
	Mask.Positions = PositionsFor(Mesh);
	Mask.Targets[0] = TakeTarget();
	Mask.Targets[1] = TakeTarget();
	Mask.LastPaint = Clock;
	Mask.NextFade = Clock + 10.0f;
	return &Mask;
}

void USkinBlood::DropMask(int32 Index)
{
	FMask& Mask = Masks[Index];
	ApplyToSkin(Mask, false);
	ApplyToHair(Mask, true);
	for (UTextureRenderTarget2D* Target : Mask.Targets)
	{
		if (Target)
		{
			FreeTargets.Add(Target);
		}
	}
	Masks.RemoveAtSwap(Index);
}

void USkinBlood::Reset()
{
	while (Masks.Num() > 0)
	{
		DropMask(Masks.Num() - 1);
	}
}

void USkinBlood::Draw(FMask& Mask)
{
	USkeletalMeshComponent* Mesh = Mask.Mesh.Get();
	if (!Mesh || !PaintMaterial)
	{
		return;
	}
	PaintMaterial->SetTextureParameterValue(TEXT("Prev"), Mask.Targets[Mask.Current]);
	PaintMaterial->SetTextureParameterValue(TEXT("Pos"), Mask.Positions);
	UKismetRenderingLibrary::DrawMaterialToRenderTarget(Mesh, Mask.Targets[1 - Mask.Current], PaintMaterial);
	Mask.Current = 1 - Mask.Current;
}

void USkinBlood::ApplyToSkin(FMask& Mask, bool bOn)
{
	USkeletalMeshComponent* Mesh = Mask.Mesh.Get();
	if (!Mesh)
	{
		return;
	}
	const FString SkinName = MakeHumanSkinName(SkinBloodBaseOf(Mesh));
	const FLinearColor Wet = SkinBloodFloor(Mask.Fluid.Wet, 1.1f, 0.03f);
	const FLinearColor Dry = SkinBloodFloor(Mask.Fluid.Dry, 1.0f, 0.016f);
	for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
	{
		UMaterialInterface* Now = Mesh->GetMaterial(Slot);
		UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Now);
		const UMaterialInterface* Parent = Dynamic ? Dynamic->Parent.Get() : Now;
		if (!Parent || !Parent->GetName().StartsWith(SkinName))
		{
			continue;
		}
		if (!Dynamic)
		{
			if (!bOn)
			{
				continue;
			}
			Dynamic = Mesh->CreateDynamicMaterialInstance(Slot);
			if (!Dynamic)
			{
				continue;
			}
		}
		Dynamic->SetScalarParameterValue(TEXT("BloodOn"), bOn ? 1.0f : 0.0f);
		if (bOn)
		{
			Dynamic->SetTextureParameterValue(TEXT("BloodMask"), Mask.Targets[Mask.Current]);
			Dynamic->SetScalarParameterValue(TEXT("BloodMaskRes"), static_cast<float>(SkinBloodResolution));
			Dynamic->SetVectorParameterValue(TEXT("BloodWetColor"), Wet);
			Dynamic->SetVectorParameterValue(TEXT("BloodDryColor"), Dry);
		}
	}
}

void USkinBlood::ApplyToHair(FMask& Mask, bool bRestore)
{
	USkeletalMeshComponent* Mesh = Mask.Mesh.Get();
	if (!Mesh || (Mask.Hair.IsEmpty() && (bRestore || Mask.HeadBlood <= 0.001f)))
	{
		return;
	}
	if (Mask.Hair.IsEmpty())
	{
		const TArray<FName> Slots = Mesh->GetMaterialSlotNames();
		for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
		{
			UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(Slot));
			FLinearColor Tint = FLinearColor::White;
			if (Dynamic && SkinBloodIsHairSlot(Slots[Slot].ToString()) && Dynamic->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("Tint")), Tint))
			{
				FHairTint& Hair = Mask.Hair.AddDefaulted_GetRef();
				Hair.Material = Dynamic;
				Hair.Original = Tint;
				Hair.Written = Tint;
			}
		}
	}
	// Blood soaks the hair near a head wound: darker and redder, a little more as more runs in.
	const float Soak = FMath::Clamp(Mask.HeadBlood, 0.0f, 1.0f) * 0.85f;
	const FLinearColor Darken = FMath::Lerp(FLinearColor::White, FLinearColor(0.42f, 0.2f, 0.18f), Soak);
	for (FHairTint& Hair : Mask.Hair)
	{
		UMaterialInstanceDynamic* Dynamic = Hair.Material.Get();
		FLinearColor Now = FLinearColor::White;
		if (!Dynamic || !Dynamic->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("Tint")), Now))
		{
			continue;
		}
		if (!Now.Equals(Hair.Written, 1e-4f))
		{
			Hair.Original = Now;  // the creator set a new colour
		}
		Hair.Written = bRestore ? Hair.Original : Hair.Original * Darken;
		Hair.Written.A = Hair.Original.A;
		Dynamic->SetVectorParameterValue(TEXT("Tint"), Hair.Written);
	}
	if (bRestore)
	{
		Mask.Hair.Reset();
	}
}

bool USkinBlood::Paint(USkeletalMeshComponent* Mesh, const FSkinBloodPaint& Mark, const FBloodFluid& Fluid, float Clock)
{
	if (!CanPaint(Mesh))
	{
		return false;
	}
	const USkeletalMesh* Asset = Mesh->GetSkeletalMeshAsset();
	const TArray<FMatrix44f>& InverseBind = Asset->GetRefBasesInvMatrix();
	const int32 Bone = SkinBloodBone(Mesh, Mark.At);
	if (Bone == INDEX_NONE || !InverseBind.IsValidIndex(Bone))
	{
		return false;
	}
	// From where the body is now into its bind pose, through the bone the mark moves with.
	const FTransform Now = Mesh->GetBoneTransform(Bone);
	const FMatrix BoneToBind = FMatrix(InverseBind[Bone]).Inverse();
	const auto Point = [&](const FVector& World) { return BoneToBind.TransformPosition(Now.InverseTransformPosition(World)); };
	const auto Direction = [&](const FVector& World) { return BoneToBind.TransformVector(Now.InverseTransformVectorNoScale(World)).GetSafeNormal(); };
	const FVector At = Point(Mark.At);
	const FVector To = Point(Mark.To);
	FVector Inward = Direction(Mark.Inward);
	if (Inward.IsNearlyZero())
	{
		Inward = FVector::ForwardVector;
	}
	FVector Down = Direction(FVector::DownVector);
	if (Down.IsNearlyZero())
	{
		Down = FVector::DownVector;
	}

	FMask* Mask = FindOrAddMask(Mesh, Clock);
	if (!Mask || !Mask->Positions)
	{
		return false;
	}
	Mask->Fluid = Fluid;
	Mask->LastPaint = Clock;
	PaintMaterial->SetScalarParameterValue(TEXT("Mode"), 0.0f);
	PaintMaterial->SetVectorParameterValue(TEXT("Hit"), FLinearColor(At.X, At.Y, At.Z, FMath::Clamp(Mark.Hole, 0.0f, 1.0f)));
	PaintMaterial->SetVectorParameterValue(TEXT("Hit2"), FLinearColor(To.X, To.Y, To.Z, Mark.Seed >= 0.0f ? Mark.Seed : FMath::FRandRange(0.0f, 97.0f)));
	PaintMaterial->SetVectorParameterValue(TEXT("Dir"), FLinearColor(Inward.X, Inward.Y, Inward.Z, FMath::Max(1.0f, Mark.Depth)));
	PaintMaterial->SetVectorParameterValue(TEXT("Down"), FLinearColor(Down.X, Down.Y, Down.Z, FMath::Clamp(Mark.Strength, 0.0f, 1.0f)));
	PaintMaterial->SetVectorParameterValue(TEXT("Shape"), FLinearColor(Mark.Core, Mark.Soak, Mark.Spatter, Mark.Run));
	Draw(*Mask);
	ApplyToSkin(*Mask, true);
	LastClock = Clock;

	// A wound (not a drop or a run) soaks the clothes over it, wider than on the skin: cloth wicks.
	if (Mark.Soak > 0.0f || Mark.Core >= 1.0f)
	{
		const FVector3f SpotAt(0.5f * (At + To));
		const float SpotRadius = (Mark.Soak * 1.5f + Mark.Core * 2.0f + 0.5f * static_cast<float>(FVector::Dist(At, To))) * FMath::Clamp(Mark.Strength, 0.3f, 1.0f);
		const float SpotStrength = FMath::Clamp(Mark.Strength, 0.0f, 1.0f);
		// A hit on a patch already soaked makes that patch wetter and a little wider instead of taking a slot of its own:
		// with only 8 slots, a burst of hits on one spot pushed every other patch off the clothes at once (sneed
		// 2026-11-20: blood "glitches out and flashes" when hit over and over). When all 8 are taken by patches far apart,
		// the new one joins the nearest, which grows to cover both, so no patch ever vanishes.
		int32 Nearest = INDEX_NONE;
		float NearestGap = TNumericLimits<float>::Max();
		for (int32 Index = 0; Index < Mask->ClothSpots.Num(); ++Index)
		{
			const FClothSpot& Old = Mask->ClothSpots[Index];
			const float Gap = FVector3f::Dist(Old.At, SpotAt) - Old.Radius;
			if (Gap < NearestGap)
			{
				NearestGap = Gap;
				Nearest = Index;
			}
		}
		if (Nearest != INDEX_NONE && (NearestGap <= 0.0f || Mask->ClothSpots.Num() >= 8))
		{
			FClothSpot Merged = Mask->ClothSpots[Nearest];
			const float Apart = FVector3f::Dist(Merged.At, SpotAt);
			if (Apart + SpotRadius > Merged.Radius)
			{
				// The smallest sphere round both, a little wider for the soak spreading; at most 40 cm.
				const float Radius = FMath::Min(40.0f, 0.5f * (Apart + Merged.Radius + SpotRadius));
				const float Toward = Apart > 1e-3f ? (Radius - Merged.Radius) / Apart : 0.0f;
				Merged.At = Merged.At + (SpotAt - Merged.At) * FMath::Clamp(Toward, 0.0f, 1.0f);
				Merged.Radius = Radius;
			}
			Merged.Strength = FMath::Min(1.0f, FMath::Max(Merged.Strength, SpotStrength) + 0.1f * SpotStrength);
			Merged.Clock = Clock;
			Mask->ClothSpots.RemoveAt(Nearest);
			Mask->ClothSpots.Add(Merged);  // newest last
		}
		else
		{
			FClothSpot& Spot = Mask->ClothSpots.AddDefaulted_GetRef();
			Spot.At = SpotAt;
			Spot.Radius = SpotRadius;
			Spot.Strength = SpotStrength;
			Spot.Clock = Clock;
		}
	}

	// A wound on the head or neck soaks the hair.
	const FString BoneName = Asset->GetRefSkeleton().GetBoneName(Bone).ToString().ToLower();
	if (BoneName.Contains(TEXT("head")) || BoneName.Contains(TEXT("neck")))
	{
		Mask->HeadBlood = FMath::Min(1.0f, Mask->HeadBlood + 0.5f * Mark.Strength * FMath::Clamp((Mark.Core + Mark.Soak) / 6.0f, 0.1f, 1.0f));
		ApplyToHair(*Mask, false);
	}
	++Paints;
	return true;
}

void USkinBlood::Tick(float Clock, TFunctionRef<float(const USkeletalMeshComponent*)> SimBlood)
{
	LastClock = Clock;
	if (Masks.IsEmpty())
	{
		return;
	}
	const float Step = FMath::Max(0.0f, SkinBloodConsole(TEXT("Blood.FadeStep"), 0.03f));
	const float DrySeconds = FMath::Max(10.0f, SkinBloodConsole(TEXT("Blood.DryMinutes"), 5.0f) * 60.0f);
	const float Lifetime = Step > 0.0f ? 10.0f * (1.0f / Step + 1.0f) : TNumericLimits<float>::Max();
	const bool bOn = GSkinBloodOn.GetValueOnGameThread() != 0;
	for (int32 Index = Masks.Num() - 1; Index >= 0; --Index)
	{
		FMask& Mask = Masks[Index];
		const USkeletalMeshComponent* Mesh = Mask.Mesh.Get();
		if (!Mesh || !bOn || Clock - Mask.LastPaint > Lifetime)
		{
			DropMask(Index);
			continue;
		}
		// Washed (swimming): the sim's blood on the body fell to nothing.
		const float Sim = SimBlood(Mesh);
		if (Sim >= 0.0f && Sim < 0.01f && Mask.LastSimBlood > 0.05f)
		{
			DropMask(Index);
			continue;
		}
		if (Sim >= 0.0f)
		{
			Mask.LastSimBlood = Sim;
		}
		// Every 10 s: 3% less of everything, a little drier.
		bool bFaded = false;
		for (int32 Steps = 0; Steps < 3 && Clock >= Mask.NextFade; ++Steps)
		{
			Mask.NextFade += 10.0f;
			PaintMaterial->SetScalarParameterValue(TEXT("Mode"), 1.0f);
			PaintMaterial->SetVectorParameterValue(TEXT("Decay"), FLinearColor(Step, 10.0f / DrySeconds, Step, 0.0f));
			Draw(Mask);
			Mask.HeadBlood = FMath::Max(0.0f, Mask.HeadBlood - Step);
			bFaded = true;
		}
		if (Clock >= Mask.NextFade)
		{
			Mask.NextFade = Clock + 10.0f;  // a long hitch: no catching up
		}
		if (bFaded)
		{
			ApplyToSkin(Mask, true);
			ApplyToHair(Mask, false);
		}
	}
}

int32 USkinBlood::GetClothSpots(const USkeletalMeshComponent* Body, FLinearColor OutSpots[8], float OutAmounts[8], float& OutWet) const
{
	OutWet = 0.0f;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		OutSpots[Index] = FLinearColor::Transparent;
		OutAmounts[Index] = 0.0f;
	}
	const FMask* Mask = Masks.FindByPredicate([Body](const FMask& M) { return M.Mesh.Get() == Body; });
	if (!Body || !Mask)
	{
		return 0;
	}
	const float Step = FMath::Max(0.0f, SkinBloodConsole(TEXT("Blood.FadeStep"), 0.03f));
	const float DrySeconds = FMath::Max(10.0f, SkinBloodConsole(TEXT("Blood.DryMinutes"), 5.0f) * 60.0f);
	int32 Count = 0;
	for (int32 Index = Mask->ClothSpots.Num() - 1; Index >= 0 && Count < 8; --Index)
	{
		const FClothSpot& Spot = Mask->ClothSpots[Index];
		const float Age = FMath::Max(0.0f, LastClock - Spot.Clock);
		const float Left = Spot.Strength * FMath::Clamp(1.0f - Step * FMath::FloorToFloat(Age / 10.0f), 0.0f, 1.0f);
		if (Left <= 0.01f)
		{
			continue;
		}
		if (Count == 0)
		{
			OutWet = FMath::Clamp(1.0f - Age / DrySeconds, 0.0f, 1.0f);
		}
		OutSpots[Count] = FLinearColor(Spot.At.X, Spot.At.Y, Spot.At.Z, Spot.Radius);
		OutAmounts[Count] = Left;
		++Count;
	}
	return Count;
}
