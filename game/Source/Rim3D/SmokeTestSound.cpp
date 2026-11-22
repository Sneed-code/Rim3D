// Smoke test `sound` (task V20, V21): the cues and their files, the voice budget, occlusion by walls, footsteps of a
// walking colonist, a campfire's loop, the ambience bed and the weather loop, music that turns tense with the alarm and
// the mixer saved in the settings. Runs with -nosound like the rest, so it checks what the sound
// system decides and the samples a voice makes, not what comes out of the speakers.
#include "SmokeTest.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

#include "SimSubsystem.h"
#include "SoundSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

namespace
{
	const FVector SoundSmokeOrigin(-1700000.0, 0.0, 0.0);

	struct FSoundSmokeState
	{
		colony::CellPos Middle = colony::CellPos{40, 40, 0};
		colony::Entity Walker = colony::kNoEntity;
		int32 MaxVoices = 0;
		int32 StepsHeard = 0;
		bool bCampfireLoop = false;
		bool bRainVoiced = false;
		float WalkTime = 0.0f;
		int32 WalkLeg = 0;
		float OldMusic = 0.6f;
		float VoiceTime = 0.0f;
		int32 VoiceStage = 0;
		FString VoiceAfterScream;
	};

	int32 SoundSmokeSteps(const USoundSubsystem& Sound)
	{
		int32 N = 0;
		for (const TCHAR* Cue : {TEXT("Step_Grass"), TEXT("Step_Dirt"), TEXT("Step_Sand"), TEXT("Step_Stone"), TEXT("Step_Wood")})
		{
			N += Sound.GetPlayCount(Cue);
		}
		return N;
	}

	bool SoundSmokeHas(const USoundSubsystem& Sound, const TCHAR* Cue, bool bNeedVoice)
	{
		TArray<FSoundVoiceInfo> Voices;
		Sound.GetVoices(Voices);
		for (const FSoundVoiceInfo& Info : Voices)
		{
			if (Info.Cue == Cue && (!bNeedVoice || Info.bHasVoice))
			{
				return true;
			}
		}
		return false;
	}
}

void USmokeTestSubsystem::BuildSoundPhases()
{
	const TSharedRef<FSoundSmokeState> State = MakeShared<FSoundSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Sound = [this]() { return GetWorld()->GetSubsystem<USoundSubsystem>(); };
	const auto SimOf = [=]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };
	const auto At = [=](int32 DX, int32 DY) { return colony::CellPos{State->Middle.x + DX, State->Middle.y + DY, 0}; };
	const auto Spawn = [=](const char* Def, int32 DX, int32 DY)
	{
		Bridge()->Submit(colony::SpawnThingCommand{Def, At(DX, DY), 1, colony::Rot4::North});
	};
	const auto Watch = [=]()
	{
		if (USoundSubsystem* S = Sound())
		{
			State->MaxVoices = FMath::Max(State->MaxVoices, S->GetVoicesInUse());
			State->bCampfireLoop |= SoundSmokeHas(*S, TEXT("Fire_Campfire"), false);
			State->bRainVoiced |= SoundSmokeHas(*S, TEXT("Weather_Rain"), true);
		}
	};
	const bool bHaveFiles = FPaths::DirectoryExists(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("ThirdParty/Sounds/cues")));

	// 1. A flat map with a wall to one side, a campfire with fuel and a colonist; the cues and their files load.
	FPhase Setup;
	Setup.Name = TEXT("sound_cues");
	Setup.WarmupSeconds = 3.0f;
	Setup.MeasureSeconds = 1.0f;
	Setup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Sound())
		{
			Fail(TEXT("USimSubsystem or USoundSubsystem did not start"));
			return;
		}
		FSimStartOptions Options;
		Options.Seed = 7;
		Options.MapSize = 80;
		Options.bFlatMap = true;
		B->SetWorldOrigin(SoundSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		for (int32 Y = -12; Y <= 12; ++Y)
		{
			Spawn("Wall", 6, Y);
		}
		Spawn("Campfire", -4, 3);
		Spawn("Human", -2, 0);
		B->StepOnce();
		B->StepOnce();
		colony::Sim* Sim = SimOf();
		const colony::Entity Fire = Sim->map().buildingAt(At(-4, 3));
		if (Fire != colony::kNoEntity && Sim->things().registry().all_of<colony::Fuel>(Fire))
		{
			Sim->jobs().fuel().add(Fire, static_cast<int>(Sim->jobs().fuel().specOf(Fire).capacity));
		}
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.lifeStage < 0)
			{
				State->Walker = View.id;
			}
		}
		B->Submit(colony::DraftCommand{State->Walker, true});
		B->StepOnce();
		const FVector Target = B->SimToWorld(State->Middle.x + 0.5f, State->Middle.y + 0.5f, 0);
		ViewFrom(Target + FVector(-600.0, 0.0, 900.0), FRotator(-55.0f, 0.0f, 0.0f));
	};
	Setup.Verify = [=, this]()
	{
		USoundSubsystem* S = Sound();
		if (!S)
		{
			return;
		}
		PhaseNumbers.Add(TEXT("cues"), S->GetCueCount());
		PhaseNumbers.Add(TEXT("cues_with_files"), S->GetCuesWithFiles());
		PhaseNumbers.Add(TEXT("calm_tracks"), S->GetMusicTracks(0));
		PhaseNumbers.Add(TEXT("tense_tracks"), S->GetMusicTracks(1));
		if (S->GetCueCount() < 100)
		{
			Fail(FString::Printf(TEXT("sound_cues: only %d SoundCues defs loaded"), S->GetCueCount()));
		}
		if (bHaveFiles && (S->GetCuesWithFiles() < 40 || S->GetMusicTracks(0) == 0))
		{
			Fail(FString::Printf(TEXT("sound_cues: the cut files are there but %d cues and %d calm tracks loaded"), S->GetCuesWithFiles(), S->GetMusicTracks(0)));
		}
		// A voice makes samples from a file (a rifle shot) and from a placeholder tone (the alarm).
		const int32 Shot = S->RenderTest(TEXT("Shot_Rifle"), 4096);
		const int32 Tone = S->RenderTest(TEXT("Alarm"), 4096);
		PhaseNumbers.Add(TEXT("shot_loud_samples"), Shot);
		PhaseNumbers.Add(TEXT("tone_loud_samples"), Tone);
		if (Shot < 500 || Tone < 500)
		{
			Fail(FString::Printf(TEXT("sound_cues: a voice made %d and %d loud samples of 4096 (shot, tone)"), Shot, Tone));
		}
		if (!S->GetBed().StartsWith(TEXT("Amb_")))
		{
			Fail(FString::Printf(TEXT("sound_cues: the ambience bed is '%s'"), *S->GetBed()));
		}
		if (!State->bCampfireLoop && !SoundSmokeHas(*S, TEXT("Fire_Campfire"), false))
		{
			Fail(TEXT("sound_cues: the campfire with fuel has no crackle loop"));
		}
		PhaseNumbers.Add(TEXT("bed_cue_is_temperate_night"), S->GetBed() == TEXT("Amb_Temperate_Night") ? 1 : 0);
	};
	Setup.OnFrame = Watch;
	Phases.Add(Setup);

	// 2. Eighty shots at once: no more than the budget plays, the rest are dropped.
	FPhase Budget;
	Budget.Name = TEXT("sound_budget");
	Budget.WarmupSeconds = 0.5f;
	Budget.MeasureSeconds = 1.0f;
	Budget.Setup = [=]()
	{
		USoundSubsystem* S = Sound();
		USimSubsystem* B = Bridge();
		if (!S || !B)
		{
			return;
		}
		State->MaxVoices = 0;
		const FVector Ears = S->GetEars();
		for (int32 I = 0; I < 80; ++I)
		{
			S->PlayCue(TEXT("Shot_Rifle"), FVector(Ears.X + (I % 8) * 100.0, Ears.Y + (I / 8) * 100.0, Ears.Z - 300.0));
		}
	};
	Budget.OnFrame = Watch;
	Budget.Verify = [=, this]()
	{
		USoundSubsystem* S = Sound();
		if (!S)
		{
			return;
		}
		PhaseNumbers.Add(TEXT("max_voices"), State->MaxVoices);
		PhaseNumbers.Add(TEXT("dropped"), S->GetDropped());
		if (State->MaxVoices > S->GetVoiceBudget() || State->MaxVoices < S->GetVoiceBudget() - 2)
		{
			Fail(FString::Printf(TEXT("sound_budget: %d voices in use at most, the budget is %d"), State->MaxVoices, S->GetVoiceBudget()));
		}
		if (S->GetDropped() < 20)
		{
			Fail(FString::Printf(TEXT("sound_budget: only %d of 80 shots were dropped"), S->GetDropped()));
		}
	};
	Phases.Add(Budget);

	// 2b. Pawn voices (docs/design/sound.md "Voices"): a hit grunt, a second grunt inside its 5 s cooldown (not played),
	// then a scream 0.2 s later that cuts the first grunt short. The distance curve halves with each doubling.
	FPhase VoicePhase;
	VoicePhase.Name = TEXT("sound_voices");
	VoicePhase.WarmupSeconds = 0.1f;
	VoicePhase.MeasureSeconds = 1.0f;
	const auto QueueVoice = [=](const char* Cue, colony::SoundVoice Kind)
	{
		USoundSubsystem* S = Sound();
		USimSubsystem* B = Bridge();
		if (!S || !B)
		{
			return;
		}
		colony::SoundPlay Play;
		Play.cue = Cue;
		Play.entity = State->Walker;
		Play.voice = Kind;
		Play.hasPos = true;
		Play.x = State->Middle.x - 1.5f;
		Play.y = State->Middle.y + 0.5f;
		S->QueuePlay(Play);
	};
	VoicePhase.Setup = [=]()
	{
		State->VoiceTime = 0.0f;
		State->VoiceStage = 0;
		QueueVoice("Voice_FemHit", colony::SoundVoice::Hit);
	};
	VoicePhase.OnFrame = [=, this]()
	{
		USoundSubsystem* S = Sound();
		if (!S)
		{
			return;
		}
		State->VoiceTime += GetWorld()->GetDeltaSeconds();
		if (State->VoiceStage == 0 && State->VoiceTime > 0.05f)
		{
			State->VoiceStage = 1;
			QueueVoice("Voice_FemHit", colony::SoundVoice::Hit);
		}
		else if (State->VoiceStage == 1 && State->VoiceTime > 0.2f)
		{
			State->VoiceStage = 2;
			QueueVoice("Voice_FemScream", colony::SoundVoice::Scream);
		}
		else if (State->VoiceStage == 2 && State->VoiceTime > 0.3f)
		{
			State->VoiceStage = 3;
			State->VoiceAfterScream = S->GetPawnVoice(State->Walker);
		}
		else if (State->VoiceStage == 3 && State->VoiceTime > 0.4f)
		{
			State->VoiceStage = 4;
			S->StartLovinForTest(State->Walker, colony::kNoEntity);
		}
	};
	VoicePhase.Verify = [=, this]()
	{
		USoundSubsystem* S = Sound();
		if (!S)
		{
			return;
		}
		PhaseNumbers.Add(TEXT("voices_cut"), S->GetVoicesCut());
		PhaseNumbers.Add(TEXT("voices_skipped"), S->GetVoicesSkipped());
		PhaseNumbers.Add(TEXT("femhit_files"), S->GetClipCount(TEXT("Voice_FemHit")));
		PhaseNumbers.Add(TEXT("femscream_files"), S->GetClipCount(TEXT("Voice_FemScream")));
		PhaseNumbers.Add(TEXT("screaming_after_cut"), State->VoiceAfterScream == TEXT("Voice_FemScream") ? 1 : 0);
		PhaseNumbers.Add(TEXT("place"), S->GetEnvironment());
		if (S->GetPlayCount(TEXT("Voice_FemHit")) != 1 || S->GetVoicesSkipped() < 1)
		{
			Fail(FString::Printf(TEXT("sound_voices: %d hit grunts played, %d skipped (want 1 and 1: the second is in the cooldown)"),
				S->GetPlayCount(TEXT("Voice_FemHit")), S->GetVoicesSkipped()));
		}
		if (S->GetVoicesCut() < 1 || State->VoiceAfterScream != TEXT("Voice_FemScream"))
		{
			Fail(FString::Printf(TEXT("sound_voices: the scream cut %d grunts and the pawn's voice is '%s'"), S->GetVoicesCut(), *State->VoiceAfterScream));
		}
		const float Near = USoundSubsystem::DistanceGain(2.0f, 60.0f, 2.0f);
		const float Twice = USoundSubsystem::DistanceGain(2.0f, 60.0f, 4.0f);
		const float Edge = USoundSubsystem::DistanceGain(2.0f, 60.0f, 60.0f);
		if (!FMath::IsNearlyEqual(Near, 1.0f) || !FMath::IsNearlyEqual(Twice, 0.5f, 0.01f) || Edge > 0.0f
			|| USoundSubsystem::AirCutoff(60.0f, 50.0f) >= USoundSubsystem::AirCutoff(60.0f, 5.0f))
		{
			Fail(FString::Printf(TEXT("sound_voices: distance gains %.2f %.2f %.2f"), Near, Twice, Edge));
		}
		if (S->GetClipCount(TEXT("Voice_FemHit")) != 24 || S->GetClipCount(TEXT("Voice_FemScream")) != 4
			|| S->GetClipCount(TEXT("Voice_LovinCouple")) != 9 || S->GetClipCount(TEXT("Voice_LovinFem")) != 6)
		{
			Fail(FString::Printf(TEXT("sound_voices: loaded %d hit grunts, %d screams, %d couple and %d women's lovin sounds from game/Content/Rim3D/Sounds/cues (want 24, 4, 9, 6: the couple cue also plays the women's)"),
				S->GetClipCount(TEXT("Voice_FemHit")), S->GetClipCount(TEXT("Voice_FemScream")), S->GetClipCount(TEXT("Voice_LovinCouple")),
				S->GetClipCount(TEXT("Voice_LovinFem"))));
		}
		PhaseNumbers.Add(TEXT("bird_files"), S->GetClipCount(TEXT("Amb_Birds_Tropical")));
		PhaseNumbers.Add(TEXT("tropical_day_files"), S->GetClipCount(TEXT("Amb_Tropical_Day")));
		// A colonist alone: a woman's lovin sound starts, a man's none (sneed's rule).
		bool bFemale = false;
		if (USimSubsystem* B = Bridge())
		{
			const colony::Snapshot& Snap = B->GetCurrentSnapshot();
			if (const colony::ThingView* View = Snap.find(State->Walker))
			{
				bFemale = View->looks >= 0 && View->looks < static_cast<int32>(Snap.looks.size()) && Snap.looks[static_cast<size_t>(View->looks)].female;
			}
		}
		PhaseNumbers.Add(TEXT("walker_female"), bFemale ? 1 : 0);
		PhaseNumbers.Add(TEXT("lovin_plays"), S->GetLovinPlays());
		if (S->GetLovinPlays() != (bFemale ? 1 : 0))
		{
			Fail(FString::Printf(TEXT("sound_voices: a lone %s's lovin started %d sounds"), bFemale ? TEXT("woman") : TEXT("man"), S->GetLovinPlays()));
		}
	};
	Phases.Add(VoicePhase);

	// 3. Walls: a sound behind the wall is muffled, one on the open side is not. The colonist walks to and fro on the
	// open side and its steps are heard.
	FPhase Walk;
	Walk.Name = TEXT("sound_steps");
	Walk.WarmupSeconds = 1.0f;
	Walk.MeasureSeconds = 9.0f;
	Walk.Setup = [=]()
	{
		USimSubsystem* B = Bridge();
		if (!B)
		{
			return;
		}
		B->SetSpeed(1);
		B->Submit(colony::MoveOrderCommand{State->Walker, At(-8, 0)});
		State->WalkTime = 0.0f;
		State->WalkLeg = 0;
	};
	Walk.OnFrame = [=, this]()
	{
		Watch();
		State->WalkTime += GetWorld()->GetDeltaSeconds();
		if (State->WalkTime > 3.0f)
		{
			State->WalkTime = 0.0f;
			++State->WalkLeg;
			Bridge()->Submit(colony::MoveOrderCommand{State->Walker, At(State->WalkLeg % 2 == 0 ? -8 : 2, State->WalkLeg % 3)});
		}
	};
	Walk.Verify = [=, this]()
	{
		USoundSubsystem* S = Sound();
		USimSubsystem* B = Bridge();
		if (!S || !B)
		{
			return;
		}
		const int32 Steps = SoundSmokeSteps(*S);
		const int32 Behind = S->OcclusionAt(B->SimToWorld(State->Middle.x + 10.5f, State->Middle.y + 0.5f, 0));
		const int32 Open = S->OcclusionAt(B->SimToWorld(State->Middle.x - 6.5f, State->Middle.y + 0.5f, 0));
		PhaseNumbers.Add(TEXT("steps_heard"), Steps);
		PhaseNumbers.Add(TEXT("occlusion_behind_wall"), Behind);
		PhaseNumbers.Add(TEXT("occlusion_open_side"), Open);
		if (Steps < 3)
		{
			Fail(FString::Printf(TEXT("sound_steps: only %d footsteps from a colonist walking for 9 seconds"), Steps));
		}
		if (Behind < 1 || Open != 0)
		{
			Fail(FString::Printf(TEXT("sound_steps: occlusion %d behind the wall and %d on the open side"), Behind, Open));
		}
	};
	Phases.Add(Walk);

	// 4. Rain: its loop plays over the bed. The alarm makes the music tense; a letter gets a stinger.
	FPhase Weather;
	Weather.Name = TEXT("sound_weather_music");
	Weather.WarmupSeconds = 6.0f;
	Weather.MeasureSeconds = 2.0f;
	Weather.Setup = [=]()
	{
		USimSubsystem* B = Bridge();
		if (!B)
		{
			return;
		}
		State->bRainVoiced = false;
		B->Submit(colony::SetWeatherCommand{"Rain", 6.0f});
		B->Submit(colony::SoundAlarmCommand{true});
	};
	Weather.OnFrame = Watch;
	Weather.Verify = [=, this]()
	{
		USoundSubsystem* S = Sound();
		if (!S)
		{
			return;
		}
		PhaseNumbers.Add(TEXT("music_mood"), S->GetMusicMood());
		PhaseNumbers.Add(TEXT("rain_voiced"), State->bRainVoiced ? 1 : 0);
		Notes.Add(FString::Printf(TEXT("sound_weather_music: weather loop %s, music %s"), *S->GetWeatherLoop(), *S->GetMusicTrack()));
		if (S->GetWeatherLoop() != TEXT("Weather_Rain"))
		{
			Fail(FString::Printf(TEXT("sound_weather_music: the weather loop is '%s' in rain"), *S->GetWeatherLoop()));
		}
		if (bHaveFiles && !State->bRainVoiced)
		{
			Fail(TEXT("sound_weather_music: the rain loop never got a voice"));
		}
		if (S->GetMusicMood() != 1)
		{
			Fail(FString::Printf(TEXT("sound_weather_music: music mood %d with the alarm on (1 is tense)"), S->GetMusicMood()));
		}
		if (bHaveFiles && S->GetMusicTracks(1) > 0 && !S->GetMusicTrack().StartsWith(TEXT("tense")))
		{
			Fail(FString::Printf(TEXT("sound_weather_music: the music playing is '%s', not a tense track"), *S->GetMusicTrack()));
		}
	};
	Phases.Add(Weather);

	// 5. The mixer: a slider is saved in the user settings.
	FPhase Mixer;
	Mixer.Name = TEXT("sound_mixer");
	Mixer.WarmupSeconds = 0.5f;
	Mixer.MeasureSeconds = 0.5f;
	Mixer.Setup = [=]()
	{
		if (USoundSubsystem* S = Sound())
		{
			State->OldMusic = S->GetGroupVolume(ESoundMix::Music);
			S->SetGroupVolume(ESoundMix::Music, 0.35f);
		}
	};
	Mixer.Verify = [=, this]()
	{
		USoundSubsystem* S = Sound();
		if (!S)
		{
			return;
		}
		float Saved = -1.0f;
		GConfig->GetFloat(TEXT("/Script/Rim3D.Sound"), TEXT("Music"), Saved, GGameUserSettingsIni);
		S->SetGroupVolume(ESoundMix::Music, State->OldMusic);
		PhaseNumbers.Add(TEXT("saved_music_volume"), Saved);
		if (!FMath::IsNearlyEqual(Saved, 0.35f, 0.001f))
		{
			Fail(FString::Printf(TEXT("sound_mixer: the music slider saved %.3f, not 0.35"), Saved));
		}
	};
	Phases.Add(Mixer);
}
