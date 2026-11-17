#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "ShuttleArtCommandlet.generated.h"

/**
 * Makes the shuttle's effect materials (docs/design/shuttle-draw.md) into /Game/Rim3D/Generated/Vehicles (not in Git):
 *
 * - M_JetFlame: unlit, additive, two sided, for instanced meshes. Its colour is the mesh's vertex colour times per-instance
 *   custom data 0 (how hard the jet burns) times Brightness, soft at the edges seen side on.
 * - M_JetDust: lit, translucent, for instanced meshes. Vertex colour, opacity from per-instance custom data 0, soft edges.
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=ShuttleArt -unattended -nosplash
 *
 * The shuttle model itself comes in with the GunArt commandlet (-List=assets/shuttle.json); the downwash that bends the
 * grass is MPC_Downwash, made by the FoliageMaterials commandlet.
 */
UCLASS()
class UShuttleArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UShuttleArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
