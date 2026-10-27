#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "PawnAnimInstance.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "PawnSubsystem.generated.h"

class ASimPawnActor;
class UAnimSequence;
class UMaterialInterface;
class UPoseSearchDatabase;
class UStaticMesh;
class USimSubsystem;
class USkeletalMesh;
namespace colony { class DefDatabase; class Sim; struct PawnLooksView; struct Snapshot; struct ThingView; }

/**
 * How a pawn looks, from the sim's PawnLooksView and its lineage's def, in Unreal's terms
 * (docs/design/looks.md). Colours are linear.
 */
struct FSimPawnLooks
{
	float Height = 1.0f;  // scale of the whole body
	float Weight = 0.0f;  // -1 thin to 1 heavy
	bool bFemale = false;
	FLinearColor Skin = FLinearColor(0.15f, 0.08f, 0.06f);
	FLinearColor Hair = FLinearColor(0.02f, 0.015f, 0.01f);
	float HornLength = 0.0f;  // cm; 0 for no horns
	float HornRadius = 0.0f;  // cm, at the base
	float HornSpread = 0.0f;  // cm from the middle of the forehead
	FLinearColor HornColor = FLinearColor::Black;

	bool operator==(const FSimPawnLooks& Other) const
	{
		return Height == Other.Height && Weight == Other.Weight && bFemale == Other.bFemale && Skin == Other.Skin && Hair == Other.Hair
			&& HornLength == Other.HornLength && HornRadius == Other.HornRadius && HornSpread == Other.HornSpread && HornColor == Other.HornColor;
	}

	/** The body shape morphs' weights (PawnLooks.h names) for this weight and gender. */
	TArray<TPair<FName, float>> MorphWeights() const;
};

/** One mesh of a worn piece: a skinned mesh (follows the body) or, with a bone, a prop on that bone (a hat). */
struct FSimWornMesh
{
	FString Path;
	FName Bone;

	bool operator==(const FSimWornMesh& Other) const { return Path == Other.Path && Bone == Other.Bone; }
};

/** A piece of apparel a pawn wears, from the snapshot's ApparelView and its def's `visual.worn` (docs/design/clothes-draw.md). */
struct FSimWornPiece
{
	uint32 Item = 0;
	uint32 Def = 0;
	uint32 Stuff = 0xFFFFFFFFu;
	TArray<FSimWornMesh> Meshes;
	/** The colour of the stuff it is made of (linear); without one the mesh's own colour stays. */
	bool bHasTint = false;
	FLinearColor Tint = FLinearColor::White;
	/** 0 whole to 1 in tatters, in sixteenths: how many holes the cloth has. */
	float Wear = 0.0f;
	bool bHidesHair = false;

	/** Same item, def, stuff and meshes (what a rebuild of the components is for); wear and tint change in place. */
	bool SameStructure(const FSimWornPiece& Other) const { return Item == Other.Item && Def == Other.Def && Stuff == Other.Stuff && Meshes == Other.Meshes; }
};

struct FSimPawnApparel
{
	TArray<FSimWornPiece> Pieces;

	bool SameStructure(const FSimPawnApparel& Other) const
	{
		if (Pieces.Num() != Other.Pieces.Num())
		{
			return false;
		}
		for (int32 I = 0; I < Pieces.Num(); ++I)
		{
			if (!Pieces[I].SameStructure(Other.Pieces[I]))
			{
				return false;
			}
		}
		return true;
	}
};

/** Pawns at each detail level this frame (USimPawnSubsystem). */
struct FSimPawnDetailCounts
{
	int32 Full = 0;
	int32 Near = 0;
	int32 Far = 0;
};

/**
 * Draws the sim's pawns: one ASimPawnActor per pawn in the snapshot, spawned when a pawn
 * appears and removed when it leaves, moved every frame to the blended sim position. Picks each
 * pawn's animation detail by distance to the camera: full motion matching for the nearest
 * (Pawns.FullDetail, 20), loops updated 20 times a second out to Pawns.NearDistance (40 m),
 * 8 times a second beyond; strand hair only at full detail. Sim PawnAction events play short
 * action animations. See docs/design/pawns.md.
 * Console: Pawns.FullDetail, Pawns.NearDistance, Pawns.MetaHumans (0 mannequins, 1 MetaHumans),
 * Pawns.ShowDetail (coloured marks over heads: green full, yellow near, red far).
 */
UCLASS()
class RIM3D_API USimPawnSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Removes every pawn actor; they come back next frame (with the current look). */
	void RemoveAll();

	ASimPawnActor* FindPawn(colony::Entity Thing) const;
	/** Every pawn actor, by the sim entity's number (entt::to_integral). */
	const TMap<uint32, TObjectPtr<ASimPawnActor>>& GetPawns() const { return Pawns; }
	const FSimPawnDetailCounts& GetDetailCounts() const { return DetailCounts; }
	/** PawnAction events heard, and how many of them started an animation. */
	int32 GetActionEvents() const { return ActionEvents; }
	int32 GetActionsStarted() const { return ActionsStarted; }
	/** False if a needed asset (mesh, databases, loops) did not load; the log says which. */
	bool AreAssetsLoaded() const { return bAssetsLoaded; }

	USkeletalMesh* GetBodyMesh() const { return BodyMesh; }
	const TArray<TObjectPtr<const UPoseSearchDatabase>>& GetDatabases() const { return Databases; }
	UAnimSequence* GetIdleLoop() const { return IdleLoop; }
	UAnimSequence* GetWalkLoop() const { return WalkLoop; }
	UAnimSequence* GetRunLoop() const { return RunLoop; }
	float GetWalkLoopSpeed() const { return WalkLoopSpeed; }
	float GetRunLoopSpeed() const { return RunLoopSpeed; }
	UAnimSequence* GetActionAnimation(colony::PawnAction Action) const;
	UClass* GetMetaHumanClass() const { return MetaHumanClass; }

	/** Looks assets from the PawnLooks commandlet (docs/design/looks.md); null if it hasn't been run. */
	USkeletalMesh* GetMorphMesh(const USkeletalMesh* Original);
	UMaterialInterface* GetSkinMaterial(const UMaterialInterface* Original);
	/** Clothes materials that may be used with the body shape morphs. */
	UMaterialInterface* GetClothMaterial(const UMaterialInterface* Original);
	UStaticMesh* GetHornMesh();
	/** The looks a pawn's sim looks and lineage add up to. */
	static FSimPawnLooks MakeLooks(const colony::PawnLooksView& View, const colony::DefDatabase* Defs);
	/** What a pawn wears, from the snapshot (U16), and a number that changes whenever any of it does (cheap: no allocation). */
	static FSimPawnApparel MakeApparel(const colony::Snapshot& Snapshot, const colony::ThingView& View, const colony::DefDatabase* Defs);
	static uint64 ApparelKey(const colony::Snapshot& Snapshot, const colony::ThingView& View);
	/** A clothes mesh made by the ClothesArt commandlet, by its object path (null, logged once, if it is missing). */
	UObject* GetClothesAsset(const FString& Path);

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void LoadAssets();
	void OnSimEvent(const colony::Event& Event);
	void AssignDetail(const FVector& CameraLocation);

	UPROPERTY()
	TMap<uint32, TObjectPtr<ASimPawnActor>> Pawns;

	UPROPERTY()
	TObjectPtr<USkeletalMesh> BodyMesh;

	UPROPERTY()
	TArray<TObjectPtr<const UPoseSearchDatabase>> Databases;

	UPROPERTY()
	TObjectPtr<UAnimSequence> IdleLoop;

	UPROPERTY()
	TObjectPtr<UAnimSequence> WalkLoop;

	UPROPERTY()
	TObjectPtr<UAnimSequence> RunLoop;

	UPROPERTY()
	TObjectPtr<UAnimSequence> CrouchPose;

	UPROPERTY()
	TObjectPtr<UClass> MetaHumanClass;

	/** Looks assets by the name of the sample asset they replace; null where there is none. */
	UPROPERTY()
	TMap<FName, TObjectPtr<USkeletalMesh>> MorphMeshes;

	UPROPERTY()
	TMap<FName, TObjectPtr<UMaterialInterface>> SkinMaterials;

	UPROPERTY()
	TMap<FName, TObjectPtr<UMaterialInterface>> ClothMaterials;

	UPROPERTY()
	TObjectPtr<UStaticMesh> HornMesh;

	/** Clothes meshes by path; null where a mesh is missing. */
	UPROPERTY()
	TMap<FString, TObjectPtr<UObject>> ClothesAssets;

	bool bHornTried = false;
	bool bLooksMissingLogged = false;

	float WalkLoopSpeed = 150.0f;
	float RunLoopSpeed = 400.0f;
	bool bAssetsTried = false;
	bool bAssetsLoaded = false;
	bool bMetaHumansShown = false;
	const colony::Sim* LastSim = nullptr;
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> EventSource;
	FSimPawnDetailCounts DetailCounts;
	int32 ActionEvents = 0;
	int32 ActionsStarted = 0;
};
