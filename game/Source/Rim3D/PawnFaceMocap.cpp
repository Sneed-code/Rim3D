#include "PawnFaceMocap.h"

#include "Dom/JsonObject.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogFaceMocap, Log, All);

const FPawnFaceMocapLibrary& FPawnFaceMocapLibrary::Get()
{
	static FPawnFaceMocapLibrary Library;
	static bool bLoaded = false;
	if (!bLoaded)
	{
		bLoaded = true;
		Library.Load();
	}
	return Library;
}

void FPawnFaceMocapLibrary::Load()
{
	for (float& Q : Quiet)
	{
		Q = 1.0f;
	}
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../assets/face_mocap"));
	FString ClipsText;
	FString MapText;
	if (!FFileHelper::LoadFileToString(ClipsText, *(Folder / TEXT("clips.json"))) || !FFileHelper::LoadFileToString(MapText, *(Folder / TEXT("arkit_map.json"))))
	{
		UE_LOG(LogFaceMocap, Warning, TEXT("No facial capture in %s: faces stay still apart from fight faces"), *Folder);
		return;
	}
	TSharedPtr<FJsonObject> ClipsJson;
	TSharedPtr<FJsonObject> MapJson;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ClipsText), ClipsJson) || !ClipsJson.IsValid() ||
		!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MapText), MapJson) || !MapJson.IsValid())
	{
		UE_LOG(LogFaceMocap, Warning, TEXT("Facial capture files in %s could not be read"), *Folder);
		return;
	}
	Fps = static_cast<float>(ClipsJson->GetNumberField(TEXT("fps")));
	for (const TSharedPtr<FJsonValue>& Name : ClipsJson->GetArrayField(TEXT("channels")))
	{
		ChannelNames.Add(Name->AsString());
	}
	if (ChannelNames.Num() != Channels || Fps <= 0.0f)
	{
		UE_LOG(LogFaceMocap, Warning, TEXT("clips.json has %d curves at %.0f fps (wanted %d)"), ChannelNames.Num(), Fps, Channels);
		ChannelNames.Reset();
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : ClipsJson->GetArrayField(TEXT("clips")))
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid())
		{
			continue;
		}
		FClip Clip;
		Clip.Name = Object->GetStringField(TEXT("name"));
		Clip.Emotion = FName(*Object->GetStringField(TEXT("emotion")));
		Clip.Frames = static_cast<int32>(Object->GetNumberField(TEXT("frames")));
		if (!FBase64::Decode(Object->GetStringField(TEXT("data")), Clip.Data) || Clip.Frames < 2 || Clip.Data.Num() != Clip.Frames * Channels)
		{
			UE_LOG(LogFaceMocap, Warning, TEXT("Clip %s is broken (%d bytes for %d frames)"), *Clip.Name, Clip.Data.Num(), Clip.Frames);
			continue;
		}
		ByEmotion.FindOrAdd(Clip.Emotion).Add(Clips.Num());
		Clips.Add(MoveTemp(Clip));
	}
	const TSharedPtr<FJsonObject>* Map = nullptr;
	if (MapJson->TryGetObjectField(TEXT("map"), Map))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Curve : (*Map)->Values)
		{
			const int32 Channel = ChannelNames.IndexOfByKey(Curve.Key);
			const TSharedPtr<FJsonObject> Targets = Curve.Value->AsObject();
			if (Channel == INDEX_NONE || !Targets.IsValid())
			{
				continue;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Target : Targets->Values)
			{
				const int32 Morph = Morphs.AddUnique(FName(*Target.Key));
				Drives[Channel].Add({Morph, static_cast<float>(Target.Value->AsNumber())});
			}
		}
	}
	const TSharedPtr<FJsonObject>* QuietJson = nullptr;
	if (MapJson->TryGetObjectField(TEXT("quiet"), QuietJson))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Curve : (*QuietJson)->Values)
		{
			const int32 Channel = ChannelNames.IndexOfByKey(Curve.Key);
			if (Channel != INDEX_NONE)
			{
				Quiet[Channel] = static_cast<float>(Curve.Value->AsNumber());
			}
		}
	}
	UE_LOG(LogFaceMocap, Display, TEXT("Facial capture: %d clips in %d emotions, %d morphs driven"), Clips.Num(), ByEmotion.Num(), Morphs.Num());
}

void FPawnFacePlayer::Tick(float Delta, FName WantEmotion, bool bWantTalking, FRandomStream& Random)
{
	const FPawnFaceMocapLibrary& Library = FPawnFaceMocapLibrary::Get();
	if (!Library.IsLoaded())
	{
		return;
	}
	const TArray<int32>* Choices = Library.ByEmotion.Find(WantEmotion);
	if (!Choices || Choices->IsEmpty())
	{
		WantEmotion = TEXT("neutral");
		Choices = Library.ByEmotion.Find(WantEmotion);
		if (!Choices || Choices->IsEmpty())
		{
			Choices = &Library.ByEmotion.CreateConstIterator().Value();
		}
	}
	SinceChange += Delta;
	const auto Pick = [&]()
	{
		Clip = (*Choices)[Random.RandRange(0, Choices->Num() - 1)];
		Time = 0.0f;
	};
	if (WantEmotion != Emotion || !Library.Clips.IsValidIndex(Clip))
	{
		Emotion = WantEmotion;
		Pick();
		// Start somewhere inside it: two people of the same mood never move together.
		Time = Random.FRand() * static_cast<float>(Library.Clips[Clip].Frames - 1) / Library.Fps * 0.7f;
		SinceChange = 0.0f;
	}
	if (bWantTalking != bTalking)
	{
		bTalking = bWantTalking;
		SinceChange = FMath::Min(SinceChange, 0.2f);
	}
	Time += Delta;
	const FPawnFaceMocapLibrary::FClip* Playing = &Library.Clips[Clip];
	if (Time * Library.Fps >= static_cast<float>(Playing->Frames - 1))
	{
		Pick();  // the next take of the same emotion; the easing below hides the cut
		SinceChange = FMath::Min(SinceChange, 0.15f);
		Playing = &Library.Clips[Clip];
	}
	const float At = FMath::Clamp(Time * Library.Fps, 0.0f, static_cast<float>(Playing->Frames - 1));
	const int32 Frame = FMath::Min(FMath::FloorToInt32(At), Playing->Frames - 2);
	const float Between = At - static_cast<float>(Frame);
	const uint8* A = Playing->Data.GetData() + Frame * FPawnFaceMocapLibrary::Channels;
	const uint8* B = A + FPawnFaceMocapLibrary::Channels;
	// Eased: quickly (80 ms, smoothing the capture's jitter) once settled, slowly (0.35 s) just after a change of
	// emotion or clip, so a face never jumps.
	const float Tau = SinceChange < 0.6f ? 0.35f : 0.08f;
	const float Ease = bStarted ? 1.0f - FMath::Exp(-Delta / Tau) : 1.0f;
	bStarted = true;
	for (int32 c = 0; c < FPawnFaceMocapLibrary::Channels; ++c)
	{
		const float Raw = FMath::Lerp(static_cast<float>(A[c]), static_cast<float>(B[c]), Between) / 255.0f;
		const float Want = Raw * (bTalking ? 1.0f : Library.Quiet[c]);
		Curves[c] += (Want - Curves[c]) * Ease;
	}
}

void FPawnFacePlayer::AddMorphs(TMap<FName, float>& Out, float Amount) const
{
	const FPawnFaceMocapLibrary& Library = FPawnFaceMocapLibrary::Get();
	if (!Library.IsLoaded() || !bStarted)
	{
		return;
	}
	for (int32 c = 0; c < FPawnFaceMocapLibrary::Channels; ++c)
	{
		if (Curves[c] <= 0.001f)
		{
			continue;
		}
		for (const FPawnFaceMocapLibrary::FDrive& Drive : Library.Drives[c])
		{
			Out.FindOrAdd(Library.Morphs[Drive.Morph]) += Curves[c] * Drive.Weight * Amount;
		}
	}
}
