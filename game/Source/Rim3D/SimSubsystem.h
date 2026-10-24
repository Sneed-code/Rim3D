#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Sim.h"

#include <memory>

#include "SimSubsystem.generated.h"

/**
 * The bridge between Unreal and the colony sim. Owns the sim for a game world
 * and steps it at a fixed rate: 30 steps per real second at 1x, one game
 * second per step.
 */
UCLASS()
class RIM3D_API USimSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** 0 pauses; 1, 2 and 3 are the normal speeds. */
	UFUNCTION(BlueprintCallable, Category = "Colony")
	void SetSpeed(int32 NewSpeed);

	UFUNCTION(BlueprintPure, Category = "Colony")
	int64 GetGameSeconds() const;

	const colony::Sim* GetSim() const { return Sim.get(); }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	std::unique_ptr<colony::Sim> Sim;
	double StepAccumulator = 0.0;
	int32 Speed = 1;
	int64 LastLoggedHour = -1;
};
