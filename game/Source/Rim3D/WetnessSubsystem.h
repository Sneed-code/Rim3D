#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "WetnessSubsystem.generated.h"

class UMaterialParameterCollection;
class USimSubsystem;
class UTexture2D;
namespace colony { class Map; }

/**
 * Rain makes things wet (docs/design/wetness.md), all cosmetic. The ground already soaks with the sky
 * (USkySubsystem::ApplyWetness); this does everything standing on it:
 *
 * - **Surfaces** (walls, roofs, floors left outside, furniture, Codex and Fab buildings, trees, plants, rocks, guns,
 *   animals, vehicles): one wetness for the whole world in MPC_Weather, which soaks in about ten game minutes of rain
 *   and dries in about two game hours after. Each material (patched by -run=WetLook) turns it into darker where porous,
 *   glossy, wettest on top and dry underneath.
 * - **Cover**: T_WeatherCover holds, per cell, the height of the highest roof or upper floor over it, rewritten when a
 *   roof, floor or the mountain changes. The materials keep dry whatever is under it, so a room's floor and furniture
 *   stay dry while its roof and outside walls are soaked. The ground's own wetness is masked the same way.
 * - **People**: each pawn has its own wetness (custom primitive data 12 on every mesh of the pawn: skin, hair and, after
 *   -run=WetLook, clothes): soaked in about five game minutes outdoors in rain, at once when swimming, dry in about
 *   forty game minutes under a roof or once the rain stops.
 *
 * Without the generated assets (run -run=WetLook) only the pawns get wet. Console: Wet.Enabled, Wet.Override (a fixed
 * surface wetness for looking at, -1 for the weather).
 */
UCLASS()
class RIM3D_API UWetnessSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& InCollection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** The surfaces' wetness now (0 dry to 1 soaked), as written to MPC_Weather (Wet.Override and Wet.Enabled included). */
	float GetSurfaceWetness() const { return ShownSurfaceWet; }
	/** True once the cover texture can be written, and how many times it was uploaded to the GPU. */
	bool IsCoverUsable() const { return bCoverUsable; }
	int32 GetCoverUploads() const { return CoverUploads; }
	/** One line on the state of things, for the log (Wet.Dump). */
	FString Describe() const;
	/** Cells with a roof or upper floor over them in the last cover written, and how many times it was written. */
	int32 GetCoveredCells() const { return CoveredCells; }
	int32 GetCoverWrites() const { return CoverWrites; }
	/** Pawns wetter than 0.1 now, and the wettest. */
	int32 GetWetPawns() const { return WetPawns; }
	float GetWettestPawn() const { return WettestPawn; }
	/** True once both generated assets were looked for and found. */
	bool HasAssets() const { return Collection != nullptr && Cover != nullptr; }
	/** Height in metres over the world origin of the highest roof or upper floor over a cell; very low for open sky. */
	float CoverAt(int32 X, int32 Y) const;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void LoadAssets();
	void UpdateCover(const colony::Map& Map, bool bForce);
	/** Sends the cover to the texture when it changed or the texture's GPU copy was made again (it loses what was sent). */
	void UploadCover();
	void UpdateSurfaces(float Elapsed);
	void UpdatePawns(const colony::Map& Map, float Elapsed);

	TWeakObjectPtr<USimSubsystem> Bridge;
	UPROPERTY()
	TObjectPtr<UMaterialParameterCollection> Collection;
	UPROPERTY()
	TObjectPtr<UTexture2D> Cover;
	bool bAssetsTried = false;
	bool bCoverUsable = false;
	bool bCoverFormatLogged = false;
	/** Single floats (32 bit) or halves (16 bit): whichever the texture was built as. */
	bool bCoverHalf = false;
	bool bCoverDirty = false;
	/** The texture's GPU copy the cover was last sent to. */
	const void* UploadedResource = nullptr;
	FVector WrittenOrigin = FVector(TNumericLimits<double>::Max());

	/** The cover as last written, one float per cell of the map (row by row, map width). */
	TArray<float> CoverCells;
	int32 CoverWidth = 0;
	int32 CoverHeight = 0;
	uint64 LastRoofChanges = ~0ULL;
	uint64 LastCollapses = ~0ULL;
	uint64 LastFloorChanges = ~0ULL;
	uint64 LastRockChanges = ~0ULL;
	int32 BuiltGeneration = -1;

	float SurfaceWet = 0.0f;
	float WrittenSurfaceWet = -1.0f;
	float ShownSurfaceWet = 0.0f;
	double LastGameSeconds = -1.0;
	/** Each pawn's wetness, by thing id, and the value last written to its meshes. */
	TMap<uint32, float> PawnWet;
	TMap<uint32, float> PawnWritten;

	int32 CoveredCells = 0;
	int32 CoverWrites = 0;
	int32 CoverUploads = 0;
	int32 WetPawns = 0;
	float WettestPawn = 0.0f;
};
