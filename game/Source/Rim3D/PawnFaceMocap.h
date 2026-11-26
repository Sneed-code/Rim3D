#pragma once

#include "CoreMinimal.h"

/**
 * Live faces from facial motion capture (sneed 2026-11-26: "add all the motion capture facial expressions i want
 * realism"; docs/design/faces-mocap.md). Cosmetic, Unreal side only.
 *
 * The clips are real actors' faces captured as Apple ARKit's 52 blendshape curves (the BEAT dataset: speakers telling
 * stories in eight emotions, recorded with an iPhone's face tracking), cut and packed by tools/face_mocap.py into
 * assets/face_mocap/clips.json. assets/face_mocap/arkit_map.json says how much of each MakeHuman morph on the creator
 * bases (the 34 expression units `Face_<Unit>`, left and right halves of the mouth units, cheek puff) each ARKit curve
 * drives. A person's face plays clips of the emotion the sim gives them (mood, talking, a mental break, fighting, pain,
 * sleep, death) one after another, eased between, with blinks, glances and every small movement the actor made. While
 * not talking, the mouth curves are turned down (the actors were talking) so a quiet face only breathes and blinks.
 */
class FPawnFaceMocapLibrary
{
public:
	static constexpr int32 Channels = 52;

	struct FClip
	{
		FString Name;
		FName Emotion;
		int32 Frames = 0;
		TArray<uint8> Data;  // Frames x Channels, 0..255 = 0..1
	};
	struct FDrive
	{
		int32 Morph = 0;  // into Morphs
		float Weight = 0.0f;
	};

	/** The library, read once from assets/face_mocap (empty if the files are missing). */
	static const FPawnFaceMocapLibrary& Get();

	bool IsLoaded() const { return Clips.Num() > 0 && Morphs.Num() > 0; }
	float Fps = 30.0f;
	TArray<FString> ChannelNames;
	TArray<FClip> Clips;
	TMap<FName, TArray<int32>> ByEmotion;
	TArray<FName> Morphs;                // every morph the curves drive
	TArray<FDrive> Drives[Channels];     // per ARKit curve
	float Quiet[Channels];               // the curve's share while not talking (1 for the eyes and brows)
	int32 ChannelIndex(const TCHAR* Name) const { return ChannelNames.IndexOfByKey(FString(Name)); }

private:
	void Load();
};

/** One face's player: which clip of which emotion, where in it, the eased curves. */
struct FPawnFacePlayer
{
	FName Emotion;
	bool bTalking = false;
	int32 Clip = INDEX_NONE;
	float Time = 0.0f;
	float SinceChange = 99.0f;
	float Curves[FPawnFaceMocapLibrary::Channels] = {};
	bool bStarted = false;

	/** Moves on by Delta seconds playing Emotion (a clip of it; neutral if there is none) and eases the curves. */
	void Tick(float Delta, FName WantEmotion, bool bWantTalking, FRandomStream& Random);
	/** The morph weights for the curves now, times Amount, added into Out (by morph name). */
	void AddMorphs(TMap<FName, float>& Out, float Amount) const;
};
