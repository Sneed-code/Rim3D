#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "TitleStage.generated.h"

class ASimPawnActor;
class UDirectionalLightComponent;
class UMaterialInterface;
class UPointLightComponent;
class UPostProcessComponent;
class UProceduralMeshComponent;
class USceneComponent;
class USimPawnSubsystem;
class UTexture2D;
namespace colony { class Sim; }

/**
 * The title screen's stage (docs/design/new-game.md "Title", sneed 2026-11-23: "from a space ship window in orbit with
 * the current character in some glitterworld wear looking at the viewer with a world at her back and the galaxy
 * background"). Cosmetic only, nothing in the sim: a starship cabin built at run time (floor, window frame, glowing
 * strips), the player's character standing in it facing the camera in sleek lumen-world clothes, and through the window
 * a lit planet (surface and cloud maps, a thin glowing rim of air) over a galaxy (an inside-out sphere with a glowing
 * map). The maps are PNGs made by tools/title_art.py (art step `title-art`) in game/Saved/TitleArt; without them the
 * planet and sky are plain colours. Its own sun, cabin lights and a fixed exposure; the sky subsystem steps aside
 * (USkySubsystem::SetStageMode) while it stands. Placed high above the map (UColonyHudSubsystem::UpdateTitleStage).
 *
 * Local axes: the camera looks along +X from (0, 0, eye height), +Y is screen right, the window is ahead.
 */
UCLASS(NotBlueprintable, Transient)
class RIM3D_API ATitleStage : public AActor
{
	GENERATED_BODY()

public:
	ATitleStage();
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Builds the cabin, planet, galaxy and lights. Call once after spawning. */
	void Build();

	/** Where the title camera is now (world), with its slow drift of a few centimetres, and its field of view. */
	FTransform GetCameraTransform() const;
	float GetFieldOfView() const;

	/** The character on show (null until the pawn assets have loaded). */
	ASimPawnActor* GetDisplayPawn() const { return DisplayPawn; }
	/** Whether she has her MakeHuman body, and her clothes on (smoke test `newgame`). */
	bool HasBody() const;
	bool IsDressed() const { return bDressed; }
	/** The garments she wears, by name ("UniformJacket+Trousers"). */
	const FString& GetOutfit() const { return Outfit; }
	bool HasPlanet() const;
	bool HasGalaxy() const;
	/** Whether the planet and galaxy maps from tools/title_art.py were found (else plain colours). */
	bool HasPlanetMap() const { return bPlanetMap; }
	bool HasGalaxyMap() const { return bGalaxyMap; }
	/** Where her look came from: "remembered" (the last game's character), "save" (read from the newest save) or "default". */
	const FString& GetLookSource() const { return LookSource; }

private:
	void BuildCabin(UMaterialInterface* Pbr, UMaterialInterface* Plain);
	void BuildPlanet(UMaterialInterface* Pbr, UMaterialInterface* Plain, UMaterialInterface* Ghost);
	void BuildGalaxy(UMaterialInterface* Pbr, UMaterialInterface* Plain);
	void BuildLights();
	void PickLook();
	void UpdatePawn();
	void Dress(USimPawnSubsystem& Pawns);
	/** A surface of M_GunPBR (colour, roughness, glow) or, without it, the plain vertex-colour material. */
	UMaterialInterface* MakeSurface(UMaterialInterface* Pbr, UMaterialInterface* Plain, const FLinearColor& Colour, float Roughness,
		const FLinearColor& Glow = FLinearColor::Black, float GlowStrength = 0.0f, UTexture2D* ColourMap = nullptr, UTexture2D* GlowMap = nullptr);
	/** A 1 x 1 texture of one colour (linear), kept alive by the stage. */
	UTexture2D* SolidTexture(const FLinearColor& Colour);

	UPROPERTY()
	TObjectPtr<USceneComponent> Root;
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Cabin;
	UPROPERTY()
	TObjectPtr<USceneComponent> PlanetAxis;
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Planet;
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> AirGlow;
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Galaxy;
	UPROPERTY()
	TObjectPtr<UDirectionalLightComponent> Sun;
	UPROPERTY()
	TArray<TObjectPtr<UPointLightComponent>> Lamps;
	UPROPERTY()
	TObjectPtr<UPostProcessComponent> Exposure;
	UPROPERTY()
	TArray<TObjectPtr<UTexture2D>> Textures;
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> Materials;
	UPROPERTY()
	TObjectPtr<ASimPawnActor> DisplayPawn;

	FString LookText;
	FString LookSource;
	FString Outfit;
	bool bLookFemale = true;
	bool bDressed = false;
	bool bPlanetMap = false;
	bool bGalaxyMap = false;
	bool bPawnFirstFrame = true;
	float Age = 0.0f;
};

/** Remembering the player's character for the title screen (a small text file in game/Saved/TitleArt). */
namespace TitleStage
{
	/** game/Saved/TitleArt, where tools/title_art.py writes the maps. */
	FString ArtFolder();
	/** The creator look text (PawnLooks::creator) of the game's protagonist, written when a game is saved or loaded. */
	void RememberCharacter(const colony::Sim& Sim);
	/** The same for a look text from the character screen ("You"). Smoke tests never write it. */
	void RememberCharacterText(const FString& CreatorText);
	/** The protagonist's creator look text in this sim, empty if none. */
	FString FindCharacterText(const colony::Sim& Sim);
}
