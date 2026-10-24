#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "Stage0Test.generated.h"

class ACameraActor;
class ACharacter;
class FJsonValue;

/**
 * Stage 0 tests that need the graphics card. Does nothing unless the game is
 * started with -Stage0Test=crowd or -Stage0Test=terrain. Each test places its
 * own camera, measures frame times, takes screenshots, writes
 * Saved/Stage0/<test>.json and quits.
 */
UCLASS()
class RIM3D_API UStage0TestSubsystem : public UTickableWorldSubsystem
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

	void BuildCrowdPhases();
	void BuildTerrainPhases();
	void StartPhase(int32 Index);
	void FinishPhase();
	void WriteResultsAndQuit();

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

	UPROPERTY()
	TObjectPtr<ACameraActor> Camera;

	UPROPERTY()
	TArray<TObjectPtr<AActor>> Spawned;

	// Runners circle this point; each has its own radius and direction.
	FVector CrowdCentre = FVector::ZeroVector;
	TArray<float> RunnerRadius;
};
