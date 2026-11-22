#pragma once

#include "CoreMinimal.h"

/**
 * The title screen's Debug page (docs/design/new-game.md "Debug scenes"): the test scenes that used to need their own
 * launch file or their own copy of the game (the gun range, the character creator, the showcase, the watchable smoke
 * tests), started from inside the one game in C:\Dev\Rim3D, so every merge reaches them.
 *
 * A scene is started by launching a fresh copy of the game with the scene's arguments (exactly what its old .bat file
 * passed) and closing this one: each scene sets its own graphics settings, console variables and sim, and a fresh start
 * is the only way to get it exactly as tested. -DebugScene=<id> marks a copy started this way: F10 there, or the scene
 * ending, starts the title screen again instead of quitting.
 */
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

	/** Starts the scene in a fresh copy of the game and closes this one. False if the copy could not be started. */
	bool Launch(const FScene& Scene);

	/** Starts the title screen in a fresh copy of the game and closes this one. */
	bool ReturnToTitle();

	/** This copy was started from the Debug page (-DebugScene= on its command line). */
	bool IsDebugScene();

	/**
	 * The command line this copy was started with, without any scene's arguments: the project, map and window settings
	 * that every copy keeps. Public for the cloud check of the argument stripping.
	 */
	FString StripSceneArgs(const FString& CommandLine);
}
