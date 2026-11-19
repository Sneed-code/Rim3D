#pragma once

#include "CoreMinimal.h"

// Fight faces (sneed, docs/design/focus.md "Faces"): a pain face for a moment on every hit and an angry face while a
// pawn fights in melee. They are made of MakeHuman's expression units, which the creator bases carry as morphs
// `Face_<Unit>` (tools/makehuman/creator.json, docs/design/makehuman.md "Facial expressions"); the weights are the
// creator's own "In pain" and "Angry" expressions (SmokeTestCreator.cpp), so the creator's Face tab shows what a pawn
// will look like. A body without those morphs (a MetaHuman) ignores them.

struct FPawnExpressionUnit
{
	const TCHAR* Morph;
	float Weight;
};

/** Pain: eyes squeezed, brows drawn down and in with the inner ends raised, teeth clenched behind lips pulled back and a
 *  little apart, the neck tight. */
inline const FPawnExpressionUnit PawnExpressionPain[] = {
	{TEXT("Face_EyeLeftClosure"), 0.65f}, {TEXT("Face_EyeRightClosure"), 0.65f},
	{TEXT("Face_EyebrowsLeftDown"), 0.55f}, {TEXT("Face_EyebrowsRightDown"), 0.55f},
	{TEXT("Face_EyebrowsLeftInnerUp"), 0.40f}, {TEXT("Face_EyebrowsRightInnerUp"), 0.40f},
	{TEXT("Face_NoseCompression"), 0.35f}, {TEXT("Face_MouthRetraction"), 0.55f}, {TEXT("Face_MouthDepression"), 0.30f},
	{TEXT("Face_MouthOpen"), 0.12f}, {TEXT("Face_NeckPlatysma"), 0.45f},
	{TEXT("Face_NoseLeftElevation"), 0.25f}, {TEXT("Face_NoseRightElevation"), 0.25f}};

/** Angry: brows hard down, nostrils up, lips pressed and pulled back, eyes narrowed, the neck tight. */
inline const FPawnExpressionUnit PawnExpressionAngry[] = {
	{TEXT("Face_EyebrowsLeftDown"), 1.00f}, {TEXT("Face_EyebrowsRightDown"), 1.00f},
	{TEXT("Face_NoseLeftElevation"), 0.50f}, {TEXT("Face_NoseRightElevation"), 0.50f},
	{TEXT("Face_MouthCompression"), 0.50f}, {TEXT("Face_MouthRetraction"), 0.30f},
	{TEXT("Face_EyeLeftSlit"), 0.30f}, {TEXT("Face_EyeRightSlit"), 0.30f}, {TEXT("Face_NeckPlatysma"), 0.20f}};

/** How long the pain face holds after a hit (game seconds), and how fast the faces come and go. */
constexpr float PawnPainFaceSeconds = 0.6f;
constexpr float PawnPainFaceIn = 0.06f;
constexpr float PawnPainFaceOut = 0.2f;
constexpr float PawnAngryFaceIn = 0.3f;
constexpr float PawnAngryFaceOut = 0.8f;
/** The angry face stays this long after the last swing, block or lock-on, so it does not flicker between blows. */
constexpr float PawnAngryHoldSeconds = 4.0f;
