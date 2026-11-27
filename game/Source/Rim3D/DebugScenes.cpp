#include "DebugScenes.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogDebugScenes, Log, All);

namespace
{
	// Play scenes run for 4 hours (their old .bat files' -SmokeTimeout); watch scenes get half an hour, far more than any
	// of them takes, so a slow PC never fails one for time.
	const DebugScenes::FScene DebugSceneList[] = {
		{TEXT("gunrange"), TEXT("Gun range"), TEXT("Play"),
			TEXT("Every gun and melee weapon in third person, random creator women as targets. 1-0 or the wheel pick a weapon, K new targets, U guards."),
			TEXT("-SmokeTest=gunrange -SmokeTimeout=14700")},
		{TEXT("gunrange_guards"), TEXT("Gun range, unaware guards"), TEXT("Play"),
			TEXT("The gun range with guards who have their backs turned: for stealth and sneaking up."),
			TEXT("-SmokeTest=gunrange -SmokeTimeout=14700 -GunRangeGuards")},
		{TEXT("creator"), TEXT("Character creator"), TEXT("Play"),
			TEXT("A woman, a man and an imp on the creator bodies with every slider, hair and clothes."),
			TEXT("-SmokeTest=creator -SmokeTimeout=14700")},
		{TEXT("duel"), TEXT("Sword and shield duel"), TEXT("Play"),
			TEXT("Elden Ring style melee: you and a hostile pawn, each with a gladius and a shield. Click attack, hold left click heavy (keep holding to charge), right mouse guard, T roll, light attack on a reeling or turned-away foe is a critical hit. Esc menu."),
			TEXT("-SmokeTest=duel -SmokeTimeout=14700")},
		{TEXT("duel_watch"), TEXT("Sword and shield duel (watch)"), TEXT("Watch"),
			TEXT("The same duel with both sides fighting on their own, from the side."), TEXT("-SmokeTest=duel -DuelWatch -SmokeTimeout=1800")},
		{TEXT("showcase"), TEXT("Body showcase"), TEXT("Play"),
			TEXT("Walk, run and jump as the ponytail woman, with the long-haired woman standing by."),
			TEXT("-SmokeTest=showcase -SmokeTimeout=14700")},
		{TEXT("rainforest"), TEXT("Rainforest colony (skip menus)"), TEXT("Play"),
			TEXT("Straight into the Crash Landed start on a rainforest tile of world 42, from above."),
			TEXT("-SimWorld -SimScenario=CrashlandedRainforest -SimOriginX=300000 -Overseer")},
		{TEXT("finishers"), TEXT("Melee finishers"), TEXT("Watch"),
			TEXT("A finisher with each melee weapon on a downed enemy."), TEXT("-SmokeTest=finishers -SmokeTimeout=1800")},
		{TEXT("melee"), TEXT("Melee"), TEXT("Watch"),
			TEXT("Carried and holstered weapons, swings, blocks and dodges."), TEXT("-SmokeTest=melee -SmokeTimeout=1800")},
		{TEXT("gore"), TEXT("Blood and gore"), TEXT("Watch"),
			TEXT("Hits, blood spray, skin stains, dismemberment and a blast on a creator body."), TEXT("-SmokeTest=gore -SmokeTimeout=1800")},
		{TEXT("hurt"), TEXT("Wounds and dirt"), TEXT("Watch"),
			TEXT("Hit wobbles, limping, patching wounds, crawling, and dirt on the body."), TEXT("-SmokeTest=hurt -SmokeTimeout=1800")},
		{TEXT("fights"), TEXT("Gunfight"), TEXT("Watch"),
			TEXT("Shots, tracers, impacts, hit reactions and ragdolls."), TEXT("-SmokeTest=fights -SmokeTimeout=1800")},
		{TEXT("handson"), TEXT("Hands-on work"), TEXT("Watch"),
			TEXT("Mining, chopping (a tree falls), hauling and building by hand."), TEXT("-SmokeTest=handson -SmokeTimeout=1800")},
		{TEXT("sky"), TEXT("Sky and weather"), TEXT("Watch"),
			TEXT("Sun and moon, rain, snow, fog and thunderstorms."), TEXT("-SmokeTest=sky -SmokeTimeout=1800")},
		{TEXT("fire"), TEXT("Fire"), TEXT("Watch"),
			TEXT("Lamps, fires, burning people, burnt ground and ash."), TEXT("-SmokeTest=fire -SmokeTimeout=1800")},
		{TEXT("water"), TEXT("Water and swimming"), TEXT("Watch"),
			TEXT("Rivers, wading, swimming and wet bodies."), TEXT("-SmokeTest=water -SmokeTimeout=1800")},
		{TEXT("animals"), TEXT("Animals"), TEXT("Watch"),
			TEXT("Every animal model, its poses and sizes."), TEXT("-SmokeTest=animals -SmokeTimeout=1800")},
		{TEXT("clothes"), TEXT("Clothes"), TEXT("Watch"),
			TEXT("Worn clothes, torn looks, hats and stripping."), TEXT("-SmokeTest=clothes -SmokeTimeout=1800")},
		{TEXT("shuttle"), TEXT("Shuttle landing"), TEXT("Watch"),
			TEXT("A visitors' shuttle flying in and landing."), TEXT("-SmokeTest=shuttle -SmokeTimeout=1800")},
		{TEXT("sound"), TEXT("Sounds"), TEXT("Watch"),
			TEXT("Every sound cue, footsteps, weather and music."), TEXT("-SmokeTest=sound -SmokeTimeout=1800")},
	};

	/** Splits a command line into its arguments, keeping quoted parts ("C:\My Folder\Rim3D.uproject") together. */
	TArray<FString> DebugSceneTokens(const FString& Line)
	{
		TArray<FString> Out;
		FString Current;
		bool bQuoted = false;
		for (const TCHAR C : Line)
		{
			if (C == TEXT('"'))
			{
				bQuoted = !bQuoted;
				Current.AppendChar(C);
			}
			else if (FChar::IsWhitespace(C) && !bQuoted)
			{
				if (!Current.IsEmpty())
				{
					Out.Add(MoveTemp(Current));
					Current.Reset();
				}
			}
			else
			{
				Current.AppendChar(C);
			}
		}
		if (!Current.IsEmpty())
		{
			Out.Add(MoveTemp(Current));
		}
		return Out;
	}

	/** "-SmokeTest=gunrange" -> "smoketest"; "-Overseer" -> "overseer"; "/Game/Levels/DefaultLevel" -> "". */
	FString DebugSceneKey(const FString& Token)
	{
		if (!Token.StartsWith(TEXT("-")))
		{
			return FString();
		}
		FString Key = Token.Mid(1);
		int32 Equals = INDEX_NONE;
		if (Key.FindChar(TEXT('='), Equals))
		{
			Key.LeftInline(Equals);
		}
		return Key.ToLower();
	}

	bool DebugSceneStart(const FString& SceneArgs, const TCHAR* What)
	{
		const FString Base = DebugScenes::StripSceneArgs(FCommandLine::GetOriginal());
		const FString Args = SceneArgs.IsEmpty() ? Base : Base + TEXT(" ") + SceneArgs;
		const FString Exe = FPlatformProcess::ExecutablePath();
		UE_LOG(LogDebugScenes, Display, TEXT("Starting %s: %s %s"), What, *Exe, *Args);
		FProcHandle Proc = FPlatformProcess::CreateProc(*Exe, *Args, true, false, false, nullptr, 0, nullptr, nullptr);
		if (!Proc.IsValid())
		{
			UE_LOG(LogDebugScenes, Error, TEXT("Could not start %s"), *Exe);
			return false;
		}
		FPlatformProcess::CloseProc(Proc);
		FPlatformMisc::RequestExit(false, TEXT("DebugScenes"));
		return true;
	}

	// The console settings before a scene started in this game: name, value and flags (which hold who set it).
	struct FDebugSceneCvar
	{
		FString Name;
		FString Value;
		EConsoleVariableFlags Flags;
	};
	TArray<FDebugSceneCvar> GDebugSceneCvars;
	bool GDebugSceneSaved = false;
	float GDebugSceneNearClip = 0.0f;
	bool GDebugSceneScreenMessages = true;
	bool GDebugSceneTravelled = false;

	void DebugSceneRemember()
	{
		GDebugSceneCvars.Reset();
		IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(FConsoleObjectVisitor::CreateLambda(
			[](const TCHAR* Name, IConsoleObject* Object)
			{
				IConsoleVariable* Var = Object ? Object->AsVariable() : nullptr;
				if (Var && !Var->TestFlags(ECVF_ReadOnly))
				{
					GDebugSceneCvars.Add({Name, Var->GetString(), Var->GetFlags()});
				}
			}),
			TEXT(""));
		GDebugSceneNearClip = GNearClippingPlane;
		GDebugSceneScreenMessages = GAreScreenMessagesEnabled;
		GDebugSceneSaved = true;
	}

	void DebugSceneRestore()
	{
		if (!GDebugSceneSaved)
		{
			return;
		}
		// The scalability groups first (each sets many others), then everything else back to its own value. Set at the
		// console's priority (the scene's console commands used it), then given back its own flags, so later settings
		// changes (the Settings window) still apply as before.
		int32 Restored = 0;
		for (const bool bGroups : {true, false})
		{
			for (const FDebugSceneCvar& Saved : GDebugSceneCvars)
			{
				if (Saved.Name.StartsWith(TEXT("sg.")) != bGroups)
				{
					continue;
				}
				IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(*Saved.Name);
				if (Var && Var->GetString() != Saved.Value)
				{
					Var->Set(*Saved.Value, ECVF_SetByConsole);
					Var->SetFlags(Saved.Flags);
					++Restored;
				}
			}
		}
		GNearClippingPlane = GDebugSceneNearClip;
		GAreScreenMessagesEnabled = GDebugSceneScreenMessages;
		UE_LOG(LogDebugScenes, Display, TEXT("Back from the scene: %d console settings put back"), Restored);
		GDebugSceneCvars.Reset();
		GDebugSceneSaved = false;
	}

	/** Loads the level again with this command line (every world system starts fresh and reads it). */
	bool DebugSceneTravel(UWorld* World, const FString& SceneArgs, const TCHAR* What, bool bIntoScene)
	{
		if (!World)
		{
			return DebugSceneStart(SceneArgs, What);
		}
		const FString Map = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
		const FString Base = DebugScenes::StripSceneArgs(FCommandLine::Get());
		const FString Line = SceneArgs.IsEmpty() ? Base : Base + TEXT(" ") + SceneArgs;
		if (bIntoScene)
		{
			if (!GDebugSceneSaved)
			{
				DebugSceneRemember();
			}
		}
		else
		{
			DebugSceneRestore();
		}
		UE_LOG(LogDebugScenes, Display, TEXT("Loading %s in this game: map %s, command line %s"), What, *Map, *Line);
		FCommandLine::Set(*Line);
		GDebugSceneTravelled = true;
		UGameplayStatics::OpenLevel(World, FName(*Map), true);
		return true;
	}
}

TConstArrayView<DebugScenes::FScene> DebugScenes::All()
{
	return MakeArrayView(DebugSceneList);
}

FString DebugScenes::StripSceneArgs(const FString& CommandLine)
{
	TSet<FString> Drop = {TEXT("smoketest"), TEXT("smoketimeout"), TEXT("debugscene"), TEXT("nomenu")};
	for (const FScene& Scene : DebugSceneList)
	{
		for (const FString& Token : DebugSceneTokens(Scene.Args))
		{
			Drop.Add(DebugSceneKey(Token));
		}
	}
	FString Out;
	for (const FString& Token : DebugSceneTokens(CommandLine))
	{
		const FString Key = DebugSceneKey(Token);
		// Every scene's own settings (-GunRangeSeed=7, -CreatorFemale ...) go too, so the title starts clean.
		if (Drop.Contains(Key) || Key.StartsWith(TEXT("gunrange")) || Key.StartsWith(TEXT("creator")) || Key.StartsWith(TEXT("showcase")))
		{
			continue;
		}
		Out += Out.IsEmpty() ? Token : TEXT(" ") + Token;
	}
	return Out;
}

bool DebugScenes::Launch(UWorld* World, const FScene& Scene)
{
	return DebugSceneTravel(World, FString::Printf(TEXT("%s -DebugScene=%s"), Scene.Args, Scene.Id), Scene.Label, true);
}

bool DebugScenes::ReturnToTitle(UWorld* World)
{
	return DebugSceneTravel(World, FString(), TEXT("the title screen"), false);
}

void DebugScenes::DumpConsoleVariables(const FString& Path)
{
	TArray<FString> Lines;
	IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(FConsoleObjectVisitor::CreateLambda(
		[&Lines](const TCHAR* Name, IConsoleObject* Object)
		{
			if (IConsoleVariable* Var = Object ? Object->AsVariable() : nullptr)
			{
				Lines.Add(FString::Printf(TEXT("%s = %s"), Name, *Var->GetString()));
			}
		}),
		TEXT(""));
	Lines.Sort();
	Lines.Add(FString::Printf(TEXT("(near clip %.2f, screen messages %d)"), GNearClippingPlane, GAreScreenMessagesEnabled ? 1 : 0));
	FFileHelper::SaveStringArrayToFile(Lines, *Path);
	UE_LOG(LogDebugScenes, Display, TEXT("Console variables written to %s (%d)"), *Path, Lines.Num());
}

bool DebugScenes::HasLoadedInGame()
{
	return GDebugSceneTravelled;
}

bool DebugScenes::IsDebugScene()
{
	FString Id;
	return FParse::Value(FCommandLine::Get(), TEXT("DebugScene="), Id) && !Id.IsEmpty();
}
