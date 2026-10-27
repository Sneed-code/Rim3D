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
#include "Sound/SoundAttenuation.h"

#include "OverseerPawn.h"
#include "SimSubsystem.h"
#include "SkySubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Sound.h"

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
	TAutoConsoleVariable<int32> GSoundVoices(TEXT("Sound.Voices"), 32, TEXT("How many sounds play at once (the voice budget)."));
	TAutoConsoleVariable<int32> GSoundTones(TEXT("Sound.Tones"), 1,
		TEXT("Placeholder tones for cues with no sound file: 0 none, 1 short sounds only, 2 loops too."));
	TAutoConsoleVariable<int32> GSoundDebug(TEXT("Sound.Debug"), 0, TEXT("1 lists the sounds playing on screen."));

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
		if (Cue.StartsWith(TEXT("Pawn_")) || Cue.StartsWith(TEXT("Animal_")) || Cue == TEXT("Work_Chat"))
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
	const FString CueFolder = FPaths::Combine(SoundFolder(), TEXT("cues"));
	int32 FileCount = 0;
	for (colony::DefId Id : Defs->idsOfKind("SoundCues"))
	{
		const colony::Def& Def = Defs->get(Id);
		FCue Cue;
		Cue.Name = UTF8_TO_TCHAR(Def.defName().c_str());
		Cue.bLoop = Def.flag("loop");
		Cue.Volume = static_cast<float>(Def.number("volume", 0.5));
		Cue.RangeM = static_cast<float>(Def.number("range", 30.0));
		Cue.Priority = static_cast<int32>(Def.integer("priority", 2));
		Cue.Group = SoundGroupOf(Cue.Name);
		Cue.Tone = SoundMakeTone(UTF8_TO_TCHAR(Def.text("tone.wave", "sine").c_str()), static_cast<float>(Def.number("tone.hz", 440.0)),
			static_cast<float>(Def.number("tone.seconds", 0.2)), Cue.bLoop, GetTypeHash(Cue.Name));
		if (const TArray<TSharedPtr<const FSoundClip>>* Have = Files.Find(Cue.Name); Have && !bFirst)
		{
			Cue.Clips = *Have;
		}
		else
		{
			for (const FString& Path : SoundWavsIn(FPaths::Combine(CueFolder, Cue.Name)))
			{
				if (TSharedPtr<const FSoundClip> Clip = SoundLoadWav(Path))
				{
					Cue.Clips.Add(Clip);
				}
			}
		}
		FileCount += Cue.Clips.Num();
		Cues.Add(Cue.Name, MoveTemp(Cue));
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
	StepDistance.Reset();
	LastStepPos.Reset();
	DoorOpen.Reset();
	NextCall.Reset();
	ThunderAt.Reset();
	PlaysTotal = 0;
	Dropped = 0;
	MaxVoicesInUse = 0;
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
	if (Event.type == colony::EventType::AlarmChanged)
	{
		bAlarmOn = Event.message.rfind("on", 0) == 0;
	}
	const colony::SoundDefOf DefOf = [this](colony::Entity E) { return DefOfEntity(E); };
	colony::soundForEvent(*Source->GetDefs(), Event, DefOf, PendingPlays);
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

int32 USoundSubsystem::AddSound(const FCue& Cue, const FVector& Location, bool bEverywhere, colony::Entity Follow, uint64 Key)
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
	Sound.EndsAt = Now + (Sound.Clip ? Sound.Clip->Seconds() / Sound.Pitch : 0.0) + 0.05;
	++PlaysTotal;
	PlayCounts.FindOrAdd(Cue.Name) += 1;
	if (Follow != colony::kNoEntity)
	{
		EntityLocation(Follow, Sound.Location);
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
	AddSound(*Cue, Location, bEverywhere, Play.entity, Key);
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
	for (const colony::SoundPlay& Play : PendingPlays)
	{
		HandlePlay(Play);
	}
	PendingPlays.clear();
	ReadSnapshot(*Source, DeltaTime);
	MixAndAssign(Source, DeltaTime);
	UpdateMusic(Source->GetCurrentSnapshot(), DeltaTime);

	if (GSoundDebug.GetValueOnGameThread() != 0 && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(0x50D0, 0.0f, FColor::Cyan, FString::Printf(TEXT("Sound: %d voices of %d, %d sounds, bed %s, weather %s, music %d %s"),
			GetVoicesInUse(), GetVoiceBudget(), Sounds.Num(), *BedCue, *WeatherCue, MusicMood, *MusicTrackName));
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
	// Walls on the line between (a door counts as open), not the ends themselves.
	const int32 DX = EarCell.x - From.x;
	const int32 DY = EarCell.y - From.y;
	const int32 Steps = FMath::Max(FMath::Abs(DX), FMath::Abs(DY));
	colony::CellPos Last = From;
	for (int32 S = 1; S < Steps && Count < 3; ++S)
	{
		const colony::CellPos C{From.x + FMath::RoundToInt(DX * float(S) / Steps), From.y + FMath::RoundToInt(DY * float(S) / Steps), From.level};
		if ((C.x != Last.x || C.y != Last.y) && Map.inBounds(C) && Map.passage(C) == colony::Passage::Blocked)
		{
			++Count;
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
	for (const colony::ThingView& View : Snap.things)
	{
		if (View.category != colony::ThingCategory::Pawn || View.dead)
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
				if (Walked >= Stride && FVector::DistSquared(Location, Ears) < FMath::Square(HearStepsCm))
				{
					Walked = FMath::Fmod(Walked, Stride);
					const colony::CellPos Cell{FMath::FloorToInt(View.x), FMath::FloorToInt(View.y), View.level};
					if (const FCue* Cue = FindCue(StepCueAt(Sim, Cell)))
					{
						const int32 Index = AddSound(*Cue, Location + FVector(0.0, 0.0, 10.0), false, colony::kNoEntity, 0);
						Sounds[Index].Gain = bAnimal ? FMath::Clamp(View.bodySize, 0.3f, 1.5f) : 1.0f;
					}
				}
			}
		}
		// Animals call now and then (cosmetic, Unreal's own random).
		if (bAnimal && !View.downed && !View.asleep && bNear)
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
		if (!Sound.bDead && Sound.bScanned && !Sound.bSeen && (Sound.Key >> 56) == static_cast<uint64>(ESoundKey::Building))
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
	UAudioComponent* Component = Voice.Component;
	Component->Stop();
	Component->bAllowSpatialization = !Sound.bEverywhere;
	Component->AttenuationSettings = Sound.bEverywhere ? nullptr : Spatial.Get();
	Component->SetWorldLocation(Sound.Location);
	Component->SetSound(Wave);
	Component->SetVolumeMultiplier(Sound.Volume);
	Component->SetPitchMultiplier(Sound.Pitch);
	Component->SetLowPassFilterEnabled(Sound.Occlusion > 0);
	Component->SetLowPassFilterFrequency(20000.0f / (1.0f + 3.0f * Sound.Occlusion));
	Component->Play();
	Voice.Sound = SoundIndex;
	Sound.Voice = VoiceIndex;
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
		const bool bLoop = Sound.Cue->bLoop;
		if (!bLoop && Now >= Sound.EndsAt)
		{
			StopSound(I);
			continue;
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
		if (!Sound.bEverywhere)
		{
			const float Share = FMath::Clamp(1.0f - Sound.DistanceM / FMath::Max(1.0f, Sound.Cue->RangeM), 0.0f, 1.0f);
			Distance = Share * Share;
		}
		Sound.Volume = bOn && Sound.Clip ? Sound.Cue->Volume * GroupVolume(Sound.Cue->Group) * Sound.Gain * Distance * FMath::Pow(0.55f, static_cast<float>(Sound.Occlusion)) : 0.0f;
		if (Sound.Volume > 0.001f)
		{
			Audible.Add(I);
		}
		else if (!bLoop && Now - Sound.StartedAt > 0.1)
		{
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
			while (FreeAt < Voices.Num() && Voices[FreeAt].Sound >= 0)
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
		Component->SetLowPassFilterEnabled(Sound.Occlusion > 0);
		Component->SetLowPassFilterFrequency(20000.0f / (1.0f + 3.0f * Sound.Occlusion));
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

void USoundSubsystem::DrawMixerPanel(float ViewportRight, float Scale)
{
	ImGui::SetNextWindowPos(ImVec2(ViewportRight - 12.0f * Scale, 100.0f * Scale), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.7f);
	ImGui::SetNextWindowCollapsed(true, ImGuiCond_Once);
	if (!ImGui::Begin("Sound##SoundMixer", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
	{
		ImGui::End();
		return;
	}
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
	if (bChanged)
	{
		SaveMixer();
	}
	ImGui::End();
}
