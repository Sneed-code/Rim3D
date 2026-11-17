#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "MotionClipsCommandlet.generated.h"

/**
 * Measures how far each fight clip really carries the body and writes it as game data (docs/design/body-motion.md):
 * data/defs/MotionClips/MotionClips.json, one def per clip listed in assets/motion_clips.json. The sim moves bodies
 * along these paths through walls and other bodies, so a roll, a stagger or a lunge can never snap back.
 *
 * For each clip (an animation under /Game/Rim3D/Generated/Animations, retargeted onto the game's mannequin) it reads
 * the pelvis on the ground 30 times a second: metres forward and to the right of where it began, in the frame the
 * body faced, for the 1.80 m man. An attack's `hitSeconds` is the list's own, or else the moment the weapon hand is
 * fastest. Clips whose animation is missing are reported and left out.
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=MotionClips -unattended -nosplash -nullrhi
 *   -List    also logs every animation under /Game/Rim3D/Generated/Animations with its length and travel
 *   -Trace=A_Name1+A_Name2   also logs every sample of those animations (travel, turn, each hand's speed)
 */
UCLASS()
class UMotionClipsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMotionClipsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
