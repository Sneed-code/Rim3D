#include "DebugScenes.h"

#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogDebugScenes, Log, All);

namespace
{
	// Play scenes run for 4 hours (their old .bat files' -SmokeTimeout); watch scenes get half an hour, far more than any
	// of them takes, so a slow PC never fails one for time.
	const DebugScenes::FScene DebugSceneList[] = {
		{TEXT("gunrange"), TEXT("Gun range"), TEXT("Play"),
			TEXT("First person with every gun and melee weapon, random creator women as targets. 1-0 or the wheel pick a weapon, K new targets, U guards."),
			TEXT("-SmokeTest=gunrange -SmokeTimeout=14700")},
		{TEXT("gunrange_guards"), TEXT("Gun range, unaware guards"), TEXT("Play"),
			TEXT("The gun range with guards who have their backs turned: for stealth and sneaking up."),
			TEXT("-SmokeTest=gunrange -SmokeTimeout=14700 -GunRangeGuards")},
		{TEXT("creator"), TEXT("Character creator"), TEXT("Play"),
			TEXT("A woman, a man and an imp on the creator bodies with every slider, hair and clothes."),
			TEXT("-SmokeTest=creator -SmokeTimeout=14700")},
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

bool DebugScenes::Launch(const FScene& Scene)
{
	return DebugSceneStart(FString::Printf(TEXT("%s -DebugScene=%s"), Scene.Args, Scene.Id), Scene.Label);
}

bool DebugScenes::ReturnToTitle()
{
	return DebugSceneStart(FString(), TEXT("the title screen"));
}

bool DebugScenes::IsDebugScene()
{
	FString Id;
	return FParse::Value(FCommandLine::Get(), TEXT("DebugScene="), Id) && !Id.IsEmpty();
}
