#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "SmokeTest.generated.h"

class ACameraActor;
class ACharacter;
class FJsonValue;

/**
 * Smoke tests that need the graphics card. Does nothing unless the game is
 * started with -SmokeTest=<name>. Each test places its own camera, measures
 * frame times, takes screenshots into Saved/Smoke/<name>/, writes
 * Saved/Smoke/<name>.json and quits with exit code 0 (pass) or 1 (fail).
 *
 * Tests: load (the level opens and the sim runs), crowd, terrain.
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
	void StartPhase(int32 Index);
	void FinishPhase();
	void WriteResultsAndQuit();
	void Fail(const FString& Message);

	void ViewFrom(const FVector& Location, const FRotator& Rotation);
	void ClearSpawned();
	ACharacter* SpawnRunner(const FVector& Location, bool bMetaHuman, int32 Seed);
	void ApplySeededLook(AActor* MetaHuman, int32 Seed, FString& OutDescription);
	void BuildTerrain();

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
