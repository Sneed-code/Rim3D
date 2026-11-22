#pragma once

#include "CoreMinimal.h"

// Names shared by the blast drawing (BlastFxSubsystem) and the commandlet that makes its art (Rim3DEditor/BlastArtCommandlet),
// docs/design/blast-draw.md.

namespace BlastFxData
{
	inline const TCHAR* const Folder = TEXT("/Game/Rim3D/Generated/Blast");
	inline const TCHAR* const FireMaterial = TEXT("/Game/Rim3D/Generated/Blast/M_BlastFire.M_BlastFire");
	inline const TCHAR* const SmokeMaterial = TEXT("/Game/Rim3D/Generated/Blast/M_BlastSmoke.M_BlastSmoke");
	inline const TCHAR* const SparkMaterial = TEXT("/Game/Rim3D/Generated/Blast/M_BlastSpark.M_BlastSpark");
	inline const TCHAR* const HazeMaterial = TEXT("/Game/Rim3D/Generated/Blast/M_BlastHaze.M_BlastHaze");
	inline const TCHAR* const ScorchMaterial = TEXT("/Game/Rim3D/Generated/Blast/M_BlastScorch.M_BlastScorch");

	// Per-instance custom data of the sprite materials (M_BlastFire, M_BlastSmoke).
	constexpr int32 DataFrame = 0;    // flipbook position 0 .. 63 (blended between whole frames)
	constexpr int32 DataOpacity = 1;  // 0 .. 1
	constexpr int32 DataTintR = 2;    // smoke colour (linear)
	constexpr int32 DataTintG = 3;
	constexpr int32 DataTintB = 4;
	constexpr int32 DataFireGain = 5;  // how bright the flipbook's fire glows (0 none)
	constexpr int32 DataFlip = 6;      // 0 .. 3: bit 1 mirrors left-right, bit 2 upside down
	constexpr int32 DataGlow = 7;      // orange light from fire below, on the smoke
	constexpr int32 SpriteFloats = 8;

	// M_BlastSpark: 0 brightness, 1 opacity. M_BlastHaze: 0 strength.
	constexpr int32 SparkFloats = 2;
	constexpr int32 HazeFloats = 1;

	constexpr int32 FlipbookFrames = 64;

	// The flipbooks (Rim3DEditor/BlastFlipbooks.h) show the volume's -1.05..1.05 across a frame, where they showed
	// -0.74..0.74 when the sprite sizes were found by eye: a cloud fills 0.74 / 1.05 of what it did, so sprites are drawn this
	// much bigger. Keep it kFill / kOldFill.
	constexpr float SpriteScale = 1.05f / 0.74f;
}
