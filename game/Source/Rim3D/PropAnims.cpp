#include "PropAnims.h"

#include "Algo/BinarySearch.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogPropAnims, Log, All);

namespace
{
	TMap<FString, TUniquePtr<PropAnims::FSet>>& PropAnimCache()
	{
		static TMap<FString, TUniquePtr<PropAnims::FSet>> Cache;
		return Cache;
	}

	bool PropAnimVec(const TSharedPtr<FJsonValue>& Value, int32 Count, double* Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Array) || Array->Num() != Count)
		{
			return false;
		}
		for (int32 I = 0; I < Count; ++I)
		{
			Out[I] = (*Array)[I]->AsNumber();
		}
		return true;
	}

	FTransform PropAnimKey(const TSharedPtr<FJsonObject>& Key)
	{
		double L[3] = {0.0, 0.0, 0.0};
		double R[4] = {0.0, 0.0, 0.0, 1.0};
		double S[3] = {1.0, 1.0, 1.0};
		PropAnimVec(Key->TryGetField(TEXT("loc")), 3, L);
		PropAnimVec(Key->TryGetField(TEXT("rot")), 4, R);
		PropAnimVec(Key->TryGetField(TEXT("scale")), 3, S);
		return FTransform(FQuat(R[0], R[1], R[2], R[3]).GetNormalized(), FVector(L[0], L[1], L[2]), FVector(S[0], S[1], S[2]));
	}

	bool PropAnimReadTrack(const TSharedPtr<FJsonObject>& Json, PropAnims::FTrack& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Times = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Locs = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Rots = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Scales = nullptr;
		if (!Json->TryGetArrayField(TEXT("t"), Times) || !Json->TryGetArrayField(TEXT("loc"), Locs) || !Json->TryGetArrayField(TEXT("rot"), Rots) ||
			!Json->TryGetArrayField(TEXT("scale"), Scales))
		{
			return false;
		}
		const int32 Count = Times->Num();
		if (Count == 0 || Locs->Num() != Count || Rots->Num() != Count || Scales->Num() != Count)
		{
			return false;
		}
		for (int32 I = 0; I < Count; ++I)
		{
			double L[3] = {0.0, 0.0, 0.0};
			double R[4] = {0.0, 0.0, 0.0, 1.0};
			double S[3] = {1.0, 1.0, 1.0};
			PropAnimVec((*Locs)[I], 3, L);
			PropAnimVec((*Rots)[I], 4, R);
			PropAnimVec((*Scales)[I], 3, S);
			Out.Times.Add(static_cast<float>((*Times)[I]->AsNumber()));
			Out.Locations.Add(FVector(L[0], L[1], L[2]));
			Out.Rotations.Add(FQuat(R[0], R[1], R[2], R[3]).GetNormalized());
			Out.Scales.Add(FVector(S[0], S[1], S[2]));
		}
		return true;
	}

	TUniquePtr<PropAnims::FSet> PropAnimLoad(const FString& Name)
	{
		const FString File = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Rim3D/Generated/PropAnims"), Name + TEXT(".json"));
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			UE_LOG(LogPropAnims, Warning, TEXT("%s is missing: %s's parts stand still (the codex-models art step makes it)"), *File, *Name);
			return nullptr;
		}
		TUniquePtr<PropAnims::FSet> Set = MakeUnique<PropAnims::FSet>();
		const TSharedPtr<FJsonObject>* Rest = nullptr;
		if (Root->TryGetObjectField(TEXT("rest"), Rest))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Rest)->Values)
			{
				const TSharedPtr<FJsonObject>* Key = nullptr;
				if (Pair.Value->TryGetObject(Key))
				{
					Set->Rest.Add(Pair.Key, PropAnimKey(*Key));
				}
			}
		}
		const TSharedPtr<FJsonObject>* Anims = nullptr;
		if (Root->TryGetObjectField(TEXT("anims"), Anims))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Anims)->Values)
			{
				const TSharedPtr<FJsonObject>* ClipJson = nullptr;
				if (!Pair.Value->TryGetObject(ClipJson))
				{
					continue;
				}
				PropAnims::FClip Clip;
				Clip.Seconds = static_cast<float>((*ClipJson)->GetNumberField(TEXT("seconds")));
				const TSharedPtr<FJsonObject>* Parts = nullptr;
				if ((*ClipJson)->TryGetObjectField(TEXT("parts"), Parts))
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Part : (*Parts)->Values)
					{
						const TSharedPtr<FJsonObject>* TrackJson = nullptr;
						PropAnims::FTrack Track;
						if (Part.Value->TryGetObject(TrackJson) && PropAnimReadTrack(*TrackJson, Track))
						{
							Clip.Parts.Add(Part.Key, MoveTemp(Track));
						}
					}
				}
				Set->Clips.Add(Pair.Key, MoveTemp(Clip));
			}
		}
		UE_LOG(LogPropAnims, Display, TEXT("%s: %d clips, %d parts at rest"), *Name, Set->Clips.Num(), Set->Rest.Num());
		return Set;
	}
}

const PropAnims::FSet* PropAnims::Find(const FString& Name)
{
	TMap<FString, TUniquePtr<FSet>>& Cache = PropAnimCache();
	if (const TUniquePtr<FSet>* Have = Cache.Find(Name))
	{
		return Have->Get();
	}
	return Cache.Add(Name, PropAnimLoad(Name)).Get();
}

FTransform PropAnims::Sample(const FTrack& Track, float Time)
{
	const int32 Count = Track.Times.Num();
	if (Count == 0)
	{
		return FTransform::Identity;
	}
	if (Time <= Track.Times[0] || Count == 1)
	{
		return FTransform(Track.Rotations[0], Track.Locations[0], Track.Scales[0]);
	}
	if (Time >= Track.Times[Count - 1])
	{
		return FTransform(Track.Rotations[Count - 1], Track.Locations[Count - 1], Track.Scales[Count - 1]);
	}
	const int32 Next = static_cast<int32>(Algo::UpperBound(Track.Times, Time));
	const int32 Prev = Next - 1;
	const float Span = FMath::Max(Track.Times[Next] - Track.Times[Prev], 1e-5f);
	const float U = FMath::Clamp((Time - Track.Times[Prev]) / Span, 0.0f, 1.0f);
	return FTransform(FQuat::Slerp(Track.Rotations[Prev], Track.Rotations[Next], U).GetNormalized(),
		FMath::Lerp(Track.Locations[Prev], Track.Locations[Next], U), FMath::Lerp(Track.Scales[Prev], Track.Scales[Next], U));
}

bool PropAnims::Evaluate(const FSet& Set, const FString& Clip, const FString& Part, float Time, bool bLoop, FTransform& Out)
{
	const FClip* Found = Set.Clips.Find(Clip);
	const FTrack* Track = Found ? Found->Parts.Find(Part) : nullptr;
	if (!Track)
	{
		return false;
	}
	const float Length = FMath::Max(Found->Seconds, 1e-3f);
	const float At = bLoop ? FMath::Fmod(FMath::Max(Time, 0.0f), Length) : FMath::Clamp(Time, 0.0f, Length);
	Out = Sample(*Track, At);
	return true;
}

FTransform PropAnims::RestOf(const FSet& Set, const FString& Part)
{
	const FTransform* Rest = Set.Rest.Find(Part);
	return Rest ? *Rest : FTransform::Identity;
}

void PropAnims::Reset()
{
	PropAnimCache().Reset();
}
