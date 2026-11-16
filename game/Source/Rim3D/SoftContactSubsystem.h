#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "SoftContactSubsystem.generated.h"

class USkeletalMeshComponent;
class UTexture2D;

/** Where the contact table lives (made by the MakeHumanArt commandlet, read by M_MH_SkinDetail's SkinSquish). */
inline const TCHAR* const SoftContactTexturePath = TEXT("/Game/Rim3D/Generated/MakeHuman/T_MH_SoftContacts.T_MH_SoftContacts");
/** The table's size: a row per soft body near the camera, two texels per capsule. */
constexpr int32 SoftContactRows = 8;
constexpr int32 SoftContactCapsules = 8;
/** The custom primitive data index that tells the skin material its row (-1: none, no squish). */
constexpr int32 SoftContactRowIndex = 13;

/**
 * Squish on contact (docs/design/soft-body.md, step 2): the skin of the nearest MakeHuman bodies within
 * Pawns.SoftContactFar of the camera (8 at most) is pushed out of what touches it: other bodies' limbs, hands and heads,
 * held weapons and props attached to a body, and a test probe. Each frame this fills a small float texture, one row per
 * body (8 capsules, each A.xyz radius and B.xyz strength), and gives each body its row in custom primitive data 13;
 * M_MH_SkinDetail's SkinSquish node moves the skin in world position offset and bends its normal to match.
 * Pure looks: nothing here reaches the sim.
 */
UCLASS()
class RIM3D_API USoftContactSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(USoftContactSubsystem, STATGROUP_Tickables); }
	virtual bool IsTickableInEditor() const override { return false; }

	/** A test capsule from A to B (cm, world) pressing on any body it reaches; Radius <= 0 takes it away. */
	void SetProbe(const FVector& A, const FVector& B, float Radius);
	/** How many bodies had a row last frame and how many capsules touched them, for the tests. */
	int32 GetBodiesLastFrame() const { return BodiesLastFrame; }
	int32 GetContactsLastFrame() const { return ContactsLastFrame; }
	/** The most capsules touching one body in any frame since the last call, for the tests. */
	int32 TakeMostContacts()
	{
		const int32 Most = MostContacts;
		MostContacts = 0;
		return Most;
	}

private:
	struct FCapsule
	{
		FVector A = FVector::ZeroVector;
		FVector B = FVector::ZeroVector;
		float Radius = 0.0f;
		TWeakObjectPtr<const USkeletalMeshComponent> Owner;  // a body's own capsules never press on it
	};
	void AddBodyCapsules(const USkeletalMeshComponent* Mesh, TArray<FCapsule>& Out) const;
	void AddAttachedCapsules(const USkeletalMeshComponent* Mesh, TArray<FCapsule>& Out) const;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Table;
	bool bTableMissing = false;
	TArray<TWeakObjectPtr<USkeletalMeshComponent>> LastRows;
	FCapsule Probe;
	int32 BodiesLastFrame = 0;
	int32 ContactsLastFrame = 0;
	int32 MostContacts = 0;
};
