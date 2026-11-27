#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

class APlayerController;

/**
 * The player's keys (sneed 2026-11-17, docs/design/controls.md): one rebindable key per action, saved in
 * GameUserSettings.ini under [Rim3D.Keys]. Code asks Rim3DKeys::IsDown / WasPressed for an action instead of reading a
 * fixed EKeys value, and the Settings window's Controls page (DrawControlsPage) changes them.
 *
 * New actions go at the end of the list (before Count) and get a row in KeyBindings.cpp.
 */
enum class ERim3DAction : uint8
{
	MoveForward,
	MoveBack,
	MoveLeft,
	MoveRight,
	WalkSlow,
	Sprint,          // a toggle: on until pressed again
	Jump,
	RollDash,        // third person: a dodge roll; first person: a dash with motion blur
	Interact,        // F: pick up, mine, chop, build, sleep in a bed
	PutDown,
	EquipWeapon,
	SwitchWeapon,
	Reload,
	Crouch,
	Prone,
	Grenade,
	Attack,
	AimBlock,        // aim a gun, or block with a melee weapon or shield
	SleepHere,
	Inventory,
	BuildMode,
	ToggleView,      // unused: the game is third person only (kept so saved key numbers don't shift)
	SwitchMode,      // overseer view and back
	TakeOver,        // take control of the colonist you look at
	FocusNextTarget, // lock-on focus: the next nearest enemy
	EndFocus,
	FreeHandToggle,  // free hand mode
	FreeHandGrip,
	FreeHandLook,
	FreeHandReachOut,
	FreeHandReachIn,
	Patch,           // patch a bleeding wound (body health part B)
	Unstuck,         // F12: after 5 s standing still, moved to the nearest clear spot (docs/design/earthworks.md)
	NextGrenade,     // R: the next kind of grenade on the belt (frag, molotov...) for G
	HeavyAttack,     // held with Attack: a heavy attack (Elden Ring style melee, souls-combat.md); Left Shift like sprint
	Count
};

namespace Rim3DKeys
{
	/** The key bound to the action now (its default until changed). */
	RIM3D_API FKey Get(ERim3DAction Action);
	RIM3D_API FKey GetDefault(ERim3DAction Action);
	/** Binds and saves. */
	RIM3D_API void Set(ERim3DAction Action, const FKey& Key);
	RIM3D_API void ResetAll();
	RIM3D_API FString GetDisplayName(ERim3DAction Action);
	/** "F", "Left Shift", "Right Mouse Button": the key's name as the player sees it. */
	RIM3D_API FString GetKeyLabel(ERim3DAction Action);

	RIM3D_API bool IsDown(const APlayerController* PC, ERim3DAction Action);
	RIM3D_API bool WasPressed(const APlayerController* PC, ERim3DAction Action);
	RIM3D_API bool WasReleased(const APlayerController* PC, ERim3DAction Action);

	/** Turns the body's keys off (IsDown and WasPressed say no for every action but BuildMode) while the build screen is
	 *  up over the walking view (UProtagonistSubsystem::OpenBuildUi): the mouse places walls and nobody walks or punches. */
	RIM3D_API void SetGameKeysBlocked(bool bBlocked);
	RIM3D_API bool AreGameKeysBlocked();

	/** Lock-on focus on or off (the focus thread's setting; saved, on by default). */
	RIM3D_API bool IsLockOnFocusEnabled();
	RIM3D_API void SetLockOnFocusEnabled(bool bEnabled);

	/** Draws the rebind list with ImGui inside a window someone else began. Click a key, then press the new one (Esc cancels). */
	RIM3D_API void DrawControlsPage(float Scale);
	/** True while the page waits for a new key: game code may ignore that press. */
	RIM3D_API bool IsCapturing();
}
