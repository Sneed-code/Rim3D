#include "GunRangeSubsystem.h"

#include "Animation/AnimSequence.h"
#include "Animation/MorphTarget.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/CameraComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <imgui.h>

#include "GunViewAnim.h"
#include "MakeHumanAnimInstance.h"
#include "FreeHandSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

DEFINE_LOG_CATEGORY_STATIC(LogGunRange, Log, All);

namespace
{
	const TCHAR* const GunRangeArtRoot = TEXT("/Game/Rim3D/Generated/Guns");
	constexpr float GunRangeSwitchSeconds = 0.3f;
	constexpr float GunRangeFlashSeconds = 0.06f;

	FVector GunRangeVec(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const FVector& Default)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(Key, Values) || Values->Num() < 3)
		{
			return Default;
		}
		return FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
	}

	FRotator GunRangeRot(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		const FVector V = GunRangeVec(Object, Key, FVector::ZeroVector);
		return FRotator(V.X, V.Y, V.Z);  // pitch, yaw, roll
	}

	float GunRangeNum(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, float Default)
	{
		double Value = Default;
		Object->TryGetNumberField(Key, Value);
		return static_cast<float>(Value);
	}

	FString GunRangeStr(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		FString Value;
		Object->TryGetStringField(Key, Value);
		return Value;
	}

	float GunRangeSmooth(float T)
	{
		T = FMath::Clamp(T, 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	/** The bolt's travel for a cycle phase U (0..1): lift, back, hold, forward, down. Returns (lift degrees, slide cm). */
	FVector2D GunRangeBolt(float U, float Lift, float Back)
	{
		if (U <= 0.0f || U >= 1.0f)
		{
			return FVector2D::ZeroVector;
		}
		const float Up = U < 0.2f ? GunRangeSmooth(U / 0.2f) : (U < 0.8f ? 1.0f : 1.0f - GunRangeSmooth((U - 0.8f) / 0.2f));
		const float Slide = U < 0.2f ? 0.0f : (U < 0.42f ? GunRangeSmooth((U - 0.2f) / 0.22f) : (U < 0.58f ? 1.0f : (U < 0.8f ? 1.0f - GunRangeSmooth((U - 0.58f) / 0.22f) : 0.0f)));
		return FVector2D(Up * Lift, Slide * Back);
	}
}

TStatId UGunRangeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGunRangeSubsystem, STATGROUP_Tickables);
}

void UGunRangeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		BridgeSource = Bridge;
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &UGunRangeSubsystem::OnSimEvent);
	}
	Collection.InitializeDependency<USimPawnSubsystem>();
	Collection.InitializeDependency<UProtagonistSubsystem>();
}

void UGunRangeSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = BridgeSource.Get())
	{
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	Super::Deinitialize();
}

void UGunRangeSubsystem::SetEnabled(bool bOn)
{
	bEnabled = bOn;
	if (bOn && Guns.IsEmpty())
	{
		LoadConfig();
	}
	if (!bOn)
	{
		HideAll();
	}
}

bool UGunRangeSubsystem::LoadConfig()
{
	FString Text;
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../assets/guns.json")));
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *File) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogGunRange, Error, TEXT("Cannot read %s"), *File);
		return false;
	}
	Root->TryGetStringField(TEXT("animRoot"), AnimRoot);
	HeadInView = GunRangeVec(Root, TEXT("headInView"), FVector(-8.0f, 0.0f, -7.0f));
	TArray<FGunRangeGun> Read;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("guns")))
	{
		const TSharedPtr<FJsonObject> J = Value->AsObject();
		if (!J.IsValid())
		{
			continue;
		}
		FGunRangeGun G;
		G.Def = GunRangeStr(J, TEXT("def"));
		G.Label = GunRangeStr(J, TEXT("label"));
		G.Credit = GunRangeStr(J, TEXT("credit"));
		const TSharedPtr<FJsonObject>* Art = nullptr;
		if (J->TryGetObjectField(TEXT("art"), Art))
		{
			G.ArtName = GunRangeStr(*Art, TEXT("name"));
		}
		const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
		if (J->TryGetArrayField(TEXT("parts"), Parts))
		{
			for (const TSharedPtr<FJsonValue>& P : *Parts)
			{
				const TSharedPtr<FJsonObject> PO = P->AsObject();
				FGunRangeGun::FPart Part;
				Part.Name = GunRangeStr(PO, TEXT("name"));
				Part.Match = GunRangeStr(PO, TEXT("match"));
				Part.Pivot = GunRangeVec(PO, TEXT("pivot"), FVector::ZeroVector);
				G.Parts.Add(Part);
			}
		}
		G.MeshRotation = GunRangeRot(J, TEXT("meshRotation"));
		G.MeshOffset = GunRangeVec(J, TEXT("meshOffset"), FVector::ZeroVector);
		G.Grip = GunRangeVec(J, TEXT("grip"), FVector::ZeroVector);
		G.GripTurn = GunRangeRot(J, TEXT("gripTurn"));
		G.bOffHand = J->HasField(TEXT("offHand"));
		G.OffHand = GunRangeVec(J, TEXT("offHand"), FVector::ZeroVector);
		G.Sight = GunRangeVec(J, TEXT("sight"), G.Sight);
		G.Muzzle = GunRangeVec(J, TEXT("muzzle"), G.Muzzle);
		G.Rake = GunRangeNum(J, TEXT("rake"), G.Rake);
		G.Stance = GunRangeStr(J, TEXT("stance"));
		G.StanceHold = GunRangeNum(J, TEXT("stanceHold"), -1.0f);
		G.Reload = GunRangeStr(J, TEXT("reload"));
		G.ReloadFrom = GunRangeNum(J, TEXT("reloadFrom"), 0.0f);
		G.ReloadTo = GunRangeNum(J, TEXT("reloadTo"), 0.0f);
		const TSharedPtr<FJsonObject>* Mag = nullptr;
		if (J->TryGetObjectField(TEXT("magazine"), Mag))
		{
			G.bMagazine = true;
			G.MagazinePart = GunRangeStr(*Mag, TEXT("part"));
			G.MagazineOut = GunRangeNum(*Mag, TEXT("out"), G.MagazineOut);
			G.MagazineIn = GunRangeNum(*Mag, TEXT("in"), G.MagazineIn);
		}
		G.Hip = GunRangeVec(J, TEXT("hip"), G.Hip);
		G.SightDistance = GunRangeNum(J, TEXT("sightDistance"), G.SightDistance);
		G.bReach = !J->HasField(TEXT("reach")) || J->GetBoolField(TEXT("reach"));
		G.ReloadSpot = GunRangeVec(J, TEXT("reloadSpot"), G.ReloadSpot);
		const TSharedPtr<FJsonObject>* KickObj = nullptr;
		if (J->TryGetObjectField(TEXT("kick"), KickObj))
		{
			G.KickBack = GunRangeNum(*KickObj, TEXT("back"), G.KickBack);
			G.KickUp = GunRangeNum(*KickObj, TEXT("up"), G.KickUp);
			G.KickRecover = GunRangeNum(*KickObj, TEXT("recover"), G.KickRecover);
		}
		const TSharedPtr<FJsonObject>* Cycle = nullptr;
		if (J->TryGetObjectField(TEXT("cycle"), Cycle))
		{
			G.bCycle = true;
			G.CyclePart = GunRangeStr(*Cycle, TEXT("part"));
			G.CycleStart = GunRangeNum(*Cycle, TEXT("start"), G.CycleStart);
			G.CycleSeconds = GunRangeNum(*Cycle, TEXT("seconds"), G.CycleSeconds);
			G.CycleLift = GunRangeNum(*Cycle, TEXT("lift"), G.CycleLift);
			G.CycleBack = GunRangeNum(*Cycle, TEXT("back"), G.CycleBack);
		}
		G.bReloadBolt = J->HasField(TEXT("reloadBolt")) && J->GetBoolField(TEXT("reloadBolt"));
		const TSharedPtr<FJsonObject>* Pump = nullptr;
		if (J->TryGetObjectField(TEXT("pump"), Pump))
		{
			G.bPump = true;
			G.PumpStart = GunRangeNum(*Pump, TEXT("start"), G.PumpStart);
			G.PumpSeconds = GunRangeNum(*Pump, TEXT("seconds"), G.PumpSeconds);
		}
		const TSharedPtr<FJsonObject>* Scope = nullptr;
		if (J->TryGetObjectField(TEXT("scope"), Scope))
		{
			G.ScopeFov = FMath::Clamp(GunRangeNum(*Scope, TEXT("fov"), 20.0f), 2.0f, 60.0f);
		}
		Read.Add(MoveTemp(G));
	}
	Guns = MoveTemp(Read);
	// The melee weapons on the rack after the guns (the combat range): drawn and swung by the main game, in third person.
	Melee.Reset();
	const TArray<TSharedPtr<FJsonValue>>* MeleeList = nullptr;
	if (Root->TryGetArrayField(TEXT("melee"), MeleeList))
	{
		for (const TSharedPtr<FJsonValue>& Value : *MeleeList)
		{
			const TSharedPtr<FJsonObject> J = Value->AsObject();
			if (J.IsValid())
			{
				Melee.Add({GunRangeStr(J, TEXT("def")), GunRangeStr(J, TEXT("label"))});
			}
		}
	}
	MeshCache.Reset();
	const int32 Was = Current;
	Current = -1;
	CurrentDef.Reset();
	UE_LOG(LogGunRange, Display, TEXT("Read %d guns from %s"), Guns.Num(), *File);
	(void)Was;
	return true;
}

UAnimSequence* UGunRangeSubsystem::LoadClip(const FString& Rel)
{
	if (Rel.IsEmpty())
	{
		return nullptr;
	}
	if (const TWeakObjectPtr<UAnimSequence>* Found = Clips.Find(Rel); Found && Found->IsValid())
	{
		return Found->Get();
	}
	const FString Path = AnimRoot / Rel + TEXT(".") + FPaths::GetBaseFilename(Rel);
	UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Clip && !MissingClips.Contains(Rel))
	{
		MissingClips.Add(Rel);
		UE_LOG(LogGunRange, Warning, TEXT("No clip %s"), *Path);
	}
	Clips.Add(Rel, Clip);
	return Clip;
}

UStaticMesh* UGunRangeSubsystem::FindMesh(const FString& Art, const FString& Match)
{
	const FString Key = Art + TEXT("|") + Match;
	if (const TWeakObjectPtr<UStaticMesh>* Found = MeshCache.Find(Key); Found && Found->IsValid())
	{
		return Found->Get();
	}
	UStaticMesh* Best = nullptr;
	if (!Art.IsEmpty())
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(FString(GunRangeArtRoot) / Art), Assets, true);
		// The largest mesh whose name has Match in it (an empty Match: the largest of all).
		double BestSize = -1.0;
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName())
			{
				continue;
			}
			if (!Match.IsEmpty() && !Asset.AssetName.ToString().Contains(Match))
			{
				continue;
			}
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.GetAsset()))
			{
				const double Size = Mesh->GetBoundingBox().GetSize().SizeSquared();
				if (Size > BestSize)
				{
					BestSize = Size;
					Best = Mesh;
				}
			}
		}
	}
	MeshCache.Add(Key, Best);
	return Best;
}

bool UGunRangeSubsystem::HasModel(int32 Index) const
{
	return Guns.IsValidIndex(Index) && const_cast<UGunRangeSubsystem*>(this)->FindMesh(Guns[Index].ArtName, Guns[Index].Parts.IsEmpty() ? FString() : Guns[Index].Parts[0].Match) != nullptr;
}

float UGunRangeSubsystem::GetReloadFraction() const
{
	const UGunViewAnimInstance* Anim = Source ? Cast<UGunViewAnimInstance>(Source->GetAnimInstance()) : nullptr;
	return Anim ? Anim->GetActionFraction() : -1.0f;
}

bool UGunRangeSubsystem::SelectGun(int32 Index)
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || !Rack.IsValidIndex(Index) || Rack[Index] == colony::kNoEntity)
	{
		return false;
	}
	const colony::ThingView* Me = Bridge->GetCurrentSnapshot().find(Protagonist->GetControlled());
	if (!Me || Me->dead)
	{
		return false;
	}
	const FString Wanted = Guns.IsValidIndex(Index) ? Guns[Index].Def : (Melee.IsValidIndex(Index - Guns.Num()) ? Melee[Index - Guns.Num()].Def : FString());
	if (Me->weapon.valid() && Bridge->GetDefs() && !Wanted.IsEmpty() && Bridge->GetDefs()->get(Me->weapon).defName() == TCHAR_TO_UTF8(*Wanted))
	{
		return true;  // in the hands already
	}
	Bridge->Submit(colony::EquipCommand{Me->id, Rack[Index]});
	return true;
}

void UGunRangeSubsystem::OnSimEvent(const colony::Event& Event)
{
	if (!bEnabled || Event.type != colony::EventType::ShotFired)
	{
		return;
	}
	const UProtagonistSubsystem* Protagonist = GetWorld() ? GetWorld()->GetSubsystem<UProtagonistSubsystem>() : nullptr;
	if (!Protagonist || !Protagonist->IsDirectMode() || Event.thing != Protagonist->GetControlled())
	{
		return;
	}
	++ShotsSeen;
	SinceShot = 0.0f;
	KickVelocity += 1.0f;
	FlashAge = 0.0f;
}

void UGunRangeSubsystem::HideAll()
{
	bShown = false;
	bScoped = false;
	if (ViewRoot)
	{
		ViewRoot->SetVisibility(false, true);
	}
	if (Flash)
	{
		Flash->SetVisibility(false);
	}
}

bool UGunRangeSubsystem::EnsureRig(ASimPawnActor& Player)
{
	USkeletalMeshComponent* PlayerBody = Player.GetBody();
	USkeletalMeshComponent* PlayerMh = Player.GetMakeHumanMesh();
	if (!PlayerBody || !PlayerMh || !PlayerBody->GetSkeletalMeshAsset() || !PlayerMh->GetSkeletalMeshAsset())
	{
		return false;
	}
	UWorld* World = GetWorld();
	if (!Holder)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
		ViewRoot = NewObject<USceneComponent>(Holder, TEXT("GunViewRoot"));
		ViewRoot->SetMobility(EComponentMobility::Movable);
		Holder->SetRootComponent(ViewRoot);
		ViewRoot->RegisterComponent();

		Source = NewObject<USkeletalMeshComponent>(Holder, TEXT("GunViewSource"));
		Source->SetupAttachment(ViewRoot);
		Source->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Source->SetCastShadow(false);
		Source->RegisterComponent();
		Source->SetVisibility(false, false);
		Source->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

		Arms = NewObject<USkeletalMeshComponent>(Holder, TEXT("GunViewArms"));
		Arms->SetupAttachment(Source);
		Arms->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Arms->SetCastShadow(false);
		Arms->bReceivesDecals = false;
		Arms->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		Arms->RegisterComponent();

		GunFrame = NewObject<USceneComponent>(Holder, TEXT("GunViewGun"));
		GunFrame->SetupAttachment(ViewRoot);
		GunFrame->SetMobility(EComponentMobility::Movable);
		GunFrame->RegisterComponent();

		HeldMag = NewObject<UStaticMeshComponent>(Holder, TEXT("GunViewHeldMag"));
		HeldMag->SetupAttachment(ViewRoot);
		HeldMag->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		HeldMag->SetCastShadow(false);
		HeldMag->RegisterComponent();
		HeldMag->SetVisibility(false);

		DroppedMag = NewObject<UStaticMeshComponent>(Holder, TEXT("GunViewDroppedMag"));
		DroppedMag->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		DroppedMag->SetMobility(EComponentMobility::Movable);
		DroppedMag->RegisterComponent();
		DroppedMag->SetVisibility(false);

		Flash = NewObject<UPointLightComponent>(Holder, TEXT("GunViewFlash"));
		Flash->SetupAttachment(ViewRoot);
		Flash->SetMobility(EComponentMobility::Movable);
		Flash->SetIntensityUnits(ELightUnits::Candelas);
		Flash->SetLightColor(FLinearColor(1.0f, 0.7f, 0.35f));
		Flash->SetAttenuationRadius(900.0f);
		Flash->SetCastShadows(false);
		Flash->RegisterComponent();
		Flash->SetVisibility(false);
		CopiedFrom = nullptr;
	}
	if (Source->GetSkeletalMeshAsset() != PlayerBody->GetSkeletalMeshAsset())
	{
		Source->SetSkeletalMeshAsset(PlayerBody->GetSkeletalMeshAsset());
		Source->SetAnimInstanceClass(UGunViewAnimInstance::StaticClass());
		Current = -1;
		CurrentDef.Reset();
	}
	if (CopiedFrom.Get() != PlayerMh || Arms->GetSkeletalMeshAsset() != PlayerMh->GetSkeletalMeshAsset())
	{
		Arms->SetSkeletalMeshAsset(PlayerMh->GetSkeletalMeshAsset());
		Arms->SetAnimInstanceClass(UMakeHumanAnimInstance::StaticClass());
		if (UMakeHumanAnimInstance* Anim = Cast<UMakeHumanAnimInstance>(Arms->GetAnimInstance()))
		{
			if (const UMakeHumanAnimInstance* PlayerAnim = Cast<UMakeHumanAnimInstance>(PlayerMh->GetAnimInstance()))
			{
				Anim->SetRetargeter(PlayerAnim->GetRetargeter());
			}
			Anim->SetSoftTissueEnabled(false);  // the arms are seen from inside the chest: no jiggle to see
		}
		// Only the arms and the chest show: no head (the view is in it), hair or legs.
		Arms->HideBoneByName(TEXT("neck_01"), PBO_None);
		Arms->HideBoneByName(TEXT("thigh_l"), PBO_None);
		Arms->HideBoneByName(TEXT("thigh_r"), PBO_None);
		CopiedFrom = PlayerMh;
		LooksCopyTimer = 0.0f;
	}
	LooksCopyTimer -= World->GetDeltaSeconds();
	if (LooksCopyTimer <= 0.0f)
	{
		CopyLooks(Player);
		LooksCopyTimer = 1.0f;
	}
	return Source->GetAnimInstance() != nullptr;
}

void UGunRangeSubsystem::CopyLooks(ASimPawnActor& Player)
{
	USkeletalMeshComponent* From = Player.GetMakeHumanMesh();
	if (!From || !Arms)
	{
		return;
	}
	for (int32 i = 0; i < From->GetNumMaterials(); ++i)
	{
		if (Arms->GetMaterial(i) != From->GetMaterial(i))
		{
			Arms->SetMaterial(i, From->GetMaterial(i));
		}
	}
	const TArray<float>& Data = From->GetCustomPrimitiveData().Data;
	for (int32 i = 0; i < Data.Num(); ++i)
	{
		Arms->SetCustomPrimitiveDataFloat(i, Data[i]);
	}
	if (const USkeletalMesh* Asset = From->GetSkeletalMeshAsset())
	{
		for (const UMorphTarget* Morph : Asset->GetMorphTargets())
		{
			if (Morph)
			{
				Arms->SetMorphTarget(Morph->GetFName(), From->GetMorphTarget(Morph->GetFName()), false);
			}
		}
	}
	Arms->SetRelativeScale3D(From->GetRelativeScale3D());
}

void UGunRangeSubsystem::SetupGun(int32 Index)
{
	Current = Index;
	for (UStaticMeshComponent* Part : PartComponents)
	{
		if (Part)
		{
			Part->DestroyComponent();
		}
	}
	PartComponents.Reset();
	bHaveCorrection = false;
	ValidFrames = 0;
	Dip = 1.0f;
	bFallback = false;
	bWasReloading = false;
	bMagOut = false;
	if (!Guns.IsValidIndex(Index) || !GunFrame)
	{
		return;
	}
	const FGunRangeGun& G = Guns[Index];
	const FTransform MeshFix(G.MeshRotation, G.MeshOffset);
	bool bAny = false;
	for (const FGunRangeGun::FPart& PartDef : G.Parts)
	{
		UStaticMesh* Mesh = FindMesh(G.ArtName, PartDef.Match);
		UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(Holder, *FString::Printf(TEXT("GunPart_%s"), *PartDef.Name));
		Part->SetupAttachment(GunFrame);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetCastShadow(false);
		Part->SetMobility(EComponentMobility::Movable);
		Part->SetStaticMesh(Mesh);
		Part->RegisterComponent();
		Part->SetRelativeTransform(MeshFix);
		Part->ComponentTags.Add(FName(*PartDef.Name));
		PartComponents.Add(Part);
		bAny |= Mesh != nullptr;
	}
	if (!bAny)
	{
		// No model imported yet: a plain dark box the gun's size stands in, so the range still works.
		if (!FallbackMesh)
		{
			FallbackMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		}
		UStaticMeshComponent* Part = PartComponents.IsEmpty() ? nullptr : PartComponents[0].Get();
		if (Part && FallbackMesh)
		{
			Part->SetStaticMesh(FallbackMesh);
			const float Length = FMath::Max(G.Muzzle.X + 20.0f, 20.0f);
			// Kept here: the moving-parts pass sets every part's transform each frame.
			FallbackFix = FTransform(FQuat::Identity, FVector(G.Muzzle.X - Length * 0.5f, 0.0f, 1.0f), FVector(Length / 100.0f, 0.035f, 0.055f));
			Part->SetRelativeTransform(FallbackFix);
			bFallback = true;
		}
		UE_LOG(LogGunRange, Warning, TEXT("%s: no model in %s/%s (run the GunArt commandlet); a box stands in"), *G.Label, GunRangeArtRoot, *G.ArtName);
	}
	if (UGunViewAnimInstance* Anim = Source ? Cast<UGunViewAnimInstance>(Source->GetAnimInstance()) : nullptr)
	{
		Anim->StopAction(0.1f);
		Anim->SetStance(LoadClip(G.Stance), G.StanceHold, 0.3f);
	}
	if (HeldMag)
	{
		HeldMag->SetVisibility(false);
	}
}

void UGunRangeSubsystem::UpdateInput()
{
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (!PC)
	{
		return;
	}
	static const FKey Keys[] = {EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine, EKeys::Zero};
	for (int32 i = 0; i < static_cast<int32>(UE_ARRAY_COUNT(Keys)) && i < Rack.Num(); ++i)
	{
		if (PC->WasInputKeyJustPressed(Keys[i]))
		{
			SelectGun(i);
		}
	}
	const int32 Count = Rack.Num();
	if (Count > 0 && (PC->WasInputKeyJustPressed(EKeys::MouseScrollUp) || PC->WasInputKeyJustPressed(EKeys::MouseScrollDown)))
	{
		const int32 Step = PC->WasInputKeyJustPressed(EKeys::MouseScrollUp) ? -1 : 1;
		SelectGun(((HeldRack < 0 ? (Current < 0 ? 0 : Current) : HeldRack) + Step + Count) % Count);
	}
	if (PC->WasInputKeyJustPressed(EKeys::F6))
	{
		LoadConfig();
	}
	// [ and ] change the mouse speed (the same saved setting as the Settings window's slider).
	if (PC->WasInputKeyJustPressed(EKeys::LeftBracket) || PC->WasInputKeyJustPressed(EKeys::RightBracket))
	{
		const float Factor = PC->WasInputKeyJustPressed(EKeys::RightBracket) ? 1.15f : 1.0f / 1.15f;
		AProtagonistPawn::SetMouseTurn(AProtagonistPawn::GetMouseTurn() * Factor);
	}
}

void UGunRangeSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bEnabled)
	{
		return;
	}
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Pawns)
	{
		return;
	}
	UpdateInput();
	DrawPanel();
	AProtagonistPawn* Body = Protagonist->GetBody();
	ASimPawnActor* Player = Protagonist->IsDirectMode() ? Pawns->FindPawn(Protagonist->GetControlled()) : nullptr;
	const colony::ThingView* Me = Player ? Bridge->GetCurrentSnapshot().find(Protagonist->GetControlled()) : nullptr;
	// What's in the hands, as a rack index; a melee weapon puts the view in third person (the main game draws the swings
	// on the whole body), and taking a gun again brings first person back if the range was the one that left it.
	if (Me && Body)
	{
		const FString Held = Me->weapon.valid() && Bridge->GetDefs() ? FString(UTF8_TO_TCHAR(Bridge->GetDefs()->get(Me->weapon).defName().c_str())) : FString();
		HeldRack = -1;
		for (int32 i = 0; i < Guns.Num() + Melee.Num(); ++i)
		{
			if (!Held.IsEmpty() && Held == (i < Guns.Num() ? Guns[i].Def : Melee[i - Guns.Num()].Def))
			{
				HeldRack = i;
			}
		}
		// Only when the weapon changes, so V still works with either in the hands.
		if (HeldRack != LastHeldRack)
		{
			LastHeldRack = HeldRack;
			const bool bMeleeHeld = HeldRack >= Guns.Num();
			if (bMeleeHeld && Body->IsFirstPerson())
			{
				Body->SetFirstPerson(false);
				bAutoThirdPerson = true;
			}
			else if (!bMeleeHeld && HeldRack >= 0 && bAutoThirdPerson && !Body->IsFirstPerson())
			{
				Body->SetFirstPerson(true);
				bAutoThirdPerson = false;
			}
		}
	}
	if (!Body || !Player || !Me || Me->downed || Me->dead || !Body->IsFirstPerson() || !EnsureRig(*Player))
	{
		HideAll();
		return;
	}
	// Which gun is in the hands (the sim says); one the range doesn't know shows nothing.
	const FString Def = Me->weapon.valid() && Bridge->GetDefs() ? FString(UTF8_TO_TCHAR(Bridge->GetDefs()->get(Me->weapon).defName().c_str())) : FString();
	if (Def != CurrentDef)
	{
		CurrentDef = Def;
		int32 Index = -1;
		for (int32 i = 0; i < Guns.Num(); ++i)
		{
			if (Guns[i].Def == Def)
			{
				Index = i;
			}
		}
		SetupGun(Index);
	}
	if (!Guns.IsValidIndex(Current))
	{
		HideAll();
		return;
	}
	UpdateView(DeltaTime, *Player);
}

void UGunRangeSubsystem::UpdateView(float Dt, ASimPawnActor& Player)
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	AProtagonistPawn* Body = Protagonist->GetBody();
	UCameraComponent* Camera = Body->GetCamera();
	UGunViewAnimInstance* Anim = Cast<UGunViewAnimInstance>(Source->GetAnimInstance());
	const FGunRangeGun& G = Guns[Current];
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::ThingView* Me = Bridge->GetCurrentSnapshot().find(Pawn);
	if (!Camera || !Anim || !Me)
	{
		HideAll();
		return;
	}
	if (ViewRoot->GetAttachParent() != Camera)
	{
		ViewRoot->AttachToComponent(Camera, FAttachmentTransformRules::KeepRelativeTransform);
	}

	// --- The reload: the clip over the stance, stretched to the sim's reload time. ---
	if (Me->reloading && !bWasReloading)
	{
		ReloadSeconds = FMath::Max(0.5f, static_cast<float>(Bridge->GetSim()->combat().shotReadyIn(Pawn)) / 30.0f);
		ReloadClock = 0.0f;
		bReloadPlaying = true;
		++ReloadsSeen;
		if (UAnimSequence* Clip = LoadClip(G.Reload))
		{
			const float To = G.ReloadTo > G.ReloadFrom ? G.ReloadTo : Clip->GetPlayLength();
			Anim->PlayAction(Clip, G.ReloadFrom, To, (To - G.ReloadFrom) / ReloadSeconds, 0.25f, 0.35f);
		}
	}
	bWasReloading = Me->reloading;
	float ReloadF = -1.0f;
	if (bReloadPlaying)
	{
		ReloadClock += Dt * static_cast<float>(FMath::Max(Bridge->GetSpeed(), 0));
		ReloadF = FMath::Clamp(ReloadClock / ReloadSeconds, 0.0f, 1.0f);
		if (ReloadF >= 1.0f || (!Me->reloading && ReloadClock > 0.2f))
		{
			bReloadPlaying = false;
			ReloadF = -1.0f;
		}
	}
	const float ActionWeight = Anim->GetActionWeight();

	// --- Where the gun is in the arms' hands (root space): from the right hand's knuckles. ---
	const FTransform RootWorld = ViewRoot->GetComponentTransform();
	const auto Bone = [this, &RootWorld](const TCHAR* Name) { return RootWorld.InverseTransformPosition(Arms->GetSocketLocation(Name)); };
	const FVector Hand = Bone(TEXT("hand_r"));
	const FVector Index = Bone(TEXT("index_01_r"));
	const FVector Pinky = Bone(TEXT("pinky_01_r"));
	const FVector Middle = Bone(TEXT("middle_01_r"));
	const FVector GripAxis = (Index - Pinky).GetSafeNormal();
	const FVector Along = (Middle - Hand).GetSafeNormal();
	const FVector Forward = (Along - FVector::DotProduct(Along, GripAxis) * GripAxis).GetSafeNormal();
	const bool bPoseValid = !GripAxis.IsNearlyZero() && !Forward.IsNearlyZero() && FVector::Dist(Hand, Middle) > 2.0f;
	ValidFrames = bPoseValid ? ValidFrames + 1 : 0;
	if (!bPoseValid)
	{
		HideAll();
		return;
	}
	const float Rake = FMath::DegreesToRadians(G.Rake);
	const FVector GunX = FMath::Sin(Rake) * GripAxis + FMath::Cos(Rake) * Forward;
	const FVector GunZ = FMath::Cos(Rake) * GripAxis - FMath::Sin(Rake) * Forward;
	const FQuat GunRot = FRotationMatrix::MakeFromXZ(GunX, GunZ).ToQuat() * G.GripTurn.Quaternion();
	const FVector Palm = (Index + Pinky) * 0.5f + (Hand - (Index + Pinky) * 0.5f) * 0.35f;
	const FVector GunLoc = Palm - GunRot.RotateVector(G.Grip);
	const FTransform GunInRoot(GunRot, GunLoc);

	// --- The aim: turn the arms so the barrel runs along the view (held still through a reload and a switch). ---
	const FVector BarrelInRoot = GunRot.GetForwardVector();
	const FVector UpInRoot = GunRot.GetUpVector();
	const FQuat Target = FRotationMatrix::MakeFromXZ(BarrelInRoot, UpInRoot).ToQuat().Inverse();
	const bool bFrozen = ActionWeight > 0.05f || Dip > 0.6f;
	if (!bHaveCorrection)
	{
		AimCorrection = Target;
		bHaveCorrection = true;
	}
	else if (!bFrozen)
	{
		AimCorrection = FQuat::Slerp(AimCorrection, Target, 1.0f - FMath::Exp(-14.0f * Dt));
	}

	// --- Procedural layers: kick, sway, bob, the switch dip, sprinting. ---
	SinceShot += Dt;
	// Kick: each shot sets a pulse; the gun snaps back to it in a few hundredths of a second and settles as it fades.
	KickVelocity = FMath::Min(KickVelocity, 1.6f) * FMath::Exp(-G.KickRecover * Dt);
	Kick = FMath::Lerp(Kick, KickVelocity, 1.0f - FMath::Exp(-45.0f * Dt));
	float MouseX = 0.0f, MouseY = 0.0f;
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0))
	{
		PC->GetInputMouseDelta(MouseX, MouseY);
	}
	const float Ads = GunRangeSmooth(Body->GetAimBlend());
	const FVector2D SwayTarget(FMath::Clamp(-MouseX * 0.35f, -3.5f, 3.5f), FMath::Clamp(-MouseY * 0.35f, -3.5f, 3.5f));
	Sway = FMath::Lerp(Sway, SwayTarget * (1.0f - 0.7f * Ads), 1.0f - FMath::Exp(-8.0f * Dt));
	const float Speed = static_cast<float>(Body->GetLocalVelocity().Size());
	BobPhase += Dt * Speed * 2.1f;
	const float BobAmount = FMath::Min(Speed / 4.0f, 1.3f) * (1.0f - 0.85f * Ads);
	Dip = FMath::Max(0.0f, Dip - Dt / GunRangeSwitchSeconds);
	SprintBlend = FMath::FInterpTo(SprintBlend, Body->IsSprinting() ? 1.0f : 0.0f, Dt, 7.0f);

	// --- Place the arms: the body hangs from the head (the view is the eyes), and the right hand reaches so the sight
	// is at its spot before the eye (aimed) or lower right (from the hip). Moving the body to bring the sight there
	// instead put the camera inside a shoulder: the stance clips don't hold the sight before the eye. ---
	const FVector SightInRoot = GunInRoot.TransformPosition(G.Sight);
	const FVector HeadInRoot = Bone(TEXT("head"));
	// Through a reload the hand keeps reaching, to the reload spot (the clip still turns the gun and moves the left
	// hand), so the gun stays in view instead of going wherever the clip's third-person hold puts it.
	const FVector Spot = FMath::Lerp(FVector(G.SightDistance, 0.0f, 0.0f) + G.Hip * (1.0f - Ads), G.ReloadSpot, FMath::Clamp(ActionWeight, 0.0f, 1.0f));
	const float DipEase = GunRangeSmooth(Dip);
	{
		// Without the procedural layers (the reach must not undo the kick or the dip): where the sight is now, how far
		// the hand has to go. The hand read this frame already carries last frame's reach, so this settles at once.
		const FVector SightBase = HeadInView + AimCorrection.RotateVector(SightInRoot - HeadInRoot);
		const FVector Shift = AimCorrection.UnrotateVector(Spot - SightBase);
		const FTransform ArmsInRoot = Arms->GetComponentTransform().GetRelativeTransform(ViewRoot->GetComponentTransform());
		const float Reach = G.bReach ? 1.0f : 0.0f;
		if (UMakeHumanAnimInstance* ArmsAnim = Cast<UMakeHumanAnimInstance>(Arms->GetAnimInstance()))
		{
			const FVector TargetRoot = Hand + Shift;
			// The elbow out to the right and down, as a shooter holds it.
			const FVector ElbowRoot = TargetRoot + AimCorrection.UnrotateVector(FVector(-30.0f, 25.0f, -30.0f));
			ArmsAnim->SetRightHandReach(Reach, ArmsInRoot.InverseTransformPosition(TargetRoot), ArmsInRoot.InverseTransformPosition(ElbowRoot));
		}
	}
	const FQuat Procedural = FRotator(G.KickUp * Kick - 38.0f * DipEase - 14.0f * SprintBlend + Sway.Y, Sway.X + 22.0f * SprintBlend, 6.0f * SprintBlend + FMath::Sin(BobPhase) * 0.8f * BobAmount).Quaternion();
	const FQuat RootRot = Procedural * AimCorrection;
	FVector RootLoc = HeadInView - RootRot.RotateVector(HeadInRoot);
	RootLoc += FVector(-G.KickBack * Kick, 0.0f, 0.0f);
	RootLoc += FVector(0.0f, FMath::Sin(BobPhase) * 0.7f, -FMath::Abs(FMath::Cos(BobPhase)) * 0.6f) * BobAmount;
	RootLoc += FVector(-4.0f, 4.0f, -22.0f) * DipEase + FVector(-4.0f, 3.0f, -5.0f) * SprintBlend;
	ViewRoot->SetRelativeLocationAndRotation(RootLoc, RootRot);
	GunFrame->SetRelativeTransform(GunInRoot);

	// Show once the pose is settled (the first frames have no animation yet). Through a scope (fully aimed, not
	// reloading or switching) the arms and gun go and the view narrows to the scope's.
	bScoped = G.ScopeFov > 0.0f && Ads > 0.93f && ActionWeight < 0.05f && Dip < 0.05f && ValidFrames >= 3;
	if (bScoped)
	{
		Camera->SetFieldOfView(G.ScopeFov);
	}
	// The free hand puts the gun away: the body's own arm shows instead (docs/design/free-hand.md).
	const bool bShow = ValidFrames >= 3 && !bScoped && !UFreeHandSubsystem::IsActive(GetWorld());
	if (bShow != bShown || Arms->IsVisible() != bShow)
	{
		ViewRoot->SetVisibility(bShow, true);
		Source->SetVisibility(false, false);
		HeldMag->SetVisibility(false);
		bShown = bShow;
	}
	{
		const FVector BarrelCam = RootRot.RotateVector(BarrelInRoot);
		BarrelOffDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(BarrelCam.X, -1.0, 1.0)));
		const FVector SightCam = RootLoc + RootRot.RotateVector(SightInRoot);
		SightOffCm = static_cast<float>(FVector2D(SightCam.Y, SightCam.Z).Size());
		BoneLine.Reset();
		for (const TCHAR* Name : {TEXT("head"), TEXT("neck_01"), TEXT("spine_05"), TEXT("clavicle_r"), TEXT("upperarm_r"), TEXT("lowerarm_r"), TEXT("hand_r"),
			TEXT("clavicle_l"), TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("hand_l")})
		{
			if (Arms->GetBoneIndex(Name) != INDEX_NONE)
			{
				const FVector At = RootLoc + RootRot.RotateVector(Bone(Name));
				BoneLine += FString::Printf(TEXT("%s(%.0f,%.0f,%.0f) "), Name, At.X, At.Y, At.Z);
			}
		}
	}

	// --- Moving parts: the bolt cycling after a shot and opening for a reload, the trigger. ---
	for (UStaticMeshComponent* Part : PartComponents)
	{
		if (!Part)
		{
			continue;
		}
		const FTransform MeshFix(G.MeshRotation, G.MeshOffset);
		FTransform Local = MeshFix;
		if (bFallback)
		{
			Part->SetRelativeTransform(Part == PartComponents[0] ? FallbackFix : MeshFix);
			continue;
		}
		for (const FGunRangeGun::FPart& PartDef : G.Parts)
		{
			if (!Part->ComponentHasTag(FName(*PartDef.Name)))
			{
				continue;
			}
			if (G.bCycle && PartDef.Name == G.CyclePart)
			{
				float U = (SinceShot - G.CycleStart) / FMath::Max(G.CycleSeconds, 0.05f);
				if (G.bReloadBolt && ReloadF >= 0.0f)
				{
					// Open early in the reload, closed at the end.
					U = ReloadF < 0.12f ? ReloadF / 0.12f * 0.42f : (ReloadF < 0.85f ? 0.5f : 0.58f + (ReloadF - 0.85f) / 0.15f * 0.42f);
				}
				const FVector2D Bolt = GunRangeBolt(U, G.CycleLift, G.CycleBack);
				const FTransform Turn(FQuat(FVector::ForwardVector, FMath::DegreesToRadians(-Bolt.X)));
				const FTransform AroundPivot = FTransform(-PartDef.Pivot) * Turn * FTransform(PartDef.Pivot + FVector(Bolt.Y, 0.0f, 0.0f));
				Local = AroundPivot * MeshFix;
			}
			else if (PartDef.Name == TEXT("trigger"))
			{
				const float Pull = SinceShot < 0.12f ? FMath::Sin(SinceShot / 0.12f * PI) : 0.0f;
				const FTransform Turn(FRotator(0.0f, 0.0f, 0.0f).Quaternion() * FQuat(FVector::RightVector, FMath::DegreesToRadians(14.0f * Pull)));
				Local = FTransform(-PartDef.Pivot) * Turn * FTransform(PartDef.Pivot) * MeshFix;
			}
		}
		Part->SetRelativeTransform(Local);
	}

	// --- The magazine: out into the left hand, the empty one falls, the full one goes in. ---
	UStaticMeshComponent* MagPart = nullptr;
	if (G.bMagazine && !G.MagazinePart.IsEmpty())
	{
		for (UStaticMeshComponent* Part : PartComponents)
		{
			if (Part && Part->ComponentHasTag(FName(*G.MagazinePart)))
			{
				MagPart = Part;
			}
		}
	}
	const bool bOut = ReloadF >= G.MagazineOut && ReloadF < G.MagazineIn;
	if (MagPart && MagPart->GetStaticMesh())
	{
		if (bOut && !bMagOut)
		{
			// The empty one drops from where it was, with the gun's motion.
			DroppedMag->SetStaticMesh(MagPart->GetStaticMesh());
			DroppedMag->SetWorldTransform(MagPart->GetComponentTransform());
			DroppedMag->SetVisibility(true);
			DropVelocity = Camera->GetComponentQuat().RotateVector(FVector(10.0f, 15.0f, -60.0f));
			DropAge = 0.0f;
		}
		bMagOut = bOut;
		MagPart->SetVisibility(bShown && !bOut);
		HeldMag->SetStaticMesh(MagPart->GetStaticMesh());
		HeldMag->SetVisibility(bShown && bOut && ReloadF > G.MagazineOut + (G.MagazineIn - G.MagazineOut) * 0.35f);
		if (HeldMag->IsVisible())
		{
			const FTransform LeftHand = Arms->GetSocketTransform(TEXT("hand_l"));
			HeldMag->SetWorldLocationAndRotation(LeftHand.TransformPosition(FVector(0.0f, 8.0f, 2.0f)), MagPart->GetComponentQuat());
		}
	}
	if (DropAge < 1.5f)
	{
		DropAge += Dt;
		DropVelocity += FVector(0.0f, 0.0f, -980.0f) * Dt;
		DroppedMag->AddWorldOffset(DropVelocity * Dt);
		DroppedMag->AddWorldRotation(FRotator(200.0f * Dt, 90.0f * Dt, 0.0f));
		if (DropAge >= 1.5f)
		{
			DroppedMag->SetVisibility(false);
		}
	}

	// --- The left hand on the fore-end (off through the reload, which moves it itself). ---
	if (UMakeHumanAnimInstance* ArmsAnim = Cast<UMakeHumanAnimInstance>(Arms->GetAnimInstance()))
	{
		const float Alpha = G.bOffHand ? FMath::Clamp(1.0f - ActionWeight * 1.5f, 0.0f, 1.0f) * (1.0f - DipEase) : 0.0f;
		if (Alpha > 0.0f)
		{
			const FTransform GunWorld = GunFrame->GetComponentTransform();
			const FTransform RightHand = Arms->GetSocketTransform(TEXT("hand_r"));
			const FVector GripTarget = GunWorld.TransformPosition(G.OffHand);
			const FVector GripLocal = RightHand.InverseTransformPosition(GripTarget);
			const FVector Elbow = GripTarget + Camera->GetComponentQuat().RotateVector(FVector(-25.0f, -25.0f, -35.0f));
			ArmsAnim->SetLeftHandGrip(Alpha, GripLocal, Elbow);
		}
		else
		{
			ArmsAnim->SetLeftHandGrip(0.0f, FVector::ZeroVector, FVector::ZeroVector);
		}
	}

	// --- The muzzle flash: a short warm light at the muzzle. ---
	FlashAge += Dt;
	const bool bFlash = FlashAge < GunRangeFlashSeconds && bShown;
	Flash->SetVisibility(bFlash);
	if (bFlash)
	{
		Flash->SetWorldLocation(GunFrame->GetComponentTransform().TransformPosition(G.Muzzle + FVector(6.0f, 0.0f, 0.0f)));
		Flash->SetIntensity(6000.0f * (1.0f - FlashAge / GunRangeFlashSeconds));
	}
}

void UGunRangeSubsystem::DrawPanel()
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
	ImGui::GetStyle().FontScaleMain = 1.2f * Scale;
	if (bScoped)
	{
		DrawScope();
	}
	const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + 14.0f * Scale, Viewport->Pos.y + 14.0f * Scale), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.55f);
	if (ImGui::Begin("##GunRange", nullptr, Flags))
	{
		ImGui::TextUnformatted("COMBAT RANGE");
		const auto KeyName = [](int32 i) { return i < 9 ? FString::FromInt(i + 1) : FString(TEXT("0")); };
		for (int32 i = 0; i < Guns.Num(); ++i)
		{
			const FString Line = FString::Printf(TEXT("%s%s  %s%s"), i == HeldRack ? TEXT("> ") : TEXT("   "), *KeyName(i), *Guns[i].Label, HasModel(i) ? TEXT("") : TEXT("  (no model yet)"));
			ImGui::TextUnformatted(TCHAR_TO_UTF8(*Line));
		}
		for (int32 m = 0; m < Melee.Num(); ++m)
		{
			const int32 i = Guns.Num() + m;
			const FString Line = FString::Printf(TEXT("%s%s  %s"), i == HeldRack ? TEXT("> ") : TEXT("   "), *KeyName(i), *Melee[m].Label);
			ImGui::TextUnformatted(TCHAR_TO_UTF8(*Line));
		}
		ImGui::Separator();
		if (HeldRack >= Guns.Num())
		{
			ImGui::TextUnformatted("Left mouse swing   Right mouse block   Alt dodge");
		}
		else
		{
			ImGui::TextUnformatted("Left mouse fire   Right mouse aim   R reload");
		}
		ImGui::TextUnformatted(TCHAR_TO_UTF8(*FString::Printf(TEXT("1-%s or wheel: weapon   N: new targets   F2 pause"), *KeyName(FMath::Max(0, Guns.Num() + Melee.Num() - 1)))));
		ImGui::TextUnformatted(TCHAR_TO_UTF8(*FString::Printf(TEXT("V: first/third person   [ ] mouse speed %.2f"), AProtagonistPawn::GetMouseTurn())));
		if (!SceneLine.IsEmpty())
		{
			ImGui::TextUnformatted(TCHAR_TO_UTF8(*SceneLine));
		}
	}
	ImGui::End();
}

void UGunRangeSubsystem::DrawScope()
{
	// The view through the scope: black outside a round field, a thin dark ring at its edge, a post from below and a
	// level line either side (an old sniper scope's reticle). Called inside DrawPanel's ImGui context.
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	ImDrawList* Draw = ImGui::GetBackgroundDrawList();
	const ImVec2 Centre(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.5f);
	const float Radius = Viewport->Size.y * 0.46f;
	const float Far = FMath::Sqrt(Viewport->Size.x * Viewport->Size.x + Viewport->Size.y * Viewport->Size.y);
	const ImU32 Black = IM_COL32(0, 0, 0, 255);
	const ImU32 Edge = IM_COL32(0, 0, 0, 150);
	const ImU32 Line = IM_COL32(8, 8, 8, 235);
	// A ring as thick as the screen covers everything outside the field.
	Draw->AddCircle(Centre, Radius + Far * 0.5f, Black, 128, Far);
	Draw->AddCircle(Centre, Radius - 6.0f, Edge, 128, 14.0f);
	const float Thin = FMath::Max(1.5f, Viewport->Size.y / 600.0f);
	Draw->AddLine(ImVec2(Centre.x - Radius, Centre.y), ImVec2(Centre.x - Radius * 0.08f, Centre.y), Line, Thin * 2.0f);
	Draw->AddLine(ImVec2(Centre.x + Radius * 0.08f, Centre.y), ImVec2(Centre.x + Radius, Centre.y), Line, Thin * 2.0f);
	// The post: wide from the bottom, a fine tip just reaching the centre.
	Draw->AddRectFilled(ImVec2(Centre.x - Thin * 2.5f, Centre.y + Radius * 0.12f), ImVec2(Centre.x + Thin * 2.5f, Centre.y + Radius), Line);
	Draw->AddTriangleFilled(ImVec2(Centre.x - Thin * 2.5f, Centre.y + Radius * 0.12f), ImVec2(Centre.x + Thin * 2.5f, Centre.y + Radius * 0.12f), ImVec2(Centre.x, Centre.y), Line);
}
