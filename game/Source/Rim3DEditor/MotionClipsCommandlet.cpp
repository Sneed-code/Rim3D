#include "MotionClipsCommandlet.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogMotionClips, Log, All);

namespace
{
	const TCHAR* const MotionClipsFolder = TEXT("/Game/Rim3D/Generated/Animations");
	const double MotionClipsRate = 30.0;  // samples per second, as the sim reads them

	/** A bone's transform in the mesh's space at `Time`: its own and every parent's, multiplied up. */
	FTransform MotionClipsBoneAt(const UAnimSequence* Clip, const FReferenceSkeleton& Ref, int32 Bone, double Time)
	{
		FTransform Out = FTransform::Identity;
		for (int32 At = Bone; At != INDEX_NONE; At = Ref.GetParentIndex(At))
		{
			FTransform Local;
			Clip->GetBoneTransform(Local, FSkeletonPoseBoneIndex(At), Time, false);
			Out = Out * Local;
		}
		return Out;
	}

	int32 MotionClipsFindBone(const FReferenceSkeleton& Ref, std::initializer_list<const TCHAR*> Names)
	{
		for (const TCHAR* Name : Names)
		{
			const int32 Index = Ref.FindBoneIndex(FName(Name));
			if (Index != INDEX_NONE)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	struct FMotionClipsPath
	{
		double Seconds = 0.0;
		double HitSeconds = -1.0;
		TArray<FVector2D> Travel;  // metres forward and right
		TArray<double> Yaw;        // degrees to the right
		TArray<double> HandSpeed;  // metres a second of each hand against the hips (for -Trace): right, left
		TArray<double> LeftSpeed;
		TArray<FVector> HandAt;    // each hand against the hips in the mesh's space, cm (for -Trace): right, left
		TArray<FVector> LeftAt;
		double StartYaw = 0.0;     // the hips' own yaw at the start, to see a clip that doesn't begin facing +Y
	};

	/**
	 * The clip's path. The mannequin faces +Y in its own space and its right is -X (the pawn actor turns the mesh by
	 * -90 degrees), so forward is the pelvis's Y and right is minus its X.
	 */
	bool MotionClipsMeasure(const UAnimSequence* Clip, bool bTurn, double Scale, FMotionClipsPath& Out)
	{
		const USkeleton* Skeleton = Clip->GetSkeleton();
		if (!Skeleton)
		{
			return false;
		}
		const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
		const int32 Pelvis = MotionClipsFindBone(Ref, {TEXT("pelvis"), TEXT("Hips")});
		const int32 Hand = MotionClipsFindBone(Ref, {TEXT("hand_r"), TEXT("RightHand")});
		const int32 LeftHand = MotionClipsFindBone(Ref, {TEXT("hand_l"), TEXT("LeftHand")});
		if (Pelvis == INDEX_NONE)
		{
			return false;
		}
		Out.Seconds = Clip->GetPlayLength();
		const int32 Count = FMath::Max(1, FMath::RoundToInt32(Out.Seconds * MotionClipsRate));
		const FTransform Start = MotionClipsBoneAt(Clip, Ref, Pelvis, 0.0);
		const double StartYaw = Start.GetRotation().Rotator().Yaw;
		Out.StartYaw = StartYaw;
		FVector LastHand = FVector::ZeroVector;
		FVector LastLeft = FVector::ZeroVector;
		double Fastest = 0.0;
		for (int32 i = 0; i <= Count; ++i)
		{
			const double Time = FMath::Min(Out.Seconds, static_cast<double>(i) / MotionClipsRate);
			const FTransform At = MotionClipsBoneAt(Clip, Ref, Pelvis, Time);
			const FVector Delta = At.GetLocation() - Start.GetLocation();
			Out.Travel.Add(FVector2D(Delta.Y, -Delta.X) * Scale / 100.0);
			// Unreal's yaw grows to the right seen from above, like the def's.
			Out.Yaw.Add(bTurn ? FMath::FindDeltaAngleDegrees(StartYaw, At.GetRotation().Rotator().Yaw) : 0.0);
			if (Hand != INDEX_NONE)
			{
				// The hand against the body (not the world), so a lunge's own travel doesn't count as hand speed.
				const FVector HandAt = MotionClipsBoneAt(Clip, Ref, Hand, Time).GetLocation() - At.GetLocation();
				const double Speed = i > 0 ? FVector::Dist(HandAt, LastHand) : 0.0;
				if (i > 0 && Time > 0.1 * Out.Seconds && Time < 0.9 * Out.Seconds && Speed > Fastest)
				{
					Fastest = Speed;
					Out.HitSeconds = Time;
				}
				LastHand = HandAt;
				Out.HandAt.Add(HandAt);
				Out.HandSpeed.Add(Speed * MotionClipsRate * Scale / 100.0);
			}
			if (LeftHand != INDEX_NONE)
			{
				const FVector LeftAt = MotionClipsBoneAt(Clip, Ref, LeftHand, Time).GetLocation() - At.GetLocation();
				Out.LeftSpeed.Add(i > 0 ? FVector::Dist(LeftAt, LastLeft) * MotionClipsRate * Scale / 100.0 : 0.0);
				LastLeft = LeftAt;
				Out.LeftAt.Add(LeftAt);
			}
		}
		return true;
	}

	FString MotionClipsNumber(double Value)
	{
		FString Text = FString::Printf(TEXT("%.4f"), FMath::IsNearlyZero(Value, 0.00005) ? 0.0 : Value);
		while (Text.EndsWith(TEXT("0")) && !Text.EndsWith(TEXT(".0")))
		{
			Text.LeftChopInline(1);
		}
		if (Text.EndsWith(TEXT(".0")))
		{
			Text.LeftChopInline(2);
		}
		return Text;
	}
}

UMotionClipsCommandlet::UMotionClipsCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UMotionClipsCommandlet::Main(const FString& Params)
{
	const FString RepoRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".."));

	if (Params.Contains(TEXT("-List")))
	{
		FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		Registry.Get().SearchAllAssets(true);
		TArray<FAssetData> Assets;
		Registry.Get().GetAssetsByPath(FName(MotionClipsFolder), Assets, true);
		Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetObjectPathString() < B.GetObjectPathString(); });
		for (const FAssetData& Asset : Assets)
		{
			const UAnimSequence* Clip = Cast<UAnimSequence>(Asset.GetAsset());
			FMotionClipsPath Path;
			if (Clip && MotionClipsMeasure(Clip, true, 1.0, Path))
			{
				double Furthest = 0.0;
				for (const FVector2D& T : Path.Travel)
				{
					Furthest = FMath::Max(Furthest, T.Size());
				}
				UE_LOG(LogMotionClips, Display, TEXT("LIST %s seconds=%.2f end=(%.2f fwd, %.2f right, %.0f deg) furthest=%.2f hand=%.2f"),
					*Asset.GetObjectPathString(), Path.Seconds, Path.Travel.Last().X, Path.Travel.Last().Y, Path.Yaw.Last(), Furthest, Path.HitSeconds);
			}
		}
	}

	// -Trace=A_Name1+A_Name2: every sample of those clips (time, travel, turn, each hand's speed), for choosing trims.
	FString TraceList;
	if (FParse::Value(*Params, TEXT("-Trace="), TraceList, false))
	{
		TArray<FString> Names;
		TraceList.ParseIntoArray(Names, TEXT("+"));
		FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		Registry.Get().SearchAllAssets(true);
		TArray<FAssetData> Assets;
		Registry.Get().GetAssetsByPath(FName(MotionClipsFolder), Assets, true);
		for (const FAssetData& Asset : Assets)
		{
			if (!Names.Contains(Asset.AssetName.ToString()))
			{
				continue;
			}
			const UAnimSequence* Clip = Cast<UAnimSequence>(Asset.GetAsset());
			FMotionClipsPath Path;
			if (!Clip || !MotionClipsMeasure(Clip, true, 1.0, Path))
			{
				continue;
			}
			UE_LOG(LogMotionClips, Display, TEXT("TRACE %s seconds=%.2f startYaw=%.0f"), *Asset.GetObjectPathString(), Path.Seconds, Path.StartYaw);
			for (int32 i = 0; i < Path.Travel.Num(); ++i)
			{
				const FVector R = Path.HandAt.IsValidIndex(i) ? Path.HandAt[i] : FVector::ZeroVector;
				const FVector L = Path.LeftAt.IsValidIndex(i) ? Path.LeftAt[i] : FVector::ZeroVector;
				UE_LOG(LogMotionClips, Display, TEXT("T %s %.2f fwd=%.2f right=%.2f yaw=%.0f rhand=%.1f lhand=%.1f rat=%.0f,%.0f,%.0f lat=%.0f,%.0f,%.0f"),
					*Asset.AssetName.ToString(), FMath::Min(Path.Seconds, i / MotionClipsRate), Path.Travel[i].X, Path.Travel[i].Y, Path.Yaw[i],
					Path.HandSpeed.IsValidIndex(i) ? Path.HandSpeed[i] : 0.0, Path.LeftSpeed.IsValidIndex(i) ? Path.LeftSpeed[i] : 0.0, R.X, R.Y, R.Z, L.X, L.Y, L.Z);
			}
		}
	}

	// -Pose=A_Name1+A_Name2: where the head, hips, hands and feet are (mesh space, cm; the mannequin faces +Y, its right is
	// -X) at the start, every 0.5 s and the end: how a fall ends and a get-up begins, and what the arms do in a react.
	FString PoseList;
	if (FParse::Value(*Params, TEXT("-Pose="), PoseList, false))
	{
		TArray<FString> Names;
		PoseList.ParseIntoArray(Names, TEXT("+"));
		FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		Registry.Get().SearchAllAssets(true);
		TArray<FAssetData> Assets;
		Registry.Get().GetAssetsByPath(FName(MotionClipsFolder), Assets, true);
		// A full /Game/ path (a clip already on the mannequin, the Game Animation Sample's get-ups) by its own path.
		for (FString& Name : Names)
		{
			if (Name.StartsWith(TEXT("/Game/")))
			{
				const FAssetData Found = Registry.Get().GetAssetByObjectPath(FSoftObjectPath(Name + TEXT(".") + FPaths::GetBaseFilename(Name)));
				if (Found.IsValid())
				{
					Assets.Add(Found);
					Name = FPaths::GetBaseFilename(Name);
				}
			}
		}
		for (const FAssetData& Asset : Assets)
		{
			const UAnimSequence* Clip = Names.Contains(Asset.AssetName.ToString()) ? Cast<UAnimSequence>(Asset.GetAsset()) : nullptr;
			const USkeleton* Skeleton = Clip ? Clip->GetSkeleton() : nullptr;
			if (!Skeleton)
			{
				continue;
			}
			const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
			const TCHAR* const Parts[] = {TEXT("head"), TEXT("pelvis"), TEXT("hand_r"), TEXT("hand_l"), TEXT("foot_r"), TEXT("foot_l"), TEXT("spine_05"), TEXT("spine_03")};
			const double Length = Clip->GetPlayLength();
			for (double Time = 0.0;; Time += 0.25)
			{
				const double T = FMath::Min(Time, Length);
				FString Line = FString::Printf(TEXT("POSE %s t=%.2f"), *Asset.AssetName.ToString(), T);
				for (const TCHAR* Part : Parts)
				{
					const int32 Bone = Ref.FindBoneIndex(FName(Part));
					if (Bone != INDEX_NONE)
					{
						const FVector P = MotionClipsBoneAt(Clip, Ref, Bone, T).GetLocation();
						Line += FString::Printf(TEXT(" %s=%.0f,%.0f,%.0f"), Part, P.X, P.Y, P.Z);
					}
				}
				UE_LOG(LogMotionClips, Display, TEXT("%s"), *Line);
				if (T >= Length)
				{
					break;
				}
			}
		}
		if (FParse::Param(*Params, TEXT("PoseOnly")))
		{
			return 0;
		}
	}

	FString Text;
	TSharedPtr<FJsonObject> Json;
	const FString ListFile = RepoRoot / TEXT("assets/motion_clips.json");
	if (!FFileHelper::LoadFileToString(Text, *ListFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json)
	{
		UE_LOG(LogMotionClips, Error, TEXT("Could not read %s"), *ListFile);
		return 1;
	}
	const double Scale = Json->HasField(TEXT("metresScale")) ? Json->GetNumberField(TEXT("metresScale")) : 1.0;
	const TArray<TSharedPtr<FJsonValue>>* Clips = nullptr;
	if (!Json->TryGetArrayField(TEXT("clips"), Clips))
	{
		UE_LOG(LogMotionClips, Error, TEXT("%s has no \"clips\" list"), *ListFile);
		return 1;
	}

	FString Out = TEXT("[\n");
	int32 Written = 0;
	int32 Missing = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Clips)
	{
		const TSharedPtr<FJsonObject> Entry = Value->AsObject();
		if (!Entry)
		{
			continue;
		}
		const FString DefName = Entry->GetStringField(TEXT("defName"));
		const FString Anim = Entry->GetStringField(TEXT("anim"));  // relative to the folder, e.g. Mixamo/Magic/A_Name
		const FString Kind = Entry->HasField(TEXT("kind")) ? Entry->GetStringField(TEXT("kind")) : TEXT("Clip");
		const bool bTurn = Entry->HasField(TEXT("turn")) && Entry->GetBoolField(TEXT("turn"));
		// `straight`: only the travel along the clip's facing, none to the side (a lunge that steps out to the right would
		// carry the thrust past its target: the impale, PC 2026-11-28).
		const bool bStraight = Entry->HasField(TEXT("straight")) && Entry->GetBoolField(TEXT("straight"));
		const bool bKeep = Entry->HasField(TEXT("keepWhenDown")) && Entry->GetBoolField(TEXT("keepWhenDown"));
		// A full /Game/ path for a clip already on the mannequin (the Game Animation Sample's get-ups), else under the folder.
		const FString AssetPath = Anim.StartsWith(TEXT("/Game/")) ? FString::Printf(TEXT("%s.%s"), *Anim, *FPaths::GetBaseFilename(Anim))
			: FString::Printf(TEXT("%s/%s.%s"), MotionClipsFolder, *Anim, *FPaths::GetBaseFilename(Anim));
		const UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		FMotionClipsPath Path;
		if (!Clip || !MotionClipsMeasure(Clip, bTurn, Scale, Path))
		{
			UE_LOG(LogMotionClips, Warning, TEXT("MISSING %s: no animation at %s"), *DefName, *AssetPath);
			++Missing;
			continue;
		}
		// An attack's blow: the list's own moment, else when the weapon hand was fastest.
		double Hit = -1.0;
		if (Kind == TEXT("Attack"))
		{
			Hit = Entry->HasField(TEXT("hitSeconds")) ? Entry->GetNumberField(TEXT("hitSeconds")) : Path.HitSeconds;
		}
		// A trimmed clip (`from`/`to` seconds): only that part is the def, starting from where the body is then.
		const double From = Entry->HasField(TEXT("from")) ? FMath::Clamp(Entry->GetNumberField(TEXT("from")), 0.0, Path.Seconds) : 0.0;
		const double To = Entry->HasField(TEXT("to")) ? FMath::Clamp(Entry->GetNumberField(TEXT("to")), From, Path.Seconds) : Path.Seconds;
		const int32 First = FMath::RoundToInt32(From * MotionClipsRate);
		const int32 Last = FMath::Min(Path.Travel.Num() - 1, FMath::RoundToInt32(To * MotionClipsRate));
		FString Root;
		for (int32 i = First; i <= Last; ++i)
		{
			FVector2D T = Path.Travel[i] - Path.Travel[First];
			if (bStraight)
			{
				T.Y = 0.0;
			}
			Root += FString::Printf(TEXT("%s[%s,%s,%s]"), i > First ? TEXT(",") : TEXT(""), *MotionClipsNumber(T.X), *MotionClipsNumber(T.Y),
				*MotionClipsNumber(Path.Yaw[i] - Path.Yaw[First]));
		}
		Out += FString::Printf(TEXT("%s  {\"defName\": \"%s\", \"kind\": \"%s\", \"anim\": \"%s\", \"from\": %s, \"seconds\": %s%s%s,\n   \"source\": \"measured by -run=MotionClips from the animation\",\n   \"root\": [%s]}"),
			Written > 0 ? TEXT(",\n") : TEXT(""), *DefName, *Kind, *Anim, *MotionClipsNumber(From), *MotionClipsNumber(To - From),
			Hit >= 0.0 ? *FString::Printf(TEXT(", \"hitSeconds\": %s"), *MotionClipsNumber(Hit - From)) : TEXT(""),
			bKeep ? TEXT(", \"keepWhenDown\": true") : TEXT(""), *Root);
		const FVector2D End = Path.Travel[Last] - Path.Travel[First];
		UE_LOG(LogMotionClips, Display, TEXT("CLIP %s seconds=%.2f hit=%.2f end=(%.2f fwd, %.2f right)"), *DefName, To - From, Hit - From, End.X, End.Y);
		// How the body lies at the start and the end (sneed 2026-11-28, get-ups that start as the fall left the body): where
		// the head points from the hips and which way the chest faces, each as (forward, right, up) of the clip's frame. A
		// chest facing up is a body on its back, down on its front.
		{
			const FReferenceSkeleton& Ref = Clip->GetSkeleton()->GetReferenceSkeleton();
			const int32 PelvisBone = MotionClipsFindBone(Ref, {TEXT("pelvis"), TEXT("Hips")});
			const int32 HeadBone = MotionClipsFindBone(Ref, {TEXT("head"), TEXT("Head")});
			const int32 LeftBone = MotionClipsFindBone(Ref, {TEXT("upperarm_l"), TEXT("LeftArm")});
			const int32 RightBone = MotionClipsFindBone(Ref, {TEXT("upperarm_r"), TEXT("RightArm")});
			if (PelvisBone != INDEX_NONE && HeadBone != INDEX_NONE && LeftBone != INDEX_NONE && RightBone != INDEX_NONE)
			{
				const auto Lie = [&](double Time)
				{
					const FVector P = MotionClipsBoneAt(Clip, Ref, PelvisBone, Time).GetLocation();
					const FVector H = (MotionClipsBoneAt(Clip, Ref, HeadBone, Time).GetLocation() - P).GetSafeNormal();
					const FVector L = MotionClipsBoneAt(Clip, Ref, LeftBone, Time).GetLocation();
					const FVector R = MotionClipsBoneAt(Clip, Ref, RightBone, Time).GetLocation();
					const FVector C = FVector::CrossProduct(R - L, H).GetSafeNormal();
					// Forward is +Y, right is -X (as the path above).
					return FString::Printf(TEXT("head (%.2f, %.2f, %.2f) chest (%.2f, %.2f, %.2f) hips %.0f cm up"), H.Y, -H.X, H.Z, C.Y, -C.X, C.Z, P.Z);
				};
				UE_LOG(LogMotionClips, Display, TEXT("POSE %s start: %s; end: %s"), *DefName, *Lie(From), *Lie(To));
			}
		}
		++Written;
	}
	Out += TEXT("\n]\n");
	const FString OutFile = RepoRoot / TEXT("data/defs/MotionClips/MotionClips.json");
	if (!FFileHelper::SaveStringToFile(Out, *OutFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogMotionClips, Error, TEXT("Could not write %s"), *OutFile);
		return 1;
	}
	UE_LOG(LogMotionClips, Display, TEXT("MotionClips: %d written to %s, %d missing"), Written, *OutFile, Missing);
	return 0;
}
