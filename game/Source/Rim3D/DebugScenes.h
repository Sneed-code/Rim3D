#pragma once

#include "CoreMinimal.h"

/**
 * The title screen's Debug page (docs/design/new-game.md "Debug scenes"): the test scenes that used to need their own
 * launch file or their own copy of the game (the gun range, the character creator, the showcase, the watchable smoke
 * tests), started from inside the one game in C:\Dev\Rim3D, so every merge reaches them.
 *
 * A scene loads inside the running game (sneed 2026-11-22: "when i load gun range it closes the title and then
 * reloads"): the scene's arguments (exactly what its old .bat file passed) go on this game's command line and the level
 * loads again, so every world system starts fresh and reads them, as in a fresh copy. The console settings a scene
 * changes (graphics, sky hour, overseer UI and so on) are remembered when it starts and put back when it ends.
 * -DebugScene=<id> marks a scene: F10 there, or the scene ending, loads the title screen again instead of quitting.
 * Without a world (never in the game) it falls back to starting a fresh copy of the game.
 */
class UWorld;

namespace DebugScenes
{
	struct FScene
	{
		const TCHAR* Id;
		const TCHAR* Label;
		/** "Play" scenes run until closed; "Watch" scenes are scripted smoke tests that end by themselves. */
		const TCHAR* Group;
		const TCHAR* Tip;
		/** The game arguments the scene adds (its old .bat file's). */
		const TCHAR* Args;
	};

	/** Every scene, in the page's order. */
	TConstArrayView<FScene> All();

	/** Loads the scene in this game (the level loads again with the scene's arguments). False if it could not start. */
	bool Launch(UWorld* World, const FScene& Scene);

	/** Loads the title screen again in this game, with the console settings as they were before the scene. */
	bool ReturnToTitle(UWorld* World);

	/** This copy was started from the Debug page (-DebugScene= on its command line). */
	bool IsDebugScene();

	/**
	 * The command line this copy was started with, without any scene's arguments: the project, map and window settings
	 * that every copy keeps. Public for the cloud check of the argument stripping.
	 */
	FString StripSceneArgs(const FString& CommandLine);
}
