#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "SmokeTest.generated.h"

class ACameraActor;
class ACharacter;
class FJsonValue;
namespace colony { class Map; }

/**
 * Smoke tests that need the graphics card. Does nothing unless the game is
 * started with -SmokeTest=<name>. Each test places its own camera, measures
 * frame times, takes screenshots into Saved/Smoke/<name>/, writes
 * Saved/Smoke/<name>.json and quits with exit code 0 (pass) or 1 (fail).
 *
 * Tests: load (the level opens and the sim runs), crowd, terrain, sim (the bridge: defs, commands,
 * events, snapshots and blending, speed keys, replay file), overseer (the camera, picking, selection
 * and the ImGui panels), pawns (pawn actors, animation detail by distance, actions, MetaHumans),
 * saves (autosave, F5 quicksave, F9 quickload, F8 bug report), protagonist (direct control of a
 * colonist: Tab, first and third person, taking over another colonist, walls, pause, saves), looks
 * (seeded pawn looks on MetaHumans: body shapes, skin colour, height, Emberkin horns), world (the ground,
 * rock, roofs and plants drawn from the sim, rebuilt when rock is mined or a tree felled), buildings
 * (blueprints, frames, finished buildings, doors that slide open, and items drawn from the sim), orders (the architect
 * menu, the marks overlay, draft, move and attack orders and the alarm, driven as the mouse does), panels (the work and
 * schedule tabs, the colonist and bills panels and the message log, driven by their click handlers), fights (the
 * protagonist's gun, muzzle flashes, tracers, impacts, hit reactions, ragdolls, wound decals and the hitbox view),
 * handson (the controlled colonist mines, chops, picks up, delivers and builds by hand, and plans a building with the build ghost),
 * animals (animal actors on their A4 models: poses, scale by life stage, animation detail by distance),
 * sky (sun and moon from the sim's clock, the season's sun height, rain, snow in the air and on the ground, wet ground, fog,
 * thunderstorm flashes and the date readout).
 * melee (carried and holstered weapons, the weapon switch key, melee swings timed to the sim, blocks, dodges and hit-stop),
 * clothes (worn apparel drawn on MetaHumans over a skin layer: stuff tint, torn look, hats and hair, taking off and stripping).
 * Optional: -SmokeTimeout=<seconds> (default 300) fails a run that hangs.
 * To add a test: write a Build<Name>Phases() and add a branch in OnWorldBeginPlay.
 * Run it with tools/smoke.ps1.
 */
UCLASS()
class RIM3D_API USmokeTestSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return !TestName.IsEmpty(); }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FPhase
	{
		FString Name;
		TFunction<void()> Setup;
		float WarmupSeconds = 5.0f;
		float MeasureSeconds = 15.0f;
		/** Runs when the phase ends, before the next one starts: checks results, calls Fail. */
		TFunction<void()> Verify;
		/** Runs every frame of the phase. */
		TFunction<void()> OnFrame;
	};

	struct FFrameSample
	{
		float FrameMs;
		float GameMs;
		float RenderMs;
		float GpuMs;
	};

	void BuildLoadPhases();
	void BuildCrowdPhases();
	void BuildTerrainPhases();
	void BuildSimPhases();
	void BuildOverseerPhases();
	void BuildPawnsPhases();
	void BuildSavesPhases();
	void BuildProtagonistPhases();  // SmokeTestProtagonist.cpp
	void BuildLooksPhases();        // SmokeTestLooks.cpp
	void BuildWorldPhases();        // SmokeTestWorld.cpp
	void BuildBuildingsPhases();    // SmokeTestBuildings.cpp
	void BuildOrdersPhases();       // SmokeTestOrders.cpp
	void BuildPanelsPhases();       // SmokeTestPanels.cpp
	void BuildFightsPhases();       // SmokeTestFights.cpp
	void BuildHandsOnPhases();      // SmokeTestHandsOn.cpp
	void BuildAnimalsPhases();      // SmokeTestAnimals.cpp
	void BuildSkyPhases();          // SmokeTestSky.cpp
	void BuildMeleePhases();        // SmokeTestMelee.cpp
	void BuildClothesPhases();      // SmokeTestClothes.cpp
	void BuildFeelPhases();         // SmokeTestFeel.cpp
	void StartPhase(int32 Index);
	void FinishPhase();
	void WriteResultsAndQuit();
	void Fail(const FString& Message);

	void ViewFrom(const FVector& Location, const FRotator& Rotation);
	void ClearSpawned();
	ACharacter* SpawnRunner(const FVector& Location, bool bMetaHuman, int32 Seed);
	void ApplySeededLook(AActor* MetaHuman, int32 Seed, FString& OutDescription);
	void BuildTerrain();
	void BuildTerrainMesh(const colony::Map& Map, double BuildStart);

	FString TestName;
	TArray<FPhase> Phases;
	int32 PhaseIndex = -1;
	float PhaseTime = 0.0f;
	bool bScreenshotTaken = false;
	TArray<FFrameSample> Samples;
	TArray<TSharedPtr<FJsonValue>> Results;
	TArray<FString> Notes;
	TArray<FString> Errors;
	double StartSeconds = 0.0;
	double TimeoutSeconds = 300.0;
	bool bFinished = false;

	UPROPERTY()
	TObjectPtr<ACameraActor> Camera;

	UPROPERTY()
	TArray<TObjectPtr<AActor>> Spawned;

	// Runners circle this point; each has its own radius and direction.
	FVector CrowdCentre = FVector::ZeroVector;
	TArray<float> RunnerRadius;
};
