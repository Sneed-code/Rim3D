#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Sound.h"

#include <atomic>
#include <vector>

#include "SoundSubsystem.generated.h"

class UAudioComponent;
class USimSubsystem;
class USoundAttenuation;
namespace colony { class Sim; struct Event; struct Snapshot; }

/** A sound in memory: 16-bit samples, interleaved when it has two channels. */
struct FSoundClip
{
	TArray<int16> Samples;
	int32 Channels = 1;
	int32 SampleRate = 44100;
	float Seconds() const { return Channels > 0 && SampleRate > 0 ? Samples.Num() / float(Channels * SampleRate) : 0.0f; }
};

/** The mixer's groups, each with a slider (docs/design/sound.md). */
enum class ESoundMix : uint8
{
	Effects,
	Ambience,
	Voices,
	Music,
	Count,
};

/** Plays one clip, once or looped, from the audio thread. One for each voice playing. */
UCLASS()
class RIM3D_API UCueVoiceWave : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	/** StartSample counts frames (a sample per channel); a loop wraps it. */
	void Setup(TSharedPtr<const FSoundClip> InClip, bool bInLoop, int64 StartSample);
	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;

	/** Samples handed to the audio device so far (the smoke test calls OnGeneratePCMAudio itself). */
	int64 GetSamplesOut() const { return SamplesOut.load(); }

private:
	TSharedPtr<const FSoundClip> Clip;
	bool bLoop = false;
	std::atomic<int64> Cursor{0};
	std::atomic<int64> SamplesOut{0};
};

/** One thing being played or waiting for a voice, for the smoke test and the debug readout. */
struct FSoundVoiceInfo
{
	FString Cue;
	bool bLoop = false;
	bool bHasVoice = false;
	float Volume = 0.0f;
	float DistanceM = 0.0f;
	int32 Occlusion = 0;
	FVector Location = FVector::ZeroVector;
};

/**
 * Sound (task V20, docs/design/sound.md). The sim says what happened; this plays it. Events go through the sim's
 * `soundForEvent` (Sound.h) and come out as cue names, the `SoundCues` defs. The snapshot gives the rest each frame:
 * footsteps by the ground under the foot, doors opening and shutting, running machines and fires, animals' calls,
 * and the ambience beds by biome, time of day, roof and weather, with the weather's loop on top.
 *
 * - **Voices:** at most `Sound.Voices` (32) at once. A new sound that finds them all taken pushes out the least
 *   important one (the def's `priority`, then the farthest); loops out of range give their voice back and pick up
 *   where they would be when they come back in range.
 * - **Distance and occlusion:** heard up to the cue's `range` in metres (0: everywhere), from the ears: the
 *   overseer camera's focus on the ground, or the camera in direct mode. Walls on the line between and a roof over
 *   one end but not the other muffle it (quieter, low-pass), read from the sim's cells.
 * - **Sounds:** the cut files in game/Content/ThirdParty/Sounds/cues/<Cue>/ (tools/sound_assets.py, V21), a random
 *   one each time with a little pitch change; a cue with no file plays its def's placeholder `tone`.
 * - **Music (V21):** Sounds/music/calm, tense and combat follow the danger: raiders on the map or the alarm make it
 *   tense, a fight makes it combat; calm tracks play with quiet gaps between. Letters get a stinger.
 * - **Mixer:** master, effects, ambience, voices and music, saved in the user settings; the panel is folded under the
 *   overseer's settings.
 *
 * Console: Sound.Enable, Sound.Master, Sound.Effects, Sound.Ambience, Sound.VoicesVolume, Sound.Music, Sound.Voices
 * (the voice budget), Sound.Tones (0 no placeholder tones, 1 for short sounds only, 2 for loops too), Sound.Debug.
 */
UCLASS()
class RIM3D_API USoundSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Plays a cue now, at a place in world cm (or heard everywhere with bEverywhere). False if it is not a cue. */
	bool PlayCue(const FString& Cue, const FVector& Location, bool bEverywhere = false);

	/** The mixer panel (sliders), drawn by the overseer under its settings. */
	void DrawMixerPanel(float ViewportRight, float Scale);

	// --- For the smoke test ---

	int32 GetCueCount() const { return Cues.Num(); }
	/** Cues with a real sound file (the rest play tones). */
	int32 GetCuesWithFiles() const;
	int32 GetMusicTracks(int32 Mood) const;
	bool HasCue(const FString& Cue) const { return Cues.Contains(Cue); }
	/** Sounds asked for since the sim started, by cue. */
	int32 GetPlayCount(const FString& Cue) const { const int32* N = PlayCounts.Find(Cue); return N ? *N : 0; }
	int32 GetPlaysTotal() const { return PlaysTotal; }
	/** Sounds that were pushed out of, or never got, a voice. */
	int32 GetDropped() const { return Dropped; }
	int32 GetVoicesInUse() const;
	int32 GetMaxVoicesInUse() const { return MaxVoicesInUse; }
	int32 GetVoiceBudget() const;
	void GetVoices(TArray<FSoundVoiceInfo>& Out) const;
	/** The ambience bed and the weather loop playing now ("" for none). */
	const FString& GetBed() const { return BedCue; }
	const FString& GetWeatherLoop() const { return WeatherCue; }
	/** 0 calm, 1 tense, 2 combat, and the track playing ("" between tracks). */
	int32 GetMusicMood() const { return MusicMood; }
	const FString& GetMusicTrack() const { return MusicTrackName; }
	/** Stingers played for letters. */
	int32 GetStingers() const { return Stingers; }
	/** Walls and roofs between a place (world cm) and the ears, as the occlusion counts them. */
	int32 OcclusionAt(const FVector& Location) const;
	FVector GetEars() const { return Ears; }
	/** Makes a test clip of a cue's tone (or its first file) and returns how many samples a voice produced from it. */
	int32 RenderTest(const FString& Cue, int32 Samples);
	/** Sets the mixer as the panel does (the smoke test checks it is saved). */
	void SetGroupVolume(ESoundMix Group, float Volume);
	float GetGroupVolume(ESoundMix Group) const;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FCue
	{
		FString Name;
		bool bLoop = false;
		float Volume = 0.5f;
		float RangeM = 30.0f;
		int32 Priority = 2;
		ESoundMix Group = ESoundMix::Effects;
		/** The cut files; empty plays the tone. */
		TArray<TSharedPtr<const FSoundClip>> Clips;
		TSharedPtr<const FSoundClip> Tone;
	};

	/** Something that should be heard: a one-shot until its clip ends, or a loop until stopped. */
	struct FSound
	{
		const FCue* Cue = nullptr;
		/** Sim-side identity of a loop (entity, cell, or a name for beds); 0 for one-shots. */
		uint64 Key = 0;
		FVector Location = FVector::ZeroVector;
		colony::Entity Follow = colony::kNoEntity;
		bool bEverywhere = false;
		float Gain = 1.0f;      // beds and weather fade with this
		float Pitch = 1.0f;
		double EndsAt = 0.0;    // one-shots: real seconds
		double StartedAt = 0.0;
		float DistanceM = 0.0f;
		int32 Occlusion = 0;
		float OcclusionAge = 99.0f;
		float Volume = 0.0f;
		int32 Voice = -1;
		float TargetGain = 1.0f;
		bool bDead = false;
		/** Kept alive by the snapshot (machines, beds), not by events: stops when a scan no longer sees it. */
		bool bScanned = false;
		bool bSeen = false;
		TSharedPtr<const FSoundClip> Clip;
	};

	struct FVoice
	{
		UAudioComponent* Component = nullptr;  // held by Components
		int32 Sound = -1;
	};

	/** A def's `visual.sound` slots that the snapshot uses, read once. */
	struct FDefSounds
	{
		FString Open;
		FString Close;
		FString Loop;
		FString Idle;
		FString Step;
	};

	void LoadCues();
	void LoadMusic();
	void SaveMixer() const;
	void OnSimEvent(const colony::Event& Event);
	void HandlePlay(const colony::SoundPlay& Play);
	int32 AddSound(const FCue& Cue, const FVector& Location, bool bEverywhere, colony::Entity Follow, uint64 Key);
	void StopSound(int32 Index);
	void ResetForSim();
	void UpdateEars();
	void ReadSnapshot(const USimSubsystem& Bridge, float Delta);
	void UpdateSteps(const colony::Sim& Sim, const colony::Snapshot& Snap, const USimSubsystem& Bridge, float Delta);
	void UpdateLoops(const colony::Snapshot& Snap, const USimSubsystem& Bridge);
	void UpdateBeds(const colony::Sim& Sim, const colony::Snapshot& Snap, float Delta);
	void UpdateMusic(const colony::Snapshot& Snap, float Delta);
	void KeepLoop(const FString& CueName, uint64 Key, const FVector& Location, colony::Entity Follow, bool bEverywhere, float Gain);
	void MixAndAssign(const USimSubsystem* Bridge, float Delta);
	void StartVoice(int32 VoiceIndex, int32 SoundIndex);
	void FreeVoice(int32 VoiceIndex);
	UAudioComponent* MakeComponent();
	const FCue* FindCue(const FString& Name) const { return Cues.Find(Name); }
	colony::DefId DefOfEntity(colony::Entity Thing) const;
	bool EntityLocation(colony::Entity Thing, FVector& Out) const;
	float GroupVolume(ESoundMix Group) const;
	TSharedPtr<const FSoundClip> PickClip(const FCue& Cue);
	const FDefSounds& SoundsOfDef(colony::DefId Def);
	FString StepCueAt(const colony::Sim& Sim, const colony::CellPos& Cell);

	UPROPERTY()
	TObjectPtr<AActor> Holder;

	UPROPERTY()
	TObjectPtr<USoundAttenuation> Spatial;

	UPROPERTY()
	TArray<TObjectPtr<UAudioComponent>> Components;

	TMap<FString, FCue> Cues;
	TArray<FSound> Sounds;
	TArray<FVoice> Voices;
	TMap<uint64, int32> LoopsByKey;
	std::vector<colony::SoundPlay> PendingPlays;
	TMap<FString, int32> PlayCounts;
	TWeakObjectPtr<USimSubsystem> Bridge;
	FDelegateHandle EventHandle;
	FRandomStream Random = FRandomStream(0x50D);

	FVector Ears = FVector::ZeroVector;
	colony::CellPos EarCell;
	bool bEarsOnMap = false;
	int32 SeenGeneration = -1;
	int32 PlaysTotal = 0;
	int32 Dropped = 0;
	int32 MaxVoicesInUse = 0;
	double Now = 0.0;

	// Snapshot-driven sounds, by entity number.
	TMap<uint32, float> StepDistance;
	TMap<uint32, FVector> LastStepPos;
	TMap<uint32, float> DoorOpen;
	TMap<uint32, float> NextCall;
	TMap<uint32, FDefSounds> DefSounds;
	TMap<int32, FString> GroundStep;
	float LoopScanAge = 99.0f;
	bool bAlarmOn = false;

	// Ambience.
	FString BedCue;
	FString WeatherCue;
	int32 SeenFlashes = -1;
	TArray<double> ThunderAt;

	// Music.
	TArray<TArray<FString>> MusicFiles;  // by mood (0 calm, 1 tense, 2 combat, 3 stinger)
	int32 MusicMood = 0;
	int32 WantedMood = 0;
	float MoodHeld = 0.0f;
	float MoodCheckAge = 99.0f;
	FString MusicTrackName;
	TSharedPtr<const FSoundClip> MusicClip;
	float MusicFade = 0.0f;
	double MusicEndsAt = 0.0;
	double NextTrackAt = 10.0;
	bool bMusicFadingOut = false;
	int32 LastTrack = -1;
	TSharedPtr<const FSoundClip> StingerClip;
	bool bStingerWanted = false;
	double StingerEndsAt = 0.0;
	double LastStingerAt = -99.0;
	int32 Stingers = 0;

	UPROPERTY()
	TObjectPtr<UAudioComponent> MusicComponent;

	UPROPERTY()
	TObjectPtr<UAudioComponent> StingerComponent;
};
