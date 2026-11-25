#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "SeeThroughArtCommandlet.generated.h"

/**
 * Makes the overseer's see-through outlines (sneed 2026-11-25: "when in the trees in overseer mode you see the pawns'
 * outlines, and structures too") into /Game/Rim3D/Generated/Overseer/ (not in Git; art step `see-through-art`):
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=SeeThroughArt -unattended -nosplash -AllowCommandletRendering
 *
 * M_SeeThrough is a post-process material (after tone mapping). People and buildings in the overseer view draw into
 * custom depth with a stencil value (1 colonist, 2 hostile, 3 other people and animals, 4 building, see
 * OverseerSubsystem); wherever such a thing is hidden behind something nearer (a tree's canopy, a roof, a wall), its edge
 * is drawn as a coloured outline and its hidden shape gets a faint tint of the same colour.
 */
UCLASS()
class USeeThroughArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	USeeThroughArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
