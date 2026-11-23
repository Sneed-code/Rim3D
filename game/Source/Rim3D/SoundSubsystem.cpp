#include "SoundSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/ReverbEffect.h"
#include "Sound/SoundAttenuation.h"

#include "OverseerPawn.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "SkySubsystem.h"
#include "WeaponDrawSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Sound.h"
#include "ColonySim/Things.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
	TAutoConsoleVariable<int32> GSoundEnable(TEXT("Sound.Enable"), 1, TEXT("0 plays no sound at all."));
	TAutoConsoleVariable<float> GSoundMaster(TEXT("Sound.Master"), 1.0f, TEXT("Master volume, 0 to 1."));
	TAutoConsoleVariable<float> GSoundEffects(TEXT("Sound.Effects"), 1.0f, TEXT("Effects volume (shots, hits, work, steps), 0 to 1."));
	TAutoConsoleVariable<float> GSoundAmbience(TEXT("Sound.Ambience"), 0.8f, TEXT("Ambience and weather volume, 0 to 1."));
	TAutoConsoleVariable<float> GSoundVoicesVolume(TEXT("Sound.VoicesVolume"), 1.0f, TEXT("Pawn and animal voices volume, 0 to 1."));
	TAutoConsoleVariable<float> GSoundMusic(TEXT("Sound.Music"), 0.6f, TEXT("Music volume, 0 to 1."));
	TAutoConsoleVariable<int32> GSoundVoices(TEXT("Sound.Voices"), 48, TEXT("How many sounds play at once (the voice budget)."));
	TAutoConsoleVariable<int32> GSoundTones(TEXT("Sound.Tones"), 1,
		TEXT("Placeholder tones for cues with no sound file: 0 none, 1 short sounds only, 2 loops too."));
	TAutoConsoleVariable<int32> GSoundDebug(TEXT("Sound.Debug"), 0,
		TEXT("1 lists the sounds playing on screen; 2 also logs every sound that was dropped or not played, and why."));
	TAutoConsoleVariable<int32> GSoundSameCue(TEXT("Sound.SameCueAtOnce"), 6,
		TEXT("At most this many of one cue play at once (the nearest); more of the same in a battle only add mush."));
	TAutoConsoleVariable<int32> GSoundEcho(TEXT("Sound.Echo"), 1, TEXT("0 turns off the echo of the place (outdoors, room, cave)."));

	/** A scream does not start again over its own first seconds (downed and killed in the same moment scream once). */
	constexpr double SoundScreamRepeatS = 0.3;
	TAutoConsoleVariable<int32> GSoundSpeedOfSound(TEXT("Sound.SpeedOfSound"), 1,
		TEXT("1: a sound is heard as late as sound takes to come that far (343 m/s), so a far shot's bang follows its flash."));
	constexpr double SoundMetresPerSecond = 343.0;
	const FName SoundReverbTag(TEXT("Rim3D.Place"));
	TAutoConsoleVariable<int32> GSoundMusicMuted(TEXT("Sound.MusicMuted"), 0, TEXT("1 mutes the music (Ctrl+M)."));
	TAutoConsoleVariable<int32> GSoundBirds(TEXT("Sound.Birds"), 1, TEXT("0 turns off the bird calls (they are ambience)."));

	/** Each wall or shut door between makes a sound this loud (about -12 dB): a closed room keeps its sounds in. */
	constexpr float SoundWallGain = 0.25f;
	/** Lovin's sound starts this many real seconds in (sooner for a short one). */
	constexpr double SoundLovinLeadS = 10.0;
	/** Birds: share of a day's real length they may sing, and the share of days with any at all. */
	constexpr float SoundBirdShareOfDay = 0.02f;
	constexpr float SoundBirdDayChance = 0.6f;

	const TCHAR* SoundConfigSection = TEXT("/Script/Rim3D.Sound");
	const TCHAR* SoundGroupKeys[] = {TEXT("Effects"), TEXT("Ambience"), TEXT("Voices"), TEXT("Music")};
	constexpr int32 SoundSampleRate = 44100;
	constexpr int32 SoundMusicMoods = 4;  // calm, tense, combat, stinger
	const TCHAR* SoundMoodFolders[SoundMusicMoods] = {TEXT("calm"), TEXT("tense"), TEXT("combat"), TEXT("stinger")};

	// Loop keys: the kind in the top byte, the sim's identity below.
	enum class ESoundKey : uint64
	{
		Building = 1,
		PawnLoop = 2,
		FireCell = 3,
		Everywhere = 4,
		Bed = 5,
		Weather = 6,
		Lovin = 7,
		WeaponLoop = 8,
	};

	uint64 SoundKey(ESoundKey Kind, uint64 Id) { return (static_cast<uint64>(Kind) << 56) | (Id & 0x00FFFFFFFFFFFFFFull); }
	uint32 SoundEntityId(colony::Entity E) { return static_cast<uint32>(E); }
	uint64 SoundCellId(const colony::CellPos& C) { return static_cast<uint64>(C.x & 0xFFFF) | (static_cast<uint64>(C.y & 0xFFFF) << 16) | (static_cast<uint64>(C.level & 0xFF) << 32); }
	uint64 SoundNameId(const FString& Name) { return GetTypeHash(Name); }

	TAutoConsoleVariable<float>& SoundGroupVar(ESoundMix Group)
	{
		switch (Group)
		{
			case ESoundMix::Ambience: return GSoundAmbience;
			case ESoundMix::Voices: return GSoundVoicesVolume;
			case ESoundMix::Music: return GSoundMusic;
			default: return GSoundEffects;
		}
	}

	ESoundMix SoundGroupOf(const FString& Cue)
	{
		if (Cue.StartsWith(TEXT("Amb_")) || Cue.StartsWith(TEXT("Weather_")))
		{
			return ESoundMix::Ambience;
		}
		if (Cue.StartsWith(TEXT("Voice_")) || Cue.StartsWith(TEXT("Pawn_")) || Cue.StartsWith(TEXT("Animal_")) || Cue == TEXT("Work_Chat"))
		{
			return ESoundMix::Voices;
		}
		return ESoundMix::Effects;
	}

	/** A plain RIFF PCM 16-bit WAV, as tools/sound_assets.py writes them. */
	bool SoundReadWav(const TArray<uint8>& Data, FSoundClip& Out)
	{
		if (Data.Num() < 12 || FMemory::Memcmp(Data.GetData(), "RIFF", 4) != 0 || FMemory::Memcmp(Data.GetData() + 8, "WAVE", 4) != 0)
		{
			return false;
		}
		int32 Pos = 12;
		int32 Bits = 0;
		bool bFormat = false;
		while (Pos + 8 <= Data.Num())
		{
			const uint8* Chunk = Data.GetData() + Pos;
			const int32 Size = static_cast<int32>(Chunk[4] | (Chunk[5] << 8) | (Chunk[6] << 16) | (Chunk[7] << 24));
			const int32 Body = Pos + 8;
			if (Size < 0 || Body + Size > Data.Num())
			{
				break;
			}
			if (FMemory::Memcmp(Chunk, "fmt ", 4) == 0 && Size >= 16)
			{
				const uint8* F = Data.GetData() + Body;
				const int32 Format = F[0] | (F[1] << 8);
				Out.Channels = F[2] | (F[3] << 8);
				Out.SampleRate = static_cast<int32>(F[4] | (F[5] << 8) | (F[6] << 16) | (F[7] << 24));
				Bits = F[14] | (F[15] << 8);
				bFormat = (Format == 1 || Format == 0xFFFE) && Bits == 16 && (Out.Channels == 1 || Out.Channels == 2);
			}
			else if (FMemory::Memcmp(Chunk, "data", 4) == 0 && bFormat)
			{
				Out.Samples.SetNumUninitialized(Size / 2);
				FMemory::Memcpy(Out.Samples.GetData(), Data.GetData() + Body, (Size / 2) * 2);
				return Out.Samples.Num() > 0;
			}
			Pos = Body + Size + (Size & 1);
		}
		return false;
	}

	TSharedPtr<const FSoundClip> SoundLoadWav(const FString& Path)
	{
		TArray<uint8> Bytes;
		TSharedPtr<FSoundClip> Clip = MakeShared<FSoundClip>();
		if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent) || !SoundReadWav(Bytes, *Clip))
		{
			return nullptr;
		}
		return Clip;
	}

	/** The def's placeholder `tone`: a sine, square, saw or noise at `hz` for `seconds`, with a short fade at both ends
	 *  (one-shots die away; loops are whole cycles so they wrap cleanly). */
	TSharedPtr<const FSoundClip> SoundMakeTone(const FString& Wave, float Hz, float Seconds, bool bLoop, uint32 Seed)
	{
		TSharedPtr<FSoundClip> Clip = MakeShared<FSoundClip>();
		Hz = FMath::Clamp(Hz, 20.0f, 8000.0f);
		Seconds = FMath::Clamp(Seconds, 0.03f, 4.0f);
		if (bLoop && Wave != TEXT("noise"))
		{
			Seconds = FMath::Max(1.0f, FMath::RoundToFloat(Seconds * Hz)) / Hz;
		}
		const int32 Count = FMath::Max(1, FMath::RoundToInt(Seconds * SoundSampleRate));
		Clip->Samples.SetNumZeroed(Count);
		FRandomStream Noise(static_cast<int32>(Seed));
		const float Smooth = 1.0f - FMath::Exp(-2.0f * PI * Hz / SoundSampleRate);
		float Low = 0.0f;
		for (int32 I = 0; I < Count; ++I)
		{
			const float T = static_cast<float>(I) / SoundSampleRate;
			const float Phase = FMath::Frac(T * Hz);
			float V = 0.0f;
			if (Wave == TEXT("square"))
			{
				V = Phase < 0.5f ? 0.6f : -0.6f;
			}
			else if (Wave == TEXT("saw"))
			{
				V = (2.0f * Phase - 1.0f) * 0.7f;
			}
			else if (Wave == TEXT("noise"))
			{
				Low += Smooth * (Noise.FRandRange(-1.0f, 1.0f) - Low);
				V = FMath::Clamp(Low * 2.5f, -1.0f, 1.0f);
			}
			else
			{
				V = FMath::Sin(2.0f * PI * Phase);
			}
			float Envelope = FMath::Min(1.0f, T / 0.005f);
			if (!bLoop)
			{
				Envelope *= FMath::Exp(-3.0f * T / Seconds) * FMath::Min(1.0f, (Seconds - T) / 0.01f);
			}
			Clip->Samples[I] = static_cast<int16>(FMath::Clamp(V * Envelope * 0.5f, -1.0f, 1.0f) * 32767.0f);
		}
		return Clip;
	}

	/** An echo for the place the ears are in, made in code (no asset): 0 outdoors, 1 a built room, 2 a cave. */
	UReverbEffect* SoundMakeReverb(UObject* Outer, int32 Place)
	{
		UReverbEffect* R = NewObject<UReverbEffect>(Outer);
		if (Place == 2)
		{
			// Rock all round: long, dense, bright tail.
			R->Density = 1.0f; R->Diffusion = 1.0f; R->Gain = 0.32f; R->GainHF = 0.7f;
			R->DecayTime = 2.9f; R->DecayHFRatio = 1.1f;
			R->ReflectionsGain = 0.6f; R->ReflectionsDelay = 0.015f; R->LateGain = 1.4f; R->LateDelay = 0.022f;
		}
		else if (Place == 1)
		{
			// A room of wood or stone: strong early reflections, short tail.
			R->Density = 1.0f; R->Diffusion = 1.0f; R->Gain = 0.32f; R->GainHF = 0.8f;
			R->DecayTime = 0.65f; R->DecayHFRatio = 0.8f;
			R->ReflectionsGain = 0.5f; R->ReflectionsDelay = 0.007f; R->LateGain = 1.2f; R->LateDelay = 0.011f;
		}
		else
		{
			// Outdoors in the forest: no walls, a soft late echo off trees and far ground, dull (leaves eat the highs).
			R->Density = 0.3f; R->Diffusion = 0.6f; R->Gain = 0.25f; R->GainHF = 0.5f;
			R->DecayTime = 1.3f; R->DecayHFRatio = 0.5f;
			R->ReflectionsGain = 0.05f; R->ReflectionsDelay = 0.03f; R->LateGain = 0.8f; R->LateDelay = 0.08f;
		}
		R->AirAbsorptionGainHF = 0.994f;
		return R;
	}

	void SoundApplyFilter(UAudioComponent* Component, float Cutoff)
	{
		Component->SetLowPassFilterEnabled(Cutoff < 19000.0f);
		Component->SetLowPassFilterFrequency(Cutoff);
	}

	FString SoundFolder()
	{
		return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("ThirdParty"), TEXT("Sounds"));
	}

	TArray<FString> SoundWavsIn(const FString& Folder)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(Folder, TEXT("*.wav")), true, false);
		Names.Sort();
		TArray<FString> Paths;
		for (const FString& Name : Names)
		{
			Paths.Add(FPaths::Combine(Folder, Name));
		}
		return Paths;
	}

	colony::CellPos SoundCellAt(const USimSubsystem& Bridge, const FVector& World)
	{
		const FVector Local = (World - Bridge.GetWorldOrigin()) / 100.0;
		int32 Level = 0;
		if (const colony::Sim* Sim = Bridge.GetSim())
		{
			const float Ground = Sim->map().heightAt(static_cast<float>(Local.X), static_cast<float>(Local.Y));
			Level = FMath::Clamp(FMath::FloorToInt((static_cast<float>(Local.Z) - Ground + 0.5f) / colony::Map::kLevelHeightM), 0, 7);
		}
		return colony::CellPos{FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), Level};
	}
}

// ---- The voice's wave -------------------------------------------------------------------------------------------------

void UCueVoiceWave::Setup(TSharedPtr<const FSoundClip> InClip, bool bInLoop, int64 StartSample)
{
	Clip = InClip;
	bLoop = bInLoop;
	NumChannels = Clip ? Clip->Channels : 1;
	SetSampleRate(static_cast<uint32>(Clip ? Clip->SampleRate : SoundSampleRate));
	Duration = INDEFINITELY_LOOPING_DURATION;
	bLooping = false;
	SoundGroup = SOUNDGROUP_Default;
	const int64 Total = Clip ? Clip->Samples.Num() : 0;
	int64 Start = FMath::Max<int64>(0, StartSample) * NumChannels;
	if (Total > 0 && bLoop)
	{
		Start %= Total;
	}
	Cursor = FMath::Min(Start, Total);
}

int32 UCueVoiceWave::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
	OutAudio.Reset();
	OutAudio.AddZeroed(NumSamples * sizeof(int16));
	int16* Out = reinterpret_cast<int16*>(OutAudio.GetData());
	const int64 Total = Clip ? Clip->Samples.Num() : 0;
	int64 At = Cursor.load();
	for (int32 I = 0; I < NumSamples && Total > 0; ++I)
	{
		if (At >= Total)
		{
			if (!bLoop)
			{
				break;
			}
			At = 0;
		}
		Out[I] = Clip->Samples[At++];
	}
	Cursor = At;
	SamplesOut += NumSamples;
	return NumSamples;
}

// ---- Setup ----------------------------------------------------------------------------------------------------------

void USoundSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Source = Collection.InitializeDependency<USimSubsystem>())
	{
		Bridge = Source;
		EventHandle = Source->OnSimEvent.AddUObject(this, &USoundSubsystem::OnSimEvent);
	}
	Collection.InitializeDependency<USkySubsystem>();
	if (GConfig)
	{
		float Value = 0.0f;
		if (GConfig->GetFloat(SoundConfigSection, TEXT("Master"), Value, GGameUserSettingsIni))
		{
			GSoundMaster.AsVariable()->Set(FMath::Clamp(Value, 0.0f, 1.0f), ECVF_SetByCode);
		}
		for (int32 G = 0; G < static_cast<int32>(ESoundMix::Count); ++G)
		{
			if (GConfig->GetFloat(SoundConfigSection, SoundGroupKeys[G], Value, GGameUserSettingsIni))
			{
				SoundGroupVar(static_cast<ESoundMix>(G)).AsVariable()->Set(FMath::Clamp(Value, 0.0f, 1.0f), ECVF_SetByCode);
			}
		}
	}
	if (GConfig)
	{
		bool bMuted = false;
		if (GConfig->GetBool(SoundConfigSection, TEXT("MusicMuted"), bMuted, GGameUserSettingsIni))
		{
			GSoundMusicMuted.AsVariable()->Set(bMuted ? 1 : 0, ECVF_SetByCode);
		}
		if (GConfig->GetBool(SoundConfigSection, TEXT("AllMuted"), bMuted, GGameUserSettingsIni) && bMuted)
		{
			GSoundEnable.AsVariable()->Set(0, ECVF_SetByCode);
		}
	}
	MusicFiles.SetNum(SoundMusicMoods);
}

void USoundSubsystem::Deinitialize()
{
	if (USimSubsystem* Source = Bridge.Get())
	{
		Source->OnSimEvent.Remove(EventHandle);
	}
	for (int32 V = 0; V < Voices.Num(); ++V)
	{
		FreeVoice(V);
	}
	if (MusicComponent)
	{
		MusicComponent->Stop();
	}
	if (Holder)
	{
		Holder->Destroy();
	}
	Holder = nullptr;
	Super::Deinitialize();
}

TStatId USoundSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USoundSubsystem, STATGROUP_Tickables);
}

bool USoundSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void USoundSubsystem::LoadCues()
{
	WorkBeats.Reset();  // they point at cues
	USimSubsystem* Source = Bridge.Get();
	std::shared_ptr<const colony::DefDatabase> Defs = Source ? Source->GetDefs() : nullptr;
	if (!Defs)
	{
		return;
	}
	// The cut files are read once; a new sim only brings new cue numbers.
	TMap<FString, TArray<TSharedPtr<const FSoundClip>>> Files;
	for (TPair<FString, FCue>& Old : Cues)
	{
		Files.Add(Old.Key, Old.Value.Clips);
	}
	const bool bFirst = Cues.Num() == 0;
	Cues.Reset();
	TArray<TPair<FString, FString>> AlsoPlays;
	const FString CueFolder = FPaths::Combine(SoundFolder(), TEXT("cues"));
	const FString OwnFolder = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Rim3D"), TEXT("Sounds"), TEXT("cues"));
	int32 FileCount = 0;
	for (colony::DefId Id : Defs->idsOfKind("SoundCues"))
	{
		const colony::Def& Def = Defs->get(Id);
		FCue Cue;
		Cue.Name = UTF8_TO_TCHAR(Def.defName().c_str());
		Cue.bLoop = Def.flag("loop");
		Cue.Volume = static_cast<float>(Def.number("volume", 0.5));
		Cue.RangeM = static_cast<float>(Def.number("range", 30.0));
		Cue.NearM = static_cast<float>(Def.number("near", FMath::Clamp(Cue.RangeM / 15.0f, 1.0f, 10.0f)));
		Cue.CooldownS = static_cast<float>(Def.number("cooldown", 0.0));
		Cue.Priority = static_cast<int32>(Def.integer("priority", 2));
		Cue.Group = SoundGroupOf(Cue.Name);
		Cue.FarCue = UTF8_TO_TCHAR(Def.text("far").c_str());
		Cue.FarFromM = static_cast<float>(Def.number("farFrom", 0.0));
		Cue.RepeatS = static_cast<float>(Def.number("repeat", 0.0));
		Cue.EveryS = FVector2f(static_cast<float>(Def.number("every.0", 0.0)), static_cast<float>(Def.number("every.1", 0.0)));
		Cue.Tone = SoundMakeTone(UTF8_TO_TCHAR(Def.text("tone.wave", "sine").c_str()), static_cast<float>(Def.number("tone.hz", 440.0)),
			static_cast<float>(Def.number("tone.seconds", 0.2)), Cue.bLoop, GetTypeHash(Cue.Name));
		if (const TArray<TSharedPtr<const FSoundClip>>* Have = Files.Find(Cue.Name); Have && !bFirst)
		{
			Cue.Clips = *Have;
		}
		else
		{
			// The cut downloads (not in Git), then our own sounds (in Git: sneed's voices).
			TArray<FString> Paths = SoundWavsIn(FPaths::Combine(CueFolder, Cue.Name));
			Paths.Append(SoundWavsIn(FPaths::Combine(OwnFolder, Cue.Name)));
			for (const FString& Path : Paths)
			{
				if (TSharedPtr<const FSoundClip> Clip = SoundLoadWav(Path))
				{
					Cue.Clips.Add(Clip);
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("Sound: %s is not a 16-bit PCM WAV, skipped"), *Path);
				}
			}
		}
		FileCount += Cue.Clips.Num();
		const std::string Also = Def.text("alsoPlays");
		if (!Also.empty())
		{
			AlsoPlays.Emplace(Cue.Name, UTF8_TO_TCHAR(Also.c_str()));
		}
		Cues.Add(Cue.Name, MoveTemp(Cue));
	}
	// `alsoPlays`: a cue that picks from another cue's files too (a couple's lovin from the women's ones as well).
	for (const TPair<FString, FString>& Pair : AlsoPlays)
	{
		FCue* To = Cues.Find(Pair.Key);
		const FCue* From = Cues.Find(Pair.Value);
		if (To && From)
		{
			for (const TSharedPtr<const FSoundClip>& Clip : From->Clips)
			{
				To->Clips.AddUnique(Clip);
			}
		}
	}
	if (bFirst)
	{
		UE_LOG(LogTemp, Log, TEXT("Sound: %d cues, %d with sound files (%d files), the rest play tones"), Cues.Num(), GetCuesWithFiles(), FileCount);
		LoadMusic();
	}
}

void USoundSubsystem::LoadMusic()
{
	const FString MusicFolder = FPaths::Combine(SoundFolder(), TEXT("music"));
	for (int32 M = 0; M < SoundMusicMoods; ++M)
	{
		MusicFiles[M] = SoundWavsIn(FPaths::Combine(MusicFolder, SoundMoodFolders[M]));
	}
}

int32 USoundSubsystem::GetCuesWithFiles() const
{
	int32 N = 0;
	for (const TPair<FString, FCue>& Cue : Cues)
	{
		N += Cue.Value.Clips.Num() > 0 ? 1 : 0;
	}
	return N;
}

int32 USoundSubsystem::GetMusicTracks(int32 Mood) const
{
	return MusicFiles.IsValidIndex(Mood) ? MusicFiles[Mood].Num() : 0;
}

int32 USoundSubsystem::GetVoiceBudget() const
{
	return FMath::Clamp(GSoundVoices.GetValueOnGameThread(), 1, 64);
}

float USoundSubsystem::GroupVolume(ESoundMix Group) const
{
	if (Group == ESoundMix::Music && GSoundMusicMuted.GetValueOnGameThread() != 0)
	{
		return 0.0f;
	}
	return FMath::Clamp(SoundGroupVar(Group).GetValueOnGameThread(), 0.0f, 1.0f) * FMath::Clamp(GSoundMaster.GetValueOnGameThread(), 0.0f, 1.0f);
}

float USoundSubsystem::GetGroupVolume(ESoundMix Group) const
{
	return SoundGroupVar(Group).GetValueOnGameThread();
}

void USoundSubsystem::SetGroupVolume(ESoundMix Group, float Volume)
{
	SoundGroupVar(Group).AsVariable()->Set(FMath::Clamp(Volume, 0.0f, 1.0f), ECVF_SetByCode);
	SaveMixer();
}

void USoundSubsystem::SaveMixer() const
{
	if (!GConfig)
	{
		return;
	}
	GConfig->SetFloat(SoundConfigSection, TEXT("Master"), GSoundMaster.GetValueOnGameThread(), GGameUserSettingsIni);
	for (int32 G = 0; G < static_cast<int32>(ESoundMix::Count); ++G)
	{
		GConfig->SetFloat(SoundConfigSection, SoundGroupKeys[G], SoundGroupVar(static_cast<ESoundMix>(G)).GetValueOnGameThread(), GGameUserSettingsIni);
	}
	GConfig->SetBool(SoundConfigSection, TEXT("MusicMuted"), GSoundMusicMuted.GetValueOnGameThread() != 0, GGameUserSettingsIni);
	GConfig->SetBool(SoundConfigSection, TEXT("AllMuted"), GSoundEnable.GetValueOnGameThread() == 0, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

UAudioComponent* USoundSubsystem::MakeComponent()
{
	UAudioComponent* Component = NewObject<UAudioComponent>(Holder);
	Component->bAutoActivate = false;
	Component->bAutoDestroy = false;
	Component->bStopWhenOwnerDestroyed = true;
	Component->RegisterComponent();
	Components.Add(Component);
	return Component;
}

void USoundSubsystem::ResetForSim()
{
	for (int32 V = 0; V < Voices.Num(); ++V)
	{
		FreeVoice(V);
	}
	Sounds.Reset();
	LoopsByKey.Reset();
	PendingPlays.clear();
	PlayCounts.Reset();
	VoicedCounts.Reset();
	StepDistance.Reset();
	WorkBeats.Reset();
	FlyWatch.Reset();
	LastStepPos.Reset();
	DoorOpen.Reset();
	NextCall.Reset();
	NextMoan.Reset();
	ThunderAt.Reset();
	PlaysTotal = 0;
	Dropped = 0;
	MaxVoicesInUse = 0;
	VoicesCut = 0;
	VoicesSkipped = 0;
	PawnVoices.Reset();
	Lovins.Reset();
	LovinPlays = 0;
	BirdDay = -1;
	BirdPlays = 0;
	BirdSecondsPlayed = 0.0f;
	BirdBurstUntil = -1.0;
	LastWeatherLoop.Reset();
	bAlarmOn = false;
	SeenFlashes = -1;
	BedCue.Reset();
	WeatherCue.Reset();
	GroundStep.Reset();
	DefSounds.Reset();
}

// ---- Events and plays ---------------------------------------------------------------------------------------------

colony::DefId USoundSubsystem::DefOfEntity(colony::Entity Thing) const
{
	const USimSubsystem* Source = Bridge.Get();
	if (!Source)
	{
		return colony::DefId{};
	}
	if (const colony::ThingView* View = Source->GetCurrentSnapshot().find(Thing))
	{
		return View->def;
	}
	return colony::DefId{};
}

bool USoundSubsystem::EntityLocation(colony::Entity Thing, FVector& Out) const
{
	const USimSubsystem* Source = Bridge.Get();
	return Source && Source->GetBlendedLocation(Thing, Out);
}

void USoundSubsystem::OnSimEvent(const colony::Event& Event)
{
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetDefs())
	{
		return;
	}
	if (Event.type == colony::EventType::Letter && GSoundEnable.GetValueOnGameThread() != 0)
	{
		bStingerWanted = true;
	}
	if (Event.type == colony::EventType::Lovin)
	{
		if (Event.message == "started")
		{
			// The lovin lasts `amount` game seconds; its sound starts 10 real seconds in, or a fifth of the way in for a
			// short one (at speed), and fades out when it ends.
			const double GameSecondsPerRealSecond = FMath::Max(1.0, 30.0 * FMath::Max(1, Source->GetSpeed()) * 48.0 / FMath::Max(1, Source->GetDayMinutes()));
			const double RealLength = Event.amount / GameSecondsPerRealSecond;
			FLovin L;
			L.A = Event.thing;
			L.B = Event.target;
			L.StartedAt = Now;
			L.PlayAt = Now + FMath::Min(SoundLovinLeadS, 0.2 * RealLength);
			Lovins.Add(L);
		}
		else
		{
			for (int32 I = Lovins.Num() - 1; I >= 0; --I)
			{
				const FLovin& L = Lovins[I];
				if ((L.A == Event.thing && L.B == Event.target) || (L.A == Event.target && L.B == Event.thing) || L.A == Event.thing)
				{
					const int32* Index = L.Key != 0 ? LoopsByKey.Find(L.Key) : nullptr;
					if (Index && Sounds.IsValidIndex(*Index) && !Sounds[*Index].bDead && Sounds[*Index].Key == L.Key)
					{
						CutSound(*Index, Event.message == "done" ? 1.5f : 0.3f);
					}
					if (L.Key != 0)
					{
						LoopsByKey.Remove(L.Key);
					}
					Lovins.RemoveAt(I);
				}
			}
		}
	}
	if (Event.type == colony::EventType::AlarmChanged)
	{
		bAlarmOn = Event.message.rfind("on", 0) == 0;
	}
	const colony::SoundDefOf DefOf = [this](colony::Entity E) { return DefOfEntity(E); };
	const colony::SoundGenderOf Gender = [this](colony::Entity E) { return GenderOf(E); };
	// A bullet in the ground sounds like the ground: dirt, sand, water, a stone or wooden floor.
	const colony::SoundStepAt StepAt = [this, Source](const colony::CellPos& Cell) {
		const colony::Sim* Sim = Source->GetSim();
		return Sim ? std::string(TCHAR_TO_UTF8(*StepCueAt(*Sim, Cell))) : std::string("Step_Dirt");
	};
	// The dead make no vocals (sneed, 2026-11-22): a shot or blow on a corpse gives no grunt.
	const colony::SoundAliveOf Alive = [Source](colony::Entity E) {
		const colony::Sim* Sim = Source->GetSim();
		if (!Sim || !Sim->things().registry().valid(E))
		{
			return true;
		}
		const colony::Health* Health = Sim->things().registry().try_get<colony::Health>(E);
		return !Health || !Health->dead;
	};
	colony::soundForEvent(*Source->GetDefs(), Event, DefOf, PendingPlays, Gender, StepAt, Alive);
}

colony::SoundGender USoundSubsystem::GenderOf(colony::Entity Thing) const
{
	const USimSubsystem* Source = Bridge.Get();
	if (!Source)
	{
		return colony::SoundGender::Unknown;
	}
	// The body as drawn: a looks override (the creator's people in the combat range and the demos) may give a woman to a
	// pawn the sim rolled as a man, and her voice must be a woman's (sneed heard no screams from the range's women).
	if (const USimPawnSubsystem* Pawns = GetWorld() ? GetWorld()->GetSubsystem<USimPawnSubsystem>() : nullptr)
	{
		if (const ASimPawnActor* Actor = Pawns->FindPawn(Thing); Actor && Actor->HasLooks())
		{
			return Actor->GetLooks().bFemale ? colony::SoundGender::Female : colony::SoundGender::Male;
		}
	}
	const colony::Snapshot& Snap = Source->GetCurrentSnapshot();
	if (const colony::ThingView* View = Snap.find(Thing))
	{
		if (View->looks >= 0 && View->looks < static_cast<int32>(Snap.looks.size()))
		{
			return Snap.looks[static_cast<size_t>(View->looks)].female ? colony::SoundGender::Female : colony::SoundGender::Male;
		}
	}
	return colony::SoundGender::Unknown;
}

TSharedPtr<const FSoundClip> USoundSubsystem::PickClip(const FCue& Cue)
{
	if (Cue.Clips.Num() > 0)
	{
		return Cue.Clips[Random.RandHelper(Cue.Clips.Num())];
	}
	const int32 Tones = GSoundTones.GetValueOnGameThread();
	if (Tones >= 2 || (Tones >= 1 && !Cue.bLoop))
	{
		return Cue.Tone;
	}
	return nullptr;
}

int32 USoundSubsystem::AddSound(const FCue& Cue, const FVector& Location, bool bEverywhere, colony::Entity Follow, uint64 Key, double Delay)
{
	FSound Sound;
	Sound.Cue = &Cue;
	Sound.Key = Key;
	Sound.Location = Location;
	Sound.Follow = Follow;
	Sound.bEverywhere = bEverywhere || Cue.RangeM <= 0.0f;
	Sound.Clip = PickClip(Cue);
	Sound.StartedAt = Now;
	Sound.Pitch = Cue.bLoop || Cue.Group == ESoundMix::Ambience ? 1.0f : Random.FRandRange(0.95f, 1.05f);
	++PlaysTotal;
	PlayCounts.FindOrAdd(Cue.Name) += 1;
	if (Follow != colony::kNoEntity)
	{
		EntityLocation(Follow, Sound.Location);
	}
	if (!Cue.bLoop && !Sound.bEverywhere && GSoundSpeedOfSound.GetValueOnGameThread() != 0)
	{
		// Sound takes its time: a shot 340 m off is heard a second after its flash. Under 10 m it is not worth a frame.
		const double Metres = FVector::Dist(Sound.Location, Ears) / 100.0;
		Sound.StartedAt = Now + (Metres > 10.0 ? Metres / SoundMetresPerSecond : 0.0);
	}
	// Asked to wait (a spent case lands after its shot, debris rains down after a blast).
	Sound.StartedAt += FMath::Max(0.0, Delay);
	Sound.EndsAt = Sound.StartedAt + (Sound.Clip ? Sound.Clip->Seconds() / Sound.Pitch : 0.0) + 0.5;
	// A shot or a blast with a distant layer plays both, crossfaded by distance in MixAndAssign.
	const FCue* Far = !Cue.FarCue.IsEmpty() && Cue.FarFromM > 0.0f && !Cue.bLoop ? Cues.Find(Cue.FarCue) : nullptr;
	if (Far && Far->Clips.Num() > 0 && Sound.Clip)
	{
		FSound Layer = Sound;
		Layer.Layer = 2;
		Layer.Clip = PickClip(*Far);
		Layer.EndsAt = Layer.StartedAt + (Layer.Clip ? Layer.Clip->Seconds() / Layer.Pitch : 0.0) + 0.5;
		Sound.Layer = 1;
		Sounds.Add(MoveTemp(Layer));
	}
	const int32 Index = Sounds.Add(MoveTemp(Sound));
	if (Key != 0)
	{
		LoopsByKey.Add(Key, Index);
	}
	return Index;
}

void USoundSubsystem::StopSound(int32 Index)
{
	if (!Sounds.IsValidIndex(Index))
	{
		return;
	}
	if (Sounds[Index].Voice >= 0)
	{
		FreeVoice(Sounds[Index].Voice);
	}
	Sounds[Index].bDead = true;
}

bool USoundSubsystem::PlayCue(const FString& Cue, const FVector& Location, bool bEverywhere)
{
	const FCue* Found = FindCue(Cue);
	if (!Found)
	{
		return false;
	}
	AddSound(*Found, Location, bEverywhere, colony::kNoEntity, 0);
	return true;
}

void USoundSubsystem::HandlePlay(const colony::SoundPlay& Play)
{
	USimSubsystem* Source = Bridge.Get();
	const FCue* Cue = FindCue(UTF8_TO_TCHAR(Play.cue.c_str()));
	if (!Source || !Cue)
	{
		return;
	}
	FVector Location = FVector::ZeroVector;
	bool bEverywhere = false;
	if (Play.hasPos)
	{
		Location = Source->GetWorldOrigin() + FVector(Play.x * 100.0, Play.y * 100.0, Play.z * 100.0 + 50.0);
		if (Play.z <= 0.01f && Source->GetSim())
		{
			Location.Z = Source->SimToWorld(Play.x, Play.y, 0).Z + 50.0;
		}
	}
	else if (Play.entity == colony::kNoEntity)
	{
		bEverywhere = true;
	}
	uint64 Key = 0;
	if (Play.mode != colony::SoundMode::Once)
	{
		if (Play.entity != colony::kNoEntity)
		{
			Key = SoundKey(ESoundKey::PawnLoop, SoundEntityId(Play.entity)) ^ (SoundNameId(Cue->Name) << 32);
		}
		else if (Play.hasPos)
		{
			Key = SoundKey(ESoundKey::FireCell, SoundCellId(Play.cell)) ^ (SoundNameId(Cue->Name) << 40);
		}
		else
		{
			Key = SoundKey(ESoundKey::Everywhere, SoundNameId(Cue->Name));
		}
	}
	if (Play.mode == colony::SoundMode::LoopStop)
	{
		if (const int32* Index = LoopsByKey.Find(Key))
		{
			StopSound(*Index);
			LoopsByKey.Remove(Key);
		}
		return;
	}
	if (Play.mode == colony::SoundMode::LoopStart && LoopsByKey.Contains(Key))
	{
		return;
	}
	if (Play.voice != colony::SoundVoice::None && Play.entity != colony::kNoEntity)
	{
		PlayVoice(*Cue, Play, Location);
		return;
	}
	AddSound(*Cue, Location, bEverywhere, Play.entity, Key, Play.delay);
	// Work goes on: a cue with a `repeat` plays again every so often while its pawn stays at the same job (UpdateWorkBeats).
	if (Cue->RepeatS > 0.0f && Play.entity != colony::kNoEntity && Play.mode == colony::SoundMode::Once)
	{
		const colony::ThingView* View = Source->GetCurrentSnapshot().find(Play.entity);
		FVector At;
		if (View && EntityLocation(Play.entity, At))
		{
			FWorkBeat& Beat = WorkBeats.FindOrAdd(SoundEntityId(Play.entity));
			Beat.Cue = Cue;
			Beat.Job = View->job;
			Beat.Start = At;
			Beat.NextAt = Now + Cue->RepeatS * Random.FRandRange(0.85f, 1.15f);
			Beat.Until = Now + 600.0;  // ten real minutes at most, whatever happens
		}
	}
}

colony::Entity USoundSubsystem::ControlledPawn() const
{
	const UProtagonistSubsystem* Protagonist = GetWorld() ? GetWorld()->GetSubsystem<UProtagonistSubsystem>() : nullptr;
	return Protagonist && Protagonist->IsDirectMode() ? Protagonist->GetControlled() : colony::kNoEntity;
}

void USoundSubsystem::UpdateWorkBeats(const colony::Snapshot& Snap)
{
	for (auto It = WorkBeats.CreateIterator(); It; ++It)
	{
		FWorkBeat& Beat = It.Value();
		const colony::ThingView* View = nullptr;
		for (const colony::ThingView& V : Snap.things)
		{
			if (SoundEntityId(V.id) == It.Key())
			{
				View = &V;
				break;
			}
		}
		FVector At;
		// Stops when the pawn is gone, down, asleep, on another job, or walked off (work is done standing).
		if (!View || View->dead || View->downed || View->asleep || View->job != Beat.Job || !Beat.Cue || Now > Beat.Until ||
			!EntityLocation(View->id, At) || FVector::Dist2D(At, Beat.Start) > 80.0)
		{
			It.RemoveCurrent();
			continue;
		}
		if (Now >= Beat.NextAt)
		{
			AddSound(*Beat.Cue, At, false, View->id, 0);
			Beat.NextAt = Now + Beat.Cue->RepeatS * Random.FRandRange(0.85f, 1.15f);
		}
	}
}

void USoundSubsystem::UpdateFlybys(const colony::Snapshot& Snap, const USimSubsystem& Source)
{
	++FlyFrame;
	if (!bEarsOnMap)
	{
		return;
	}
	const FVector Origin = Source.GetWorldOrigin();
	for (const colony::ProjectileView& P : Snap.projectiles)
	{
		const FDefSounds& Slots = SoundsOfDef(P.def);
		if (Slots.Flyby.IsEmpty() && Slots.Incoming.IsEmpty())
		{
			continue;
		}
		const uint32 Id = SoundEntityId(P.id);
		FFlyWatch& W = FlyWatch.FindOrAdd(Id);
		W.SeenFrame = FlyFrame;
		if (W.bDone)
		{
			continue;
		}
		const FVector Where = Origin + FVector(P.x * 100.0, P.y * 100.0, P.z * 100.0);
		const float DistM = FVector::Dist(Where, Ears) / 100.0f;
		if (!Slots.Incoming.IsEmpty())
		{
			// A shell whistles for its last ~1.7 s down: when it falls and the ground is that close in time.
			const float StepsPerSecond = 30.0f;
			if (P.vz < 0.0f)
			{
				const float Ground = Source.SimToWorld(P.x, P.y, P.level).Z / 100.0f - Origin.Z / 100.0f;
				const float Seconds = (P.z - Ground) / (-P.vz * StepsPerSecond);
				if (Seconds <= 1.8f)
				{
					const FVector Land = Origin + FVector((P.x + P.vx * Seconds * StepsPerSecond) * 100.0, (P.y + P.vy * Seconds * StepsPerSecond) * 100.0, Ground * 100.0 + 50.0);
					if (const FCue* Cue = FindCue(Slots.Incoming); Cue && FVector::Dist(Land, Ears) / 100.0f < Cue->RangeM)
					{
						AddSound(*Cue, Land, false, colony::kNoEntity, 0);
					}
					W.bDone = true;
				}
			}
			continue;
		}
		// A bullet or arrow that came from afar (so not the player's own shot) and passes within 3 m: at its closest
		// point, the whiz or whoosh.
		W.bWasFar = W.bWasFar || DistM > 8.0f;
		if (W.bWasFar && DistM > W.LastDistM && W.LastDistM < 3.0f)
		{
			if (const FCue* Cue = FindCue(Slots.Flyby))
			{
				AddSound(*Cue, Where, false, colony::kNoEntity, 0);
			}
			W.bDone = true;
		}
		W.LastDistM = DistM;
	}
	for (auto It = FlyWatch.CreateIterator(); It; ++It)
	{
		if (It.Value().SeenFrame != FlyFrame)
		{
			It.RemoveCurrent();
		}
	}
}

void USoundSubsystem::PlayVoice(const FCue& Cue, const colony::SoundPlay& Play, const FVector& Location)
{
	// One voice per pawn: a scream cuts that pawn's hit grunt short and is never cut by one; a grunt waits out its
	// cooldown. Nobody else's sounds are touched.
	FPawnVoice& Voice = PawnVoices.FindOrAdd(SoundEntityId(Play.entity));
	const bool bSpeaking = Sounds.IsValidIndex(Voice.Sound) && !Sounds[Voice.Sound].bDead && Sounds[Voice.Sound].VoiceOf == Play.entity;
	const bool bScreaming = bSpeaking && Sounds[Voice.Sound].VoiceKind == colony::SoundVoice::Scream;
	const TCHAR* Skip = nullptr;
	if (Play.voice == colony::SoundVoice::Hit)
	{
		if (Now - Voice.LastHitAt < Cue.CooldownS)
		{
			Skip = TEXT("hit grunt in its cooldown");
		}
		else if (bScreaming)
		{
			Skip = TEXT("hit grunt over a scream");
		}
	}
	else if (Play.voice == colony::SoundVoice::Moan && bSpeaking)
	{
		Skip = TEXT("moan while that pawn speaks");
	}
	else if (bScreaming && Now - Sounds[Voice.Sound].StartedAt < SoundScreamRepeatS)
	{
		Skip = TEXT("already screaming");
	}
	if (Skip)
	{
		++VoicesSkipped;
		UE_CLOG(GSoundDebug.GetValueOnGameThread() >= 2, LogTemp, Log, TEXT("Sound: %s not played: %s"), *Cue.Name, Skip);
		return;
	}
	if (bSpeaking)
	{
		++VoicesCut;
		UE_CLOG(GSoundDebug.GetValueOnGameThread() >= 2, LogTemp, Log, TEXT("Sound: %s cut short by %s"), *Sounds[Voice.Sound].Cue->Name, *Cue.Name);
		CutSound(Voice.Sound);
	}
	const colony::SoundVoice Kind = Play.voice;
	const int32 Index = AddSound(Cue, Location, false, Play.entity, 0);
	Sounds[Index].VoiceOf = Play.entity;
	Sounds[Index].VoiceKind = Kind;
	Voice.Sound = Index;  // AddSound and CutSound leave PawnVoices alone, so the reference still holds
	if (Kind == colony::SoundVoice::Hit)
	{
		Voice.LastHitAt = Now;
	}
}

void USoundSubsystem::CutSound(int32 Index, float FadeSeconds)
{
	if (!Sounds.IsValidIndex(Index))
	{
		return;
	}
	FSound& Sound = Sounds[Index];
	if (Voices.IsValidIndex(Sound.Voice))
	{
		FVoice& Voice = Voices[Sound.Voice];
		if (Voice.Component)
		{
			Voice.Component->FadeOut(FadeSeconds, 0.0f);
		}
		Voice.Sound = -1;
		Voice.Wave = nullptr;
		Voice.BusyUntil = Now + FadeSeconds + 0.04;
	}
	Sound.Voice = -1;
	Sound.bDead = true;
}

void USoundSubsystem::UpdateLovin()
{
	// Lovin (docs/design/sound.md "Lovin"): a man and a woman pick from all six of sneed's recordings, women (two, or one
	// alone) from the three women's ones, men nothing. It is an ordinary sound at the bed, so a closed room keeps it in.
	for (FLovin& L : Lovins)
	{
		if (L.bPlayed || Now < L.PlayAt)
		{
			continue;
		}
		L.bPlayed = true;
		const FString Name = UTF8_TO_TCHAR(colony::soundLovinCue(GenderOf(L.A), GenderOf(L.B)).c_str());
		const FCue* Cue = Name.IsEmpty() ? nullptr : FindCue(Name);
		if (!Cue)
		{
			continue;
		}
		FVector Where = FVector::ZeroVector;
		EntityLocation(L.A, Where);
		L.Key = SoundKey(ESoundKey::Lovin, SoundEntityId(L.A));
		AddSound(*Cue, Where, false, L.A, L.Key);
		++LovinPlays;
	}
}

void USoundSubsystem::StartLovinForTest(colony::Entity A, colony::Entity B)
{
	FLovin L;
	L.A = A;
	L.B = B;
	L.StartedAt = Now;
	L.PlayAt = Now;
	Lovins.Add(L);
}

void USoundSubsystem::UpdateBirds(const colony::Sim& Sim, const colony::Snapshot& Snap)
{
	(void)Snap;
	// A burst when rain comes in: the weather's loop turning to rain.
	const bool bRaining = WeatherCue.Contains(TEXT("Rain"));
	const bool bRainComing = bRaining && !LastWeatherLoop.Contains(TEXT("Rain"));
	LastWeatherLoop = WeatherCue;
	const USimSubsystem* Source = Bridge.Get();
	const colony::Def* Biome = Source && Source->GetDefs() ? Source->GetDefs()->find("Biomes", Sim.biome()) : nullptr;
	const std::string Slot = Biome ? colony::soundSlot(*Biome, "birds") : std::string();
	const FCue* Cue = Slot.empty() ? nullptr : FindCue(UTF8_TO_TCHAR(Slot.c_str()));
	if (!Cue || GSoundBirds.GetValueOnGameThread() == 0)
	{
		return;
	}
	// A new day: will the birds sing at all today (not every day), and how long they may (2% of the day's real length,
	// about a minute with the 48-minute day).
	const int64 Day = static_cast<int64>(Sim.time().day());
	if (Day != BirdDay)
	{
		BirdDay = Day;
		bBirdDay = Random.FRand() < SoundBirdDayChance;
		BirdSecondsLeft = SoundBirdShareOfDay * static_cast<float>(FMath::Max(1, Sim.dayMinutes())) * 60.0f;
		BirdSecondsPlayed = 0.0f;
	}
	if (bRainComing)
	{
		BirdBurstUntil = Now + 20.0;
	}
	if (Now < NextBirdCheck)
	{
		return;
	}
	NextBirdCheck = Now + 8.0;
	const double HourSeconds = static_cast<double>(colony::kSecondsPerHour);
	const float Hour = static_cast<float>(FMath::Fmod(static_cast<double>(Sim.time().seconds), 24.0 * HourSeconds) / HourSeconds);
	const bool bDawnOrDusk = (Hour >= 5.5f && Hour < 7.5f) || (Hour >= 17.5f && Hour < 19.5f);
	const bool bBurst = Now < BirdBurstUntil;
	if (BirdSecondsLeft <= 0.0f || (!bBirdDay && !bBurst) || (bRaining && !bBurst))
	{
		return;
	}
	const float Chance = bBurst ? 0.5f : bDawnOrDusk ? 0.07f : 0.0f;
	if (Chance <= 0.0f || Random.FRand() >= Chance)
	{
		return;
	}
	// Somewhere in the trees round the ears, 15 to 40 m off and up in the canopy, so distance, walls and echo apply.
	const float Angle = Random.FRandRange(0.0f, 2.0f * PI);
	const float Dist = Random.FRandRange(1500.0f, 4000.0f);
	const FVector Where = Ears + FVector(FMath::Cos(Angle) * Dist, FMath::Sin(Angle) * Dist, 400.0f);
	const int32 Index = AddSound(*Cue, Where, false, colony::kNoEntity, 0);
	const float Length = Sounds[Index].Clip ? Sounds[Index].Clip->Seconds() : 0.0f;
	BirdSecondsLeft -= Length;
	BirdSecondsPlayed += Length;
	++BirdPlays;
	if (bBurst)
	{
		BirdBurstUntil = -1.0;
	}
}

bool USoundSubsystem::IsMusicMuted() const
{
	return GSoundMusicMuted.GetValueOnGameThread() != 0;
}

bool USoundSubsystem::IsAllMuted() const
{
	return GSoundEnable.GetValueOnGameThread() == 0;
}

void USoundSubsystem::ToggleMusicMute()
{
	const bool bMute = !IsMusicMuted();
	GSoundMusicMuted.AsVariable()->Set(bMute ? 1 : 0, ECVF_SetByCode);
	SaveMixer();
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(0x50D1, 2.5f, FColor::White, bMute ? TEXT("Music muted (Ctrl+M)") : TEXT("Music on (Ctrl+M)"));
	}
}

void USoundSubsystem::ToggleAllMute()
{
	const bool bMute = !IsAllMuted();
	GSoundEnable.AsVariable()->Set(bMute ? 0 : 1, ECVF_SetByCode);
	SaveMixer();
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(0x50D1, 2.5f, FColor::White, bMute ? TEXT("All sound muted (Ctrl+S)") : TEXT("Sound on (Ctrl+S)"));
	}
}

void USoundSubsystem::UpdateMuteKeys()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC || !(PC->IsInputKeyDown(EKeys::LeftControl) || PC->IsInputKeyDown(EKeys::RightControl)))
	{
		return;
	}
	if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput)
	{
		return;  // typing in a text box
	}
	if (PC->WasInputKeyJustPressed(EKeys::M))
	{
		ToggleMusicMute();
	}
	if (PC->WasInputKeyJustPressed(EKeys::S))
	{
		ToggleAllMute();
	}
}

FString USoundSubsystem::GetPawnVoice(colony::Entity Pawn) const
{
	const FPawnVoice* Voice = PawnVoices.Find(SoundEntityId(Pawn));
	if (Voice && Sounds.IsValidIndex(Voice->Sound) && !Sounds[Voice->Sound].bDead && Sounds[Voice->Sound].VoiceOf == Pawn)
	{
		return Sounds[Voice->Sound].Cue->Name;
	}
	return FString();
}

float USoundSubsystem::DistanceGain(float NearM, float RangeM, float DistanceM)
{
	if (RangeM <= 0.0f)
	{
		return 1.0f;
	}
	if (DistanceM >= RangeM)
	{
		return 0.0f;
	}
	const float Near = FMath::Clamp(NearM, 0.25f, RangeM);
	float Gain = Near / FMath::Max(DistanceM, Near);
	const float FadeFrom = RangeM * 0.7f;
	if (DistanceM > FadeFrom)
	{
		const float T = (DistanceM - FadeFrom) / (RangeM - FadeFrom);
		Gain *= 1.0f - T * T * (3.0f - 2.0f * T);
	}
	return Gain;
}

float USoundSubsystem::AirCutoff(float RangeM, float DistanceM)
{
	if (RangeM <= 0.0f)
	{
		return 20000.0f;
	}
	return FMath::Max(800.0f, 20000.0f * FMath::Pow(2.0f, -4.0f * FMath::Max(0.0f, DistanceM) / RangeM));
}

USoundAttenuation* USoundSubsystem::SpatialFor(const FCue& Cue)
{
	const int32 Range = FMath::Max(1, FMath::RoundToInt(Cue.RangeM));
	if (TObjectPtr<USoundAttenuation>* Found = SpatialByRange.Find(Range))
	{
		return Found->Get();
	}
	USoundAttenuation* A = NewObject<USoundAttenuation>(this);
	A->Attenuation.bAttenuate = false;         // the distance falloff is ours (DistanceGain)
	A->Attenuation.bSpatialize = true;
	A->Attenuation.bAttenuateWithLPF = false;  // so is the low-pass (air and walls)
	// More echo the farther a sound is: close, you hear it dry; far, mostly what comes back off trees and walls.
	A->Attenuation.bEnableReverbSend = true;
	A->Attenuation.ReverbSendMethod = EReverbSendMethod::Linear;
	A->Attenuation.ReverbWetLevelMin = 0.15f;
	A->Attenuation.ReverbWetLevelMax = 0.8f;
	A->Attenuation.ReverbDistanceMin = FMath::Clamp(Cue.NearM, 0.5f, FMath::Max(Cue.RangeM, 0.5f)) * 100.0f;
	A->Attenuation.ReverbDistanceMax = FMath::Max(Cue.RangeM, Cue.NearM + 1.0f) * 100.0f;
	SpatialByRange.Add(Range, A);
	return A;
}

void USoundSubsystem::UpdateEnvironment(const colony::Sim& Sim)
{
	int32 Place = 0;
	if (bEarsOnMap)
	{
		const colony::Roof Roof = Sim.map().roof(colony::CellPos{EarCell.x, EarCell.y, 0});
		Place = Roof == colony::Roof::Thick ? 2 : Roof == colony::Roof::Thin ? 1 : 0;
	}
	if (GSoundEcho.GetValueOnGameThread() == 0)
	{
		Place = -2;
	}
	if (Place == Environment)
	{
		return;
	}
	Environment = Place;
	if (Place < 0)
	{
		UGameplayStatics::DeactivateReverbEffect(this, SoundReverbTag);
		return;
	}
	static const float SoundWetByPlace[] = {0.35f, 0.5f, 0.7f};
	if (Reverbs.IsValidIndex(Place) && Reverbs[Place])
	{
		UGameplayStatics::ActivateReverbEffect(this, Reverbs[Place], SoundReverbTag, 1.0f, SoundWetByPlace[Place], 1.0f);
	}
}


void USoundSubsystem::KeepLoop(const FString& CueName, uint64 Key, const FVector& Location, colony::Entity Follow, bool bEverywhere, float Gain)
{
	const FCue* Cue = FindCue(CueName);
	if (!Cue)
	{
		return;
	}
	int32 Index = INDEX_NONE;
	if (const int32* Found = LoopsByKey.Find(Key))
	{
		Index = *Found;
	}
	else
	{
		Index = AddSound(*Cue, Location, bEverywhere, Follow, Key);
	}
	FSound& Sound = Sounds[Index];
	Sound.bScanned = true;
	Sound.bSeen = true;
	Sound.TargetGain = Gain;
	if (Follow == colony::kNoEntity)
	{
		Sound.Location = Location;
	}
}

// ---- Tick -----------------------------------------------------------------------------------------------------------

void USoundSubsystem::Tick(float DeltaTime)
{
	Now += DeltaTime;
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim() || !Source->GetDefs())
	{
		return;
	}
	if (!Holder)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
		if (!Holder)
		{
			return;
		}
		Spatial = NewObject<USoundAttenuation>(this);
		Spatial->Attenuation.bAttenuate = false;  // the distance falloff is ours (the cue's range)
		Spatial->Attenuation.bSpatialize = true;
		Spatial->Attenuation.bAttenuateWithLPF = false;
		MusicComponent = MakeComponent();
		StingerComponent = MakeComponent();
		for (int32 Place = 0; Place < 3; ++Place)
		{
			Reverbs.Add(SoundMakeReverb(this, Place));
		}
	}
	if (SeenGeneration != Source->GetSimGeneration())
	{
		SeenGeneration = Source->GetSimGeneration();
		LoadCues();
		ResetForSim();
	}
	while (Voices.Num() < GetVoiceBudget())
	{
		FVoice Voice;
		Voice.Component = MakeComponent();
		Voices.Add(Voice);
	}

	UpdateEars();
	UpdateEnvironment(*Source->GetSim());
	for (const colony::SoundPlay& Play : PendingPlays)
	{
		HandlePlay(Play);
	}
	PendingPlays.clear();
	ReadSnapshot(*Source, DeltaTime);
	UpdateLovin();
	UpdateBirds(*Source->GetSim(), Source->GetCurrentSnapshot());
	UpdateMuteKeys();
	MixAndAssign(Source, DeltaTime);
	UpdateMusic(Source->GetCurrentSnapshot(), DeltaTime);

	if (GSoundDebug.GetValueOnGameThread() != 0 && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(0x50D0, 0.0f, FColor::Cyan, FString::Printf(TEXT("Sound: %d voices of %d, %d sounds, %d dropped, place %d, bed %s, weather %s, music %d %s"),
			GetVoicesInUse(), GetVoiceBudget(), Sounds.Num(), Dropped, Environment, *BedCue, *WeatherCue, MusicMood, *MusicTrackName));
	}
}

void USoundSubsystem::UpdateEars()
{
	UWorld* World = GetWorld();
	APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	if (Controller)
	{
		if (const AOverseerPawn* Overseer = Cast<AOverseerPawn>(Controller->GetViewTarget()))
		{
			// From above the map you hear what you look at, a little raised so the far side of a room is not silent.
			Ears = Overseer->GetFocus() + FVector(0.0, 0.0, 300.0);
		}
		else if (Controller->PlayerCameraManager)
		{
			Ears = Controller->PlayerCameraManager->GetCameraLocation();
		}
	}
	if (const USimSubsystem* Source = Bridge.Get())
	{
		EarCell = SoundCellAt(*Source, Ears);
		bEarsOnMap = Source->GetSim() && Source->GetSim()->map().inBounds(colony::CellPos{EarCell.x, EarCell.y, 0});
	}
}

int32 USoundSubsystem::OcclusionAt(const FVector& Location) const
{
	const USimSubsystem* Source = Bridge.Get();
	const colony::Sim* Sim = Source ? Source->GetSim() : nullptr;
	if (!Sim || !bEarsOnMap)
	{
		return 0;
	}
	const colony::Map& Map = Sim->map();
	const colony::CellPos From = SoundCellAt(*Source, Location);
	if (!Map.inBounds(colony::CellPos{From.x, From.y, 0}))
	{
		return 0;
	}
	int32 Count = 0;
	// Walls and shut doors on the line between, not the ends themselves.
	const int32 DX = EarCell.x - From.x;
	const int32 DY = EarCell.y - From.y;
	const int32 Steps = FMath::Max(FMath::Abs(DX), FMath::Abs(DY));
	colony::CellPos Last = From;
	for (int32 S = 1; S < Steps && Count < 3; ++S)
	{
		const colony::CellPos C{From.x + FMath::RoundToInt(DX * float(S) / Steps), From.y + FMath::RoundToInt(DY * float(S) / Steps), From.level};
		if ((C.x != Last.x || C.y != Last.y) && Map.inBounds(C))
		{
			const colony::Passage Pass = Map.passage(C);
			bool bShut = Pass == colony::Passage::Blocked;
			if (Pass == colony::Passage::Door)
			{
				// A shut door muffles like a wall; an open one lets it through.
				const colony::Entity Door = Map.buildingAt(C);
				const float* Open = DoorOpen.Find(SoundEntityId(Door));
				bShut = !Open || *Open < 0.3f;
			}
			Count += bShut ? 1 : 0;
		}
		Last = C;
	}
	// Under a roof at one end and not the other: through the roof or the walls of the room.
	const colony::CellPos EarGround{EarCell.x, EarCell.y, FMath::Min(EarCell.level, From.level)};
	if ((Map.roof(From) != colony::Roof::None) != (Map.roof(EarGround) != colony::Roof::None))
	{
		++Count;
	}
	return FMath::Min(Count, 3);
}

// ---- What the snapshot sounds like ------------------------------------------------------------------------------------

const USoundSubsystem::FDefSounds& USoundSubsystem::SoundsOfDef(colony::DefId Def)
{
	if (FDefSounds* Have = DefSounds.Find(Def.value))
	{
		return *Have;
	}
	FDefSounds Slots;
	const USimSubsystem* Source = Bridge.Get();
	if (Source && Source->GetDefs() && Def.valid())
	{
		const colony::Def& D = Source->GetDefs()->get(Def);
		Slots.Open = UTF8_TO_TCHAR(colony::soundSlot(D, "open").c_str());
		Slots.Close = UTF8_TO_TCHAR(colony::soundSlot(D, "close").c_str());
		Slots.Loop = UTF8_TO_TCHAR(colony::soundSlot(D, "loop").c_str());
		Slots.Idle = UTF8_TO_TCHAR(colony::soundSlot(D, "idle").c_str());
		Slots.Step = D.kind() == "Terrain" ? FString(UTF8_TO_TCHAR(colony::soundStepCue(*Source->GetDefs(), Def).c_str())) : FString();
		Slots.Flyby = UTF8_TO_TCHAR(colony::soundSlot(D, "flyby").c_str());
		Slots.Incoming = UTF8_TO_TCHAR(colony::soundSlot(D, "incoming").c_str());
	}
	return DefSounds.Add(Def.value, Slots);
}

FString USoundSubsystem::StepCueAt(const colony::Sim& Sim, const colony::CellPos& Cell)
{
	const colony::DefId Floor = Sim.floors().at(Cell);
	if (Floor.valid())
	{
		return SoundsOfDef(Floor).Step;
	}
	const colony::Terrain Ground = Sim.map().terrain(Cell);
	if (Ground == colony::Terrain::UpperFloor)
	{
		return TEXT("Step_Wood");
	}
	const int32 Kind = static_cast<int32>(Ground);
	if (const FString* Have = GroundStep.Find(Kind))
	{
		return *Have;
	}
	const char* Name = "Soil";
	switch (Ground)
	{
		case colony::Terrain::Water: Name = "WaterShallow"; break;
		case colony::Terrain::Sand: Name = "Sand"; break;
		case colony::Terrain::Rock:
		case colony::Terrain::RoughStone: Name = "Granite_Rough"; break;
		default: break;
	}
	const USimSubsystem* Source = Bridge.Get();
	FString Cue = TEXT("Step_Dirt");
	if (Source && Source->GetDefs())
	{
		Cue = UTF8_TO_TCHAR(colony::soundStepCue(*Source->GetDefs(), Source->GetDefs()->idOf("Terrain", Name)).c_str());
	}
	return GroundStep.Add(Kind, Cue);
}

void USoundSubsystem::ReadSnapshot(const USimSubsystem& Source, float Delta)
{
	const colony::Sim& Sim = *Source.GetSim();
	const colony::Snapshot& Snap = Source.GetCurrentSnapshot();
	UpdateSteps(Sim, Snap, Source, Delta);
	UpdateWorkBeats(Snap);
	UpdateFlybys(Snap, Source);
	LoopScanAge += Delta;
	if (LoopScanAge >= 0.1f)
	{
		LoopScanAge = 0.0f;
		UpdateLoops(Snap, Source);
	}
	UpdateBeds(Sim, Snap, Delta);
}

void USoundSubsystem::UpdateSteps(const colony::Sim& Sim, const colony::Snapshot& Snap, const USimSubsystem& Source, float Delta)
{
	constexpr float HearStepsCm = 2600.0f;
	TSet<uint32> Seen;
	// The player's own character makes no footsteps (sneed, 2026-11-20); everyone else does.
	const colony::Entity Me = ControlledPawn();
	for (const colony::ThingView& View : Snap.things)
	{
		if (View.category != colony::ThingCategory::Pawn || View.dead || View.id == Me)
		{
			continue;
		}
		const uint32 Id = SoundEntityId(View.id);
		FVector Location;
		if (!Source.GetBlendedLocation(View.id, Location))
		{
			continue;
		}
		const bool bAnimal = View.lifeStage >= 0;
		const bool bNear = FVector::DistSquared(Location, Ears) < FMath::Square(HearStepsCm + 1000.0f);
		if (bNear)
		{
			Seen.Add(Id);
			FVector* Last = LastStepPos.Find(Id);
			if (!Last)
			{
				LastStepPos.Add(Id, Location);
			}
			else
			{
				const float Moved = FVector::Dist2D(Location, *Last) / 100.0f;
				*Last = Location;
				float& Walked = StepDistance.FindOrAdd(Id);
				// A stride is about 0.7 m for a person, longer for big animals; a jump (a load, a teleport) is not walking.
				const float Stride = bAnimal ? FMath::Clamp(0.5f + 0.5f * View.bodySize, 0.35f, 2.0f) : 0.7f;
				if (Moved > 3.0f || View.downed)
				{
					Walked = 0.0f;
				}
				else
				{
					Walked += Moved;
				}
				// Footsteps are quiet and short-ranged: crouched (sneaking) a third as loud and heard within 8 m, prone a
				// fifth within 5 m, running half again as loud.
				const bool bCrouched = !bAnimal && View.stance == colony::Stance::Crouching;
				const bool bProne = !bAnimal && View.stance == colony::Stance::Prone;
				const float StepGain = bProne ? 0.2f : bCrouched ? 0.35f : View.sprinting ? 1.5f : 1.0f;
				const float HearCm = bProne ? 500.0f : bCrouched ? 800.0f : HearStepsCm;
				if (Walked >= Stride && FVector::DistSquared(Location, Ears) < FMath::Square(HearCm))
				{
					Walked = FMath::Fmod(Walked, Stride);
					const colony::CellPos Cell{FMath::FloorToInt(View.x), FMath::FloorToInt(View.y), View.level};
					if (const FCue* Cue = FindCue(StepCueAt(Sim, Cell)))
					{
						const int32 Index = AddSound(*Cue, Location + FVector(0.0, 0.0, 10.0), false, colony::kNoEntity, 0);
						Sounds[Index].Gain = (bAnimal ? FMath::Clamp(View.bodySize, 0.3f, 1.5f) : 1.0f) * StepGain;
					}
				}
			}
		}
		// People lying downed and awake moan, groan and beg now and then (cosmetic, Unreal's own random): only while that
		// pawn's voice is silent, so a hit grunt or a scream always wins.
		if (!bAnimal && View.downedAwake && bNear)
		{
			float& Next = NextMoan.FindOrAdd(Id);
			const FString CueName = Source.GetDefs() && View.def.valid()
				? FString(UTF8_TO_TCHAR(colony::soundDownedVoice(Source.GetDefs()->get(View.def), GenderOf(View.id)).c_str())) : FString();
			const FCue* Cue = CueName.IsEmpty() ? nullptr : FindCue(CueName);
			if (Cue && Next <= 0.0f)
			{
				Next = Random.FRandRange(1.5f, FMath::Max(Cue->EveryS.X, 2.0f));  // the first a moment after going down
			}
			Next -= Delta;
			if (Cue && Next <= 0.0f)
			{
				colony::SoundPlay Play;
				Play.cue = TCHAR_TO_UTF8(*CueName);
				Play.entity = View.id;
				Play.voice = colony::SoundVoice::Moan;
				PlayVoice(*Cue, Play, Location + FVector(0.0, 0.0, 20.0));
				Next = Random.FRandRange(FMath::Max(Cue->EveryS.X, 1.0f), FMath::Max(Cue->EveryS.Y, Cue->EveryS.X + 1.0f));
			}
		}
		else if (!bAnimal)
		{
			NextMoan.Remove(Id);
		}
		// Animals, mechanoids and chitinids call now and then (cosmetic, Unreal's own random).
		const bool bCalls = bAnimal || !SoundsOfDef(View.def).Idle.IsEmpty();
		if (bCalls && !View.downed && !View.asleep && bNear)
		{
			float& Next = NextCall.FindOrAdd(Id);
			if (Next <= 0.0f)
			{
				Next = Random.FRandRange(8.0f, 40.0f);
			}
			Next -= Delta;
			if (Next <= 0.0f)
			{
				const FString& Idle = SoundsOfDef(View.def).Idle;
				if (const FCue* Cue = Idle.IsEmpty() ? nullptr : FindCue(Idle))
				{
					AddSound(*Cue, Location + FVector(0.0, 0.0, 60.0), false, View.id, 0);
				}
				Next = Random.FRandRange(20.0f, 60.0f);
			}
		}
	}
	for (auto It = LastStepPos.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key()))
		{
			StepDistance.Remove(It.Key());
			It.RemoveCurrent();
		}
	}
}

void USoundSubsystem::UpdateLoops(const colony::Snapshot& Snap, const USimSubsystem& Source)
{
	TSet<uint32> Doors;
	for (FSound& Sound : Sounds)
	{
		Sound.bSeen = false;
	}
	for (const colony::ThingView& View : Snap.things)
	{
		if (View.category != colony::ThingCategory::Building || View.stage != colony::ConstructionStage::Built)
		{
			continue;
		}
		const FDefSounds& Slots = SoundsOfDef(View.def);
		if (Slots.Open.IsEmpty() && Slots.Loop.IsEmpty())
		{
			continue;
		}
		const FVector Location = Source.SimToWorld(View.x, View.y, View.level) + FVector(0.0, 0.0, 100.0);
		if (!Slots.Open.IsEmpty())
		{
			// A door opening from shut, and shutting again.
			const uint32 Id = SoundEntityId(View.id);
			Doors.Add(Id);
			float& Was = DoorOpen.FindOrAdd(Id, View.doorOpen);
			const FString* Cue = Was <= 0.05f && View.doorOpen > 0.05f ? &Slots.Open : (Was > 0.05f && View.doorOpen <= 0.05f ? &Slots.Close : nullptr);
			if (Cue && !Cue->IsEmpty() && FVector::DistSquared(Location, Ears) < FMath::Square(4000.0f))
			{
				PlayCue(*Cue, Location);
			}
			Was = View.doorOpen;
		}
		// Machines hum while they run, fires crackle while they have fuel.
		const bool bPowerOk = View.power == colony::PowerState::None || View.power == colony::PowerState::Powered;
		const bool bFuelOk = View.fuelCapacity <= 0.0f || View.fuel > 0.0f;
		if (!Slots.Loop.IsEmpty() && bPowerOk && bFuelOk && (View.power != colony::PowerState::None || View.fuelCapacity > 0.0f))
		{
			KeepLoop(Slots.Loop, SoundKey(ESoundKey::Building, SoundEntityId(View.id)), Location, colony::kNoEntity, false, 1.0f);
		}
	}
	// Weapons in hand that hum (the plasma sword) or spin (the minigun's motor, sneed 2026-11-23: "a wind up ... a wind
	// down that dynamically winds up again if you resume"): one loop each, its pitch and loudness following the barrels'
	// spin, so letting go and firing again bends the same sound back up instead of cutting it.
	const UWeaponDrawSubsystem* Draw = GetWorld()->GetSubsystem<UWeaponDrawSubsystem>();
	const colony::DefDatabase& LoopDefs = *Source.GetDefs();
	for (const colony::ThingView& View : Snap.things)
	{
		if (View.category != colony::ThingCategory::Pawn || View.dead || View.downed || !View.weapon.valid())
		{
			continue;
		}
		const colony::Def& WeaponDef = LoopDefs.get(View.weapon);
		const FString Hum = UTF8_TO_TCHAR(colony::soundSlot(WeaponDef, "hum").c_str());
		const FString Spin = UTF8_TO_TCHAR(colony::soundSlot(WeaponDef, "spin").c_str());
		if (Hum.IsEmpty() && Spin.IsEmpty())
		{
			continue;
		}
		const FVector Location = Source.SimToWorld(View.x, View.y, View.level) + FVector(0.0, 0.0, 100.0);
		if (FVector::DistSquared(Location, Ears) > FMath::Square(12000.0f))
		{
			continue;
		}
		if (!Hum.IsEmpty())
		{
			KeepLoop(Hum, SoundKey(ESoundKey::WeaponLoop, SoundEntityId(View.id)) ^ (SoundNameId(Hum) << 32), Location, View.id, false, 1.0f);
		}
		const float Share = !Spin.IsEmpty() && Draw ? Draw->GetSpinShare(View.id) : 0.0f;
		if (Share > 0.02f)
		{
			const uint64 Key = SoundKey(ESoundKey::WeaponLoop, SoundEntityId(View.id)) ^ (SoundNameId(Spin) << 32);
			KeepLoop(Spin, Key, Location, View.id, false, 1.0f);
			if (const int32* Found = LoopsByKey.Find(Key))
			{
				FSound& Motor = Sounds[*Found];
				Motor.Gain = FMath::Sqrt(Share);
				Motor.Pitch = 0.45f + 0.65f * Share;
				Motor.bLivePitch = true;
			}
		}
	}
	for (auto It = DoorOpen.CreateIterator(); It; ++It)
	{
		if (!Doors.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	// Building loops that were not kept this scan (switched off, out of fuel, gone) stop.
	for (int32 I = 0; I < Sounds.Num(); ++I)
	{
		FSound& Sound = Sounds[I];
		if (!Sound.bDead && Sound.bScanned && !Sound.bSeen &&
			((Sound.Key >> 56) == static_cast<uint64>(ESoundKey::Building) || (Sound.Key >> 56) == static_cast<uint64>(ESoundKey::WeaponLoop)))
		{
			LoopsByKey.Remove(Sound.Key);
			StopSound(I);
		}
	}
}

void USoundSubsystem::UpdateBeds(const colony::Sim& Sim, const colony::Snapshot& Snap, float Delta)
{
	const USimSubsystem* Source = Bridge.Get();
	const colony::DefDatabase& Db = *Source->GetDefs();
	// The bed: the biome's day or night, indoors under a roof, the cave under the mountain's.
	FString Bed;
	if (const colony::Def* Biome = Db.find("Biomes", Sim.biome()))
	{
		const int32 Hour = Sim.time().hourOfDay();
		const char* Slot = Hour >= 6 && Hour < 20 ? "day" : "night";
		if (bEarsOnMap)
		{
			const colony::Roof Roof = Sim.map().roof(colony::CellPos{EarCell.x, EarCell.y, 0});
			if (Roof == colony::Roof::Thick)
			{
				Slot = "cave";
			}
			else if (Roof == colony::Roof::Thin)
			{
				Slot = "indoor";
			}
		}
		Bed = UTF8_TO_TCHAR(colony::soundSlot(*Biome, Slot).c_str());
	}
	BedCue = Bed;
	if (!Bed.IsEmpty())
	{
		KeepLoop(Bed, SoundKey(ESoundKey::Bed, SoundNameId(Bed)), Ears, colony::kNoEntity, true, 1.0f);
	}
	// The weather's loop on top, crossfading with the one before by the transition.
	const colony::WeatherView& W = Snap.weather;
	FString Now1;
	FString Before;
	if (W.weather.valid())
	{
		Now1 = UTF8_TO_TCHAR(colony::soundSlot(Db.get(W.weather), "loop").c_str());
	}
	if (W.previous.valid() && W.transition < 1.0f)
	{
		Before = UTF8_TO_TCHAR(colony::soundSlot(Db.get(W.previous), "loop").c_str());
	}
	WeatherCue = Now1;
	if (!Now1.IsEmpty())
	{
		KeepLoop(Now1, SoundKey(ESoundKey::Weather, SoundNameId(Now1)), Ears, colony::kNoEntity, true, FMath::Clamp(W.transition, 0.0f, 1.0f));
	}
	if (!Before.IsEmpty() && Before != Now1)
	{
		KeepLoop(Before, SoundKey(ESoundKey::Weather, SoundNameId(Before)), Ears, colony::kNoEntity, true, 1.0f - FMath::Clamp(W.transition, 0.0f, 1.0f));
	}
	// Beds and weather loops that are no longer wanted fade out, then stop.
	for (int32 I = 0; I < Sounds.Num(); ++I)
	{
		FSound& Sound = Sounds[I];
		const uint64 Kind = Sound.Key >> 56;
		if (Sound.bDead || (Kind != static_cast<uint64>(ESoundKey::Bed) && Kind != static_cast<uint64>(ESoundKey::Weather)))
		{
			continue;
		}
		const bool bWanted = Sound.Cue && (Sound.Cue->Name == Bed || Sound.Cue->Name == Now1 || Sound.Cue->Name == Before);
		if (!bWanted)
		{
			Sound.TargetGain = 0.0f;
		}
		Sound.Gain = FMath::FInterpConstantTo(Sound.Gain, Sound.TargetGain, Delta, 0.4f);
		if (!bWanted && Sound.Gain <= 0.0f)
		{
			LoopsByKey.Remove(Sound.Key);
			StopSound(I);
		}
	}
	// Thunder follows the sky's lightning flashes, later the farther it is.
	if (const USkySubsystem* Sky = GetWorld()->GetSubsystem<USkySubsystem>())
	{
		const int32 Flashes = Sky->GetFlashes();
		if (SeenFlashes >= 0 && Flashes > SeenFlashes)
		{
			ThunderAt.Add(Now + Random.FRandRange(0.4f, 2.5f));
		}
		SeenFlashes = Flashes;
	}
	for (int32 I = ThunderAt.Num() - 1; I >= 0; --I)
	{
		if (Now >= ThunderAt[I])
		{
			FString Thunder = TEXT("Weather_Thunder");
			if (W.weather.valid())
			{
				const std::string Slot = colony::soundSlot(Db.get(W.weather), "thunder");
				if (!Slot.empty())
				{
					Thunder = UTF8_TO_TCHAR(Slot.c_str());
				}
			}
			PlayCue(Thunder, Ears, true);
			ThunderAt.RemoveAt(I);
		}
	}
}

// ---- Voices ---------------------------------------------------------------------------------------------------------

void USoundSubsystem::StartVoice(int32 VoiceIndex, int32 SoundIndex)
{
	FVoice& Voice = Voices[VoiceIndex];
	FSound& Sound = Sounds[SoundIndex];
	UCueVoiceWave* Wave = NewObject<UCueVoiceWave>(this);
	// A loop that comes back into range picks up where it would be by now.
	const int64 Offset = Sound.Cue->bLoop && Sound.Clip ? static_cast<int64>((Now - Sound.StartedAt) * Sound.Clip->SampleRate) : 0;
	Wave->Setup(Sound.Clip, Sound.Cue->bLoop, Offset);
	// The death sting keeps its own pitch and speed while the world is slowed to a fifth: Unreal's audio is never slowed
	// by global time dilation (the voice wave is fed on the audio thread in real time), so nothing needs setting here.
	UAudioComponent* Component = Voice.Component;
	Component->Stop();
	// Never let Unreal's own voice limit or silence check drop it: which sounds play is decided here.
	Wave->Priority = 4.0f - static_cast<float>(Sound.Cue->Priority);
	Wave->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;
	Component->bAllowSpatialization = !Sound.bEverywhere;
	Component->AttenuationSettings = Sound.bEverywhere ? nullptr : SpatialFor(*Sound.Cue);
	Component->SetWorldLocation(Sound.Location);
	Component->SetSound(Wave);
	Component->SetVolumeMultiplier(Sound.Volume);
	Component->SetPitchMultiplier(Sound.Pitch);
	SoundApplyFilter(Component, Sound.Cutoff);
	Component->Play();
	Voice.Sound = SoundIndex;
	Voice.Wave = Wave;
	Voice.BusyUntil = 0.0;
	Sound.Voice = VoiceIndex;
	FIntPoint& Voiced = VoicedCounts.FindOrAdd(Sound.Cue->Name);
	const bool bFromFile = Sound.Clip.IsValid() && Sound.Cue->Clips.Contains(Sound.Clip);
	Voiced.X += 1;
	Voiced.Y += bFromFile ? 1 : 0;
	UE_CLOG(GSoundDebug.GetValueOnGameThread() >= 1, LogTemp, Log, TEXT("Sound: %s playing (%s) %.0f m away, volume %.2f"), *Sound.Cue->Name,
		bFromFile ? TEXT("recording") : TEXT("tone"), Sound.DistanceM, Sound.Volume);
	// A one-shot's clock starts when it really starts: a long frame before this must not end it early.
	Sound.EndsAt = Now + (Sound.Clip ? Sound.Clip->Seconds() / Sound.Pitch : 0.0) + 0.5;
	Sound.DoneAt = -1.0;
}

void USoundSubsystem::FreeVoice(int32 VoiceIndex)
{
	if (!Voices.IsValidIndex(VoiceIndex))
	{
		return;
	}
	FVoice& Voice = Voices[VoiceIndex];
	if (Voice.Component)
	{
		Voice.Component->Stop();
		Voice.Component->SetSound(nullptr);
	}
	Voice.Wave = nullptr;
	if (Sounds.IsValidIndex(Voice.Sound) && Sounds[Voice.Sound].Voice == VoiceIndex)
	{
		Sounds[Voice.Sound].Voice = -1;
	}
	Voice.Sound = -1;
}

int32 USoundSubsystem::GetVoicesInUse() const
{
	int32 N = 0;
	for (const FVoice& Voice : Voices)
	{
		N += Voice.Sound >= 0 ? 1 : 0;
	}
	return N;
}

void USoundSubsystem::MixAndAssign(const USimSubsystem* Source, float Delta)
{
	const bool bOn = GSoundEnable.GetValueOnGameThread() != 0;
	// Bring every sound up to date: where it is, how loud from here, and whether a one-shot is over.
	TArray<int32> Audible;
	for (int32 I = 0; I < Sounds.Num(); ++I)
	{
		FSound& Sound = Sounds[I];
		if (Sound.bDead)
		{
			continue;
		}
		if (Now < Sound.StartedAt)
		{
			continue;  // still on its way at the speed of sound
		}
		const bool bLoop = Sound.Cue->bLoop;
		if (!bLoop)
		{
			// Over when the clip has handed out its last sample and the device has had time to play it (not by the
			// game clock alone, which cut short sounds off after a long frame), or at the latest half a second late.
			const UCueVoiceWave* Wave = Voices.IsValidIndex(Sound.Voice) ? Voices[Sound.Voice].Wave : nullptr;
			if (Wave && Wave->IsDone() && Sound.DoneAt < 0.0)
			{
				Sound.DoneAt = Now;
			}
			if (Now >= Sound.EndsAt || (Sound.DoneAt >= 0.0 && Now - Sound.DoneAt >= 0.15))
			{
				StopSound(I);
				continue;
			}
		}
		if (Sound.Follow != colony::kNoEntity)
		{
			FVector Followed;
			if (EntityLocation(Sound.Follow, Followed))
			{
				Sound.Location = Followed + FVector(0.0, 0.0, 100.0);
			}
			else if (bLoop)
			{
				LoopsByKey.Remove(Sound.Key);
				StopSound(I);  // the burning pawn is gone
				continue;
			}
		}
		if (Sound.bEverywhere)
		{
			Sound.DistanceM = 0.0f;
			Sound.Location = Ears;
		}
		else
		{
			Sound.DistanceM = static_cast<float>(FVector::Dist(Sound.Location, Ears) / 100.0);
		}
		Sound.OcclusionAge += Delta;
		if (Sound.OcclusionAge >= 0.25f)
		{
			Sound.OcclusionAge = 0.0f;
			const bool bWeather = Sound.Cue->Name.StartsWith(TEXT("Weather_"));
			if (!Sound.bEverywhere)
			{
				Sound.Occlusion = OcclusionAt(Sound.Location);
			}
			else if (bWeather && bEarsOnMap && Source && Source->GetSim())
			{
				// Rain and wind outside are muffled indoors.
				Sound.Occlusion = Source->GetSim()->map().roof(colony::CellPos{EarCell.x, EarCell.y, 0}) != colony::Roof::None ? 2 : 0;
			}
		}
		float Distance = 1.0f;
		float Air = 20000.0f;
		if (!Sound.bEverywhere)
		{
			Distance = DistanceGain(Sound.Cue->NearM, Sound.Cue->RangeM, Sound.DistanceM);
			Air = AirCutoff(Sound.Cue->RangeM, Sound.DistanceM);
		}
		if (Sound.Layer != 0)
		{
			// Near layer out, far layer in, from `farFrom` to four times it (equal power, so the sum stays as loud).
			const float From = Sound.Cue->FarFromM;
			const float T = FMath::SmoothStep(From, From * 4.0f, Sound.DistanceM);
			Distance *= Sound.Layer == 1 ? FMath::Cos(T * UE_HALF_PI) : FMath::Sin(T * UE_HALF_PI);
		}
		Sound.Cutoff = FMath::Min(Air, 20000.0f / (1.0f + 3.0f * Sound.Occlusion));
		Sound.Volume = bOn && Sound.Clip ? Sound.Cue->Volume * GroupVolume(Sound.Cue->Group) * Sound.Gain * Distance * FMath::Pow(SoundWallGain, static_cast<float>(Sound.Occlusion)) : 0.0f;
		if (Sound.Volume > 0.001f)
		{
			Audible.Add(I);
		}
		else if (!bLoop && Now - Sound.StartedAt > 0.1)
		{
			UE_CLOG(GSoundDebug.GetValueOnGameThread() >= 2 && Sound.Voice < 0 && bOn, LogTemp, Log, TEXT("Sound: %s not played: out of hearing (%.0f m of %.0f)"),
				*Sound.Cue->Name, Sound.DistanceM, Sound.Cue->RangeM);
			StopSound(I);  // a one-shot out of hearing is not worth keeping
		}
		else if (Sound.Voice >= 0)
		{
			FreeVoice(Sound.Voice);
		}
	}
	// The most important first: lowest priority number, then the loudest from here.
	Audible.Sort([this](int32 A, int32 B)
	{
		const FSound& SA = Sounds[A];
		const FSound& SB = Sounds[B];
		if (SA.Cue->Priority != SB.Cue->Priority)
		{
			return SA.Cue->Priority < SB.Cue->Priority;
		}
		return SA.Volume > SB.Volume;
	});
	// In a battle, the twentieth rifle shot adds nothing the nearest six don't: past that many of one cue, the farther
	// ones go to the back of the queue, so a scream or a nearby clang still gets a voice.
	{
		const int32 SameCue = FMath::Max(1, GSoundSameCue.GetValueOnGameThread());
		TMap<const FCue*, int32> PerCue;
		TArray<int32> Ranked;
		TArray<int32> Crowded;
		Ranked.Reserve(Audible.Num());
		for (const int32 I : Audible)
		{
			int32& N = PerCue.FindOrAdd(Sounds[I].Cue);
			(++N <= SameCue || Sounds[I].Cue->bLoop ? Ranked : Crowded).Add(I);
		}
		Ranked.Append(Crowded);
		Audible = MoveTemp(Ranked);
	}
	const int32 Budget = FMath::Min(GetVoiceBudget(), Voices.Num());
	for (int32 Rank = Budget; Rank < Audible.Num(); ++Rank)
	{
		FSound& Sound = Sounds[Audible[Rank]];
		if (!Sound.Cue->bLoop)
		{
			// A one-shot that lost its voice, or never got one, is gone; a loop waits without one.
			if (Sound.Voice >= 0 || Now - Sound.StartedAt <= Delta + 0.001)
			{
				++Dropped;
				UE_CLOG(GSoundDebug.GetValueOnGameThread() >= 2, LogTemp, Log, TEXT("Sound: %s dropped: %d more important sounds playing (%.0f m)"),
					*Sound.Cue->Name, Budget, Sound.DistanceM);
			}
			StopSound(Audible[Rank]);
		}
		else if (Sound.Voice >= 0)
		{
			FreeVoice(Sound.Voice);
		}
	}
	int32 FreeAt = 0;
	for (int32 Rank = 0; Rank < FMath::Min(Budget, Audible.Num()); ++Rank)
	{
		FSound& Sound = Sounds[Audible[Rank]];
		if (Sound.Voice < 0)
		{
			while (FreeAt < Voices.Num() && (Voices[FreeAt].Sound >= 0 || Voices[FreeAt].BusyUntil > Now))
			{
				++FreeAt;
			}
			if (FreeAt >= Voices.Num())
			{
				break;
			}
			StartVoice(FreeAt, Audible[Rank]);
			continue;
		}
		UAudioComponent* Component = Voices[Sound.Voice].Component;
		Component->SetWorldLocation(Sound.Location);
		Component->SetVolumeMultiplier(Sound.Volume);
		if (Sound.bLivePitch)
		{
			Component->SetPitchMultiplier(Sound.Pitch);
		}
		SoundApplyFilter(Component, Sound.Cutoff);
	}
	MaxVoicesInUse = FMath::Max(MaxVoicesInUse, GetVoicesInUse());
	// Drop the finished sounds from the list (keys point at indices, so rebuild them).
	if (Sounds.Num() > 64)
	{
		TArray<FSound> Kept;
		Kept.Reserve(Sounds.Num());
		LoopsByKey.Reset();
		for (int32 V = 0; V < Voices.Num(); ++V)
		{
			Voices[V].Sound = -1;
		}
		for (TPair<uint32, FPawnVoice>& Voice : PawnVoices)
		{
			Voice.Value.Sound = -1;
		}
		for (FSound& Sound : Sounds)
		{
			if (Sound.bDead)
			{
				continue;
			}
			const int32 Index = Kept.Add(MoveTemp(Sound));
			if (Kept[Index].Voice >= 0)
			{
				Voices[Kept[Index].Voice].Sound = Index;
			}
			if (Kept[Index].Key != 0)
			{
				LoopsByKey.Add(Kept[Index].Key, Index);
			}
			if (Kept[Index].VoiceOf != colony::kNoEntity)
			{
				PawnVoices.FindOrAdd(SoundEntityId(Kept[Index].VoiceOf)).Sound = Index;
			}
		}
		Sounds = MoveTemp(Kept);
	}
}

void USoundSubsystem::GetVoices(TArray<FSoundVoiceInfo>& Out) const
{
	Out.Reset();
	for (const FSound& Sound : Sounds)
	{
		if (Sound.bDead)
		{
			continue;
		}
		FSoundVoiceInfo Info;
		Info.Cue = Sound.Cue->Name;
		Info.bLoop = Sound.Cue->bLoop;
		Info.bHasVoice = Sound.Voice >= 0;
		Info.Volume = Sound.Volume;
		Info.DistanceM = Sound.DistanceM;
		Info.Occlusion = Sound.Occlusion;
		Info.Location = Sound.Location;
		Out.Add(Info);
	}
}

int32 USoundSubsystem::RenderTest(const FString& Cue, int32 Samples)
{
	const FCue* Found = FindCue(Cue);
	if (!Found)
	{
		return 0;
	}
	UCueVoiceWave* Wave = NewObject<UCueVoiceWave>(this);
	Wave->Setup(Found->Clips.Num() > 0 ? Found->Clips[0] : Found->Tone, false, 0);
	TArray<uint8> Bytes;
	const int32 Made = Wave->OnGeneratePCMAudio(Bytes, Samples);
	const int16* Data = reinterpret_cast<const int16*>(Bytes.GetData());
	int32 Loud = 0;
	for (int32 I = 0; I < Made; ++I)
	{
		Loud += FMath::Abs(Data[I]) > 64 ? 1 : 0;
	}
	return Loud;
}

// ---- Music ----------------------------------------------------------------------------------------------------------

void USoundSubsystem::UpdateMusic(const colony::Snapshot& Snap, float Delta)
{
	if (!MusicComponent)
	{
		return;
	}
	// How dangerous it is: raiders on the map or the alarm make it tense, shooting makes it combat.
	MoodCheckAge += Delta;
	if (MoodCheckAge >= 1.0f)
	{
		MoodCheckAge = 0.0f;
		int32 Danger = bAlarmOn ? 1 : 0;
		for (const colony::ThingView& View : Snap.things)
		{
			if (View.category != colony::ThingCategory::Pawn || View.dead || View.downed)
			{
				continue;
			}
			const bool bHostile = View.faction != 1 && View.faction != 100 && !View.visitor && !View.prisoner && View.squad != 0;
			if (bHostile)
			{
				Danger = FMath::Max(Danger, 1);
			}
			if ((bHostile || View.faction == 1) && View.fighting && View.fightTarget != colony::kNoEntity)
			{
				Danger = 2;
			}
		}
		// Up at once; down only after it has been quieter for a while.
		if (Danger >= WantedMood)
		{
			WantedMood = Danger;
			MoodHeld = 0.0f;
		}
		else
		{
			MoodHeld += 1.0f;
			if (MoodHeld >= 20.0f)
			{
				WantedMood = Danger;
				MoodHeld = 0.0f;
			}
		}
	}
	const float Volume = GSoundEnable.GetValueOnGameThread() != 0 ? GroupVolume(ESoundMix::Music) * 0.7f : 0.0f;
	const bool bPlaying = MusicClip.IsValid();
	if (bPlaying && WantedMood != MusicMood)
	{
		bMusicFadingOut = true;
	}
	if (bPlaying)
	{
		MusicFade = bMusicFadingOut ? FMath::Max(0.0f, MusicFade - Delta / 3.0f) : FMath::Min(1.0f, MusicFade + Delta / 2.0f);
		MusicComponent->SetVolumeMultiplier(FMath::Max(0.0001f, MusicFade * Volume));
		if ((bMusicFadingOut && MusicFade <= 0.0f) || (MusicMood == 0 && Now >= MusicEndsAt))
		{
			MusicComponent->Stop();
			MusicComponent->SetSound(nullptr);
			MusicClip.Reset();
			MusicTrackName.Reset();
			bMusicFadingOut = false;
			// Calm tracks leave quiet between them; danger starts at once.
			NextTrackAt = Now + (WantedMood == 0 ? Random.FRandRange(45.0f, 150.0f) : 0.5f);
		}
	}
	else
	{
		MusicMood = WantedMood;
		int32 Folder = MusicMood;
		if (Folder == 2 && MusicFiles[2].Num() == 0)
		{
			Folder = 1;  // no combat tracks yet: the tense ones play
		}
		if (Now >= NextTrackAt && MusicFiles[Folder].Num() > 0 && Volume > 0.0f)
		{
			int32 Pick = Random.RandHelper(MusicFiles[Folder].Num());
			if (MusicFiles[Folder].Num() > 1 && Pick == LastTrack)
			{
				Pick = (Pick + 1) % MusicFiles[Folder].Num();
			}
			LastTrack = Pick;
			MusicClip = SoundLoadWav(MusicFiles[Folder][Pick]);
			if (MusicClip)
			{
				UCueVoiceWave* Wave = NewObject<UCueVoiceWave>(this);
				Wave->Setup(MusicClip, MusicMood != 0, 0);
				MusicComponent->bAllowSpatialization = false;
				MusicComponent->SetSound(Wave);
				MusicFade = 0.0f;
				MusicComponent->SetVolumeMultiplier(0.0001f);
				MusicComponent->Play();
				MusicEndsAt = Now + MusicClip->Seconds();
				MusicTrackName = FPaths::GetCleanFilename(MusicFiles[Folder][Pick]);
				MusicTrackName = FString(SoundMoodFolders[Folder]) / MusicTrackName;
			}
			else
			{
				NextTrackAt = Now + 30.0;
			}
		}
	}
	// A letter gets a short stinger over the music.
	if (bStingerWanted)
	{
		bStingerWanted = false;
		if (MusicFiles[3].Num() > 0 && Volume > 0.0f && Now - LastStingerAt > 4.0)
		{
			StingerClip = SoundLoadWav(MusicFiles[3][Random.RandHelper(MusicFiles[3].Num())]);
			if (StingerClip)
			{
				UCueVoiceWave* Wave = NewObject<UCueVoiceWave>(this);
				Wave->Setup(StingerClip, false, 0);
				StingerComponent->Stop();
				StingerComponent->bAllowSpatialization = false;
				StingerComponent->SetSound(Wave);
				StingerComponent->SetVolumeMultiplier(Volume * 0.8f);
				StingerComponent->Play();
				StingerEndsAt = Now + StingerClip->Seconds() + 0.1;
				LastStingerAt = Now;
				++Stingers;
			}
		}
	}
	if (StingerClip && Now >= StingerEndsAt)
	{
		StingerComponent->Stop();
		StingerComponent->SetSound(nullptr);
		StingerClip.Reset();
	}
}

// ---- Mixer panel ----------------------------------------------------------------------------------------------------

void USoundSubsystem::DrawMixerBody(float Scale)
{
	ImGui::PushItemWidth(260.0f * Scale);
	bool bChanged = false;
	float Master = GSoundMaster.GetValueOnGameThread();
	if (ImGui::SliderFloat("Master", &Master, 0.0f, 1.0f, "%.2f"))
	{
		GSoundMaster.AsVariable()->Set(Master, ECVF_SetByCode);
		bChanged = true;
	}
	const char* Labels[] = {"Effects", "Ambience", "Voices", "Music"};
	for (int32 G = 0; G < static_cast<int32>(ESoundMix::Count); ++G)
	{
		float Value = SoundGroupVar(static_cast<ESoundMix>(G)).GetValueOnGameThread();
		if (ImGui::SliderFloat(Labels[G], &Value, 0.0f, 1.0f, "%.2f"))
		{
			SoundGroupVar(static_cast<ESoundMix>(G)).AsVariable()->Set(Value, ECVF_SetByCode);
			bChanged = true;
		}
	}
	ImGui::Text("%d of %d voices in use", GetVoicesInUse(), GetVoiceBudget());
	if (!MusicTrackName.IsEmpty())
	{
		ImGui::Text("Music: %s", TCHAR_TO_UTF8(*MusicTrackName));
	}
	ImGui::PopItemWidth();
	if (bChanged)
	{
		SaveMixer();
	}
}
