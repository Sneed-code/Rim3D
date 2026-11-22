#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/JobType.h"
#include "ColonySim/Sound.h"

#include <atomic>
#include <vector>

#include "SoundSubsystem.generated.h"

class UAudioComponent;
class USimSubsystem;
class USoundAttenuation;
class UReverbEffect;
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

	/** A one-shot that has handed its last sample to the audio device (a loop never is). */
	bool IsDone() const { return !bLoop && (!Clip || Cursor.load() >= Clip->Samples.Num()); }

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

	/** Handles a play as if the sim had sent it, on the next tick (the smoke test sends pawn voices this way). */
	void QueuePlay(const colony::SoundPlay& Play) { PendingPlays.push_back(Play); }

	/**
	 * How loud a sound is at a distance, 0 to 1 (docs/design/sound.md "Distance"): full up to `NearM`, then halved with
	 * every doubling of the distance, as sound spreads in open air (-6 dB), and eased to silence over the last 30% of the
	 * cue's range so nothing stops with a jump.
	 */
	static float DistanceGain(float NearM, float RangeM, float DistanceM);
	/** The low-pass for air: far sounds lose their highs first (20 kHz close, about 1.3 kHz at the edge of the range). */
	static float AirCutoff(float RangeM, float DistanceM);

	/** The Sound page of the Settings window: the mixer sliders (inside the open window). */
	void DrawMixerBody(float Scale);

	// --- For the smoke test ---

	int32 GetCueCount() const { return Cues.Num(); }
	/** Cues with a real sound file (the rest play tones). */
	int32 GetCuesWithFiles() const;
	int32 GetMusicTracks(int32 Mood) const;
	bool HasCue(const FString& Cue) const { return Cues.Contains(Cue); }
	/** How many sound files a cue has (0: it plays its tone). */
	int32 GetClipCount(const FString& Cue) const { const FCue* C = Cues.Find(Cue); return C ? C->Clips.Num() : 0; }
	/** Sounds asked for since the sim started, by cue. */
	int32 GetPlayCount(const FString& Cue) const { const int32* N = PlayCounts.Find(Cue); return N ? *N : 0; }
	int32 GetPlaysTotal() const { return PlaysTotal; }
	/** Sounds that really started on a voice (heard, not dropped), whose cue name starts with `Prefix` (e.g. "Shot_"),
	 *  and of those how many played a recorded file rather than a placeholder tone (smoke tests: guns are heard). */
	int32 GetVoicedWithPrefix(const FString& Prefix, bool bFilesOnly) const
	{
		int32 Count = 0;
		for (const TPair<FString, FIntPoint>& Pair : VoicedCounts)
		{
			Count += Pair.Key.StartsWith(Prefix) ? (bFilesOnly ? Pair.Value.Y : Pair.Value.X) : 0;
		}
		return Count;
	}
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
	/** The room the ears are in, for the echo: 0 outdoors, 1 under a built roof, 2 under rock (a cave). */
	int32 GetEnvironment() const { return Environment; }
	/** Pawn voices cut short by a newer voice of the same pawn (a scream over a hit grunt), and voices not played
	 *  (a hit grunt in its cooldown or over a scream). */
	int32 GetVoicesCut() const { return VoicesCut; }
	int32 GetVoicesSkipped() const { return VoicesSkipped; }
	/** Lovin sounds started, bird calls played (and their seconds today). */
	int32 GetLovinPlays() const { return LovinPlays; }
	int32 GetBirdPlays() const { return BirdPlays; }
	float GetBirdSecondsPlayed() const { return BirdSecondsPlayed; }
	/** Ctrl+M and Ctrl+S (also Sound.MusicMuted, Sound.Enable). */
	bool IsMusicMuted() const;
	bool IsAllMuted() const;
	void ToggleMusicMute();
	void ToggleAllMute();
	/** For the smoke test: start a lovin's sound now, as the Lovin event would after its lead-in. */
	void StartLovinForTest(colony::Entity A, colony::Entity B);
	/** Whether a pawn's voice sound is playing now, and which cue. */
	FString GetPawnVoice(colony::Entity Pawn) const;
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
		/** Full volume up to here; beyond it -6 dB for every doubling of the distance. */
		float NearM = 2.0f;
		/** Pawn voices: seconds before the same pawn can make this sound again (a hit grunt's 5). */
		float CooldownS = 0.0f;
		int32 Priority = 2;
		ESoundMix Group = ESoundMix::Effects;
		/** A shot's or a blast's distant layer (`far`): past `farFrom` metres it takes over from the near sound, fully at
		 * four times that, so a far gunshot is the low boom and rolling echo, not a quiet crack. */
		FString FarCue;
		float FarFromM = 0.0f;
		/** Work: seconds between plays while the same pawn keeps at the same job, standing (`repeat`; 0 plays once). */
		float RepeatS = 0.0f;
		/** Downed voices: seconds between one and the next from the same pawn, a random pick in [min, max] (`every`). */
		FVector2f EveryS = FVector2f::ZeroVector;
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
		/** When it is heard (real seconds): later than asked by the distance over the speed of sound (343 m/s). */
		double StartedAt = 0.0;
		/** 0 the whole sound; 1 the near and 2 the far layer of a cue with a `far` layer (they crossfade by distance). */
		uint8 Layer = 0;
		float DistanceM = 0.0f;
		int32 Occlusion = 0;
		float OcclusionAge = 99.0f;
		float Volume = 0.0f;
		/** Low-pass for distance and walls together, Hz (20000: none). */
		float Cutoff = 20000.0f;
		int32 Voice = -1;
		/** One-shots: when the clip had handed out its last sample (the device still has a little of it to play). */
		double DoneAt = -1.0;
		/** A pawn's own voice: the pawn, and whether it is a hit grunt or a scream. */
		colony::Entity VoiceOf = colony::kNoEntity;
		colony::SoundVoice VoiceKind = colony::SoundVoice::None;
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
		UCueVoiceWave* Wave = nullptr;          // held by the component while it plays
		int32 Sound = -1;
		/** A voice fading out after a cut is not handed to a new sound until then. */
		double BusyUntil = 0.0;
	};

	/** The sound a pawn's voice is making, and when its last hit grunt started (for the cooldown). */
	struct FPawnVoice
	{
		int32 Sound = -1;
		double LastHitAt = -1.0e9;
	};

	/** A def's `visual.sound` slots that the snapshot uses, read once. */
	struct FDefSounds
	{
		FString Open;
		FString Close;
		FString Loop;
		FString Idle;
		FString Step;
		/** Projectiles: what it sounds like passing the ears (`flyby`), and a shell's whistle as it comes down (`incoming`). */
		FString Flyby;
		FString Incoming;
	};

	/** A work sound repeating while its pawn works (sound.md "Every weapon, surface and job"). */
	struct FWorkBeat
	{
		const FCue* Cue = nullptr;
		colony::JobType Job = colony::JobType::None;
		FVector Start = FVector::ZeroVector;
		double NextAt = 0.0;
		double Until = 0.0;
	};

	/** A projectile watched for its closest pass by the ears (a bullet's whiz) or its last seconds down (a shell). */
	struct FFlyWatch
	{
		float LastDistM = 1.0e9f;
		bool bWasFar = false;
		bool bDone = false;
		uint32 SeenFrame = 0;
	};

	void LoadCues();
	void LoadMusic();
	void SaveMixer() const;
	void OnSimEvent(const colony::Event& Event);
	void HandlePlay(const colony::SoundPlay& Play);
	void PlayVoice(const FCue& Cue, const colony::SoundPlay& Play, const FVector& Location);
	/** Stops a sound with a fade (40 ms: no click), leaving its voice to finish the fade. */
	void CutSound(int32 Index, float FadeSeconds = 0.04f);
	void UpdateLovin();
	void UpdateBirds(const colony::Sim& Sim, const colony::Snapshot& Snap);
	void UpdateMuteKeys();
	void UpdateEnvironment(const colony::Sim& Sim);
	USoundAttenuation* SpatialFor(const FCue& Cue);
	colony::SoundGender GenderOf(colony::Entity Thing) const;
	int32 AddSound(const FCue& Cue, const FVector& Location, bool bEverywhere, colony::Entity Follow, uint64 Key, double Delay = 0.0);
	void UpdateWorkBeats(const colony::Snapshot& Snap);
	void UpdateFlybys(const colony::Snapshot& Snap, const USimSubsystem& Source);
	/** The pawn the player walks as in direct mode (its own steps are not heard), else none. */
	colony::Entity ControlledPawn() const;
	void StopSound(int32 Index);
	void ResetForSim();
	void UpdateEars();
	void ReadSnapshot(const USimSubsystem& Bridge, float Delta);
	void UpdateSteps(const colony::Sim& Sim, const colony::Snapshot& Snap, const USimSubsystem& Bridge, float Delta);
	void UpdateLoops(const colony::Snapshot& Snap, const USimSubsystem& Source);
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

	/** Attenuation by cue range (rounded metres): it sets how much echo a sound sends, more the farther it is. */
	UPROPERTY()
	TMap<int32, TObjectPtr<USoundAttenuation>> SpatialByRange;

	/** The echo of the place the ears are in: outdoors, a room, a cave. */
	UPROPERTY()
	TArray<TObjectPtr<UReverbEffect>> Reverbs;

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
	int32 VoicesCut = 0;
	int32 VoicesSkipped = 0;

	/** A couple (or one) having lovin: its sound starts a little into it and fades when it ends. */
	struct FLovin
	{
		colony::Entity A = colony::kNoEntity;
		colony::Entity B = colony::kNoEntity;
		double StartedAt = 0.0;    // real seconds
		double PlayAt = 0.0;       // real seconds
		uint64 Key = 0;            // its sound, by key (indices move)
		bool bPlayed = false;
	};
	TArray<FLovin> Lovins;
	int32 LovinPlays = 0;

	// Birds: short bursts at dawn, dusk and before rain, under 2% of the day, not every day.
	int64 BirdDay = -1;
	bool bBirdDay = false;
	float BirdSecondsLeft = 0.0f;
	double NextBirdCheck = 0.0;
	double BirdBurstUntil = -1.0;
	int32 BirdPlays = 0;
	float BirdSecondsPlayed = 0.0f;
	TMap<FString, FIntPoint> VoicedCounts;  // by cue: started on a voice (X), of those from a recorded file (Y)
	FString LastWeatherLoop;
	int32 Environment = -1;
	TMap<uint32, FPawnVoice> PawnVoices;
	double Now = 0.0;

	// Snapshot-driven sounds, by entity number.
	TMap<uint32, float> StepDistance;
	TMap<uint32, FVector> LastStepPos;
	TMap<uint32, float> DoorOpen;
	TMap<uint32, float> NextCall;
	/** Pawns lying downed and awake: seconds to their next moan or plea (docs/design/sound.md "Downed voices"). */
	TMap<uint32, float> NextMoan;
	TMap<uint32, FDefSounds> DefSounds;
	TMap<int32, FString> GroundStep;
	TMap<uint32, FWorkBeat> WorkBeats;
	TMap<uint32, FFlyWatch> FlyWatch;
	uint32 FlyFrame = 0;
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
