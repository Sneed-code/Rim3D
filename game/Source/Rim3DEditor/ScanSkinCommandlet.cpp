#include "ScanSkinCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "MeshDescription.h"
#include "MetaHumanCharacter.h"
#include "MetaHumanFaceTextureSynthesizer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SkeletalMeshAttributes.h"

DEFINE_LOG_CATEGORY_STATIC(LogScanSkin, Log, All);

namespace
{
	const TCHAR* const ScanSkinPresetFolder = TEXT("/MetaHumanCharacter/Optional/Presets");
	const TCHAR* const ScanSkinSynthesisFolder = TEXT("Optional/TextureSynthesis");

	FString ScanSkinOutDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("ScanSkin"));
	}

	// ---- Meshes ----------------------------------------------------------------------------------

	void ScanSkinAppendFloats(FString& Out, const TCHAR* Key, const TArray<float>& Values)
	{
		Out += FString::Printf(TEXT("\"%s\":["), Key);
		for (int32 i = 0; i < Values.Num(); ++i)
		{
			Out += FString::Printf(i ? TEXT(",%.5g") : TEXT("%.5g"), Values[i]);
		}
		Out += TEXT("],\n");
	}

	void ScanSkinAppendInts(FString& Out, const TCHAR* Key, const TArray<int32>& Values)
	{
		Out += FString::Printf(TEXT("\"%s\":["), Key);
		for (int32 i = 0; i < Values.Num(); ++i)
		{
			Out += FString::Printf(i ? TEXT(",%d") : TEXT("%d"), Values[i]);
		}
		Out += TEXT("],\n");
	}

	void ScanSkinAppendNames(FString& Out, const TCHAR* Key, const TArray<FString>& Values)
	{
		Out += FString::Printf(TEXT("\"%s\":["), Key);
		for (int32 i = 0; i < Values.Num(); ++i)
		{
			Out += FString::Printf(i ? TEXT(",\"%s\"") : TEXT("\"%s\""), *Values[i].ReplaceCharWithEscapedChar());
		}
		Out += TEXT("],\n");
	}

	// One skeletal mesh's full-detail mesh description as JSON, in Unreal's own terms (centimetres, X forward of the
	// asset, Y right, Z up; texture coordinates with V down): points, triangles (three point indices and three texture
	// coordinates each), each triangle's material slot, skin weights by bone, and the skeleton's bind pose in component
	// space. scan_skin.py turns it into Blender's axes.
	bool ScanSkinWriteMesh(const FString& Name, const FString& AssetPath, const FString& File)
	{
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath);
		if (!Mesh || !Mesh->HasMeshDescription(0))
		{
			UE_LOG(LogScanSkin, Error, TEXT("Mesh %s (%s) did not load or has no mesh description"), *Name, *AssetPath);
			return false;
		}
		const FMeshDescription& Description = *Mesh->GetMeshDescription(0);
		FSkeletalMeshConstAttributes Attributes(Description);
		const TVertexAttributesConstRef<FVector3f> Points = Attributes.GetVertexPositions();
		const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		const TPolygonGroupAttributesConstRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		FSkinWeightsVertexAttributesConstRef Skin = Attributes.GetVertexSkinWeights();

		TMap<FVertexID, int32> VertexIndex;
		TArray<float> Positions;
		TArray<int32> SkinOffsets, SkinBones;
		TArray<float> SkinWeights;
		for (const FVertexID V : Description.Vertices().GetElementIDs())
		{
			VertexIndex.Add(V, VertexIndex.Num());
			const FVector3f P = Points[V];
			Positions.Append({P.X, P.Y, P.Z});
			SkinOffsets.Add(SkinBones.Num());
			for (const UE::AnimationCore::FBoneWeight W : Skin.Get(V))
			{
				if (W.GetWeight() > 0.0f)
				{
					SkinBones.Add(W.GetBoneIndex());
					SkinWeights.Add(W.GetWeight());
				}
			}
		}
		SkinOffsets.Add(SkinBones.Num());

		TMap<FPolygonGroupID, int32> SlotIndex;
		TArray<FString> Slots;
		for (const FPolygonGroupID G : Description.PolygonGroups().GetElementIDs())
		{
			SlotIndex.Add(G, Slots.Num());
			Slots.Add(SlotNames[G].ToString());
		}
		TArray<int32> Triangles, TriangleSlots;
		TArray<float> CornerUVs;
		const int32 UVChannels = UVs.GetNumChannels();
		for (const FTriangleID T : Description.Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexInstanceID> Corners = Description.GetTriangleVertexInstances(T);
			for (const FVertexInstanceID VI : Corners)
			{
				Triangles.Add(VertexIndex.FindChecked(Description.GetVertexInstanceVertex(VI)));
				const FVector2f UV = UVChannels > 0 ? UVs.Get(VI, 0) : FVector2f::ZeroVector;
				CornerUVs.Append({UV.X, UV.Y});
			}
			TriangleSlots.Add(SlotIndex.FindChecked(Description.GetTrianglePolygonGroup(T)));
		}

		const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
		TArray<FString> Bones;
		TArray<int32> Parents;
		TArray<float> BonePositions, BoneRotations;
		TArray<FTransform> World;
		World.SetNum(Skeleton.GetNum());
		for (int32 b = 0; b < Skeleton.GetNum(); ++b)
		{
			const int32 Parent = Skeleton.GetParentIndex(b);
			World[b] = Parent == INDEX_NONE ? Skeleton.GetRefBonePose()[b] : Skeleton.GetRefBonePose()[b] * World[Parent];
			Bones.Add(Skeleton.GetBoneName(b).ToString());
			Parents.Add(Parent);
			const FVector L = World[b].GetLocation();
			const FQuat Q = World[b].GetRotation();
			BonePositions.Append({static_cast<float>(L.X), static_cast<float>(L.Y), static_cast<float>(L.Z)});
			BoneRotations.Append({static_cast<float>(Q.X), static_cast<float>(Q.Y), static_cast<float>(Q.Z), static_cast<float>(Q.W)});
		}

		FString Out = FString::Printf(TEXT("{\"name\":\"%s\",\"asset\":\"%s\",\"units\":\"cm\",\"axes\":\"unreal\",\"uvChannels\":%d,\n"), *Name, *AssetPath, UVChannels);
		ScanSkinAppendNames(Out, TEXT("slots"), Slots);
		ScanSkinAppendNames(Out, TEXT("bones"), Bones);
		ScanSkinAppendInts(Out, TEXT("parents"), Parents);
		ScanSkinAppendFloats(Out, TEXT("bonePositions"), BonePositions);
		ScanSkinAppendFloats(Out, TEXT("boneRotations"), BoneRotations);
		ScanSkinAppendFloats(Out, TEXT("positions"), Positions);
		ScanSkinAppendInts(Out, TEXT("triangles"), Triangles);
		ScanSkinAppendInts(Out, TEXT("triangleSlots"), TriangleSlots);
		ScanSkinAppendFloats(Out, TEXT("uvs"), CornerUVs);
		ScanSkinAppendInts(Out, TEXT("skinOffsets"), SkinOffsets);
		ScanSkinAppendInts(Out, TEXT("skinBones"), SkinBones);
		Out += TEXT("\"skinWeights\":[");
		for (int32 i = 0; i < SkinWeights.Num(); ++i)
		{
			Out += FString::Printf(i ? TEXT(",%.4g") : TEXT("%.4g"), SkinWeights[i]);
		}
		Out += TEXT("]}\n");
		const bool bSaved = FFileHelper::SaveStringToFile(Out, *File);
		UE_LOG(LogScanSkin, Display, TEXT("Mesh %s: %d points, %d triangles, %d slots (%s), %d bones -> %s%s"), *Name, VertexIndex.Num(), TriangleSlots.Num(),
			Slots.Num(), *FString::Join(Slots, TEXT(", ")), Bones.Num(), *File, bSaved ? TEXT("") : TEXT(" (WRITE FAILED)"));
		return bSaved;
	}

	// ---- Textures --------------------------------------------------------------------------------

	bool ScanSkinSavePng(FImage& Image, const FString& File)
	{
		const bool bPngReady = Image.Format == ERawImageFormat::BGRA8 || Image.Format == ERawImageFormat::G8 || Image.Format == ERawImageFormat::G16
			|| Image.Format == ERawImageFormat::RGBA16;
		if (!bPngReady)
		{
			FImage Converted;
			Image.CopyTo(Converted, ERawImageFormat::RGBA16, EGammaSpace::Linear);
			return FImageUtils::SaveImageByExtension(*File, Converted);
		}
		return FImageUtils::SaveImageByExtension(*File, Image);
	}

	// Every Texture2D under the listed folders goes into textures.json (name, package, size, sRGB, compression); those
	// under the export folders are written as PNG too, from their source art (full size). A PNG already there is kept
	// unless -force.
	int32 ScanSkinTextures(const TSharedPtr<FJsonObject>& Config, bool bForce)
	{
		const TArray<TSharedPtr<FJsonValue>>* ExportFolders = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ListFolders = nullptr;
		Config->TryGetArrayField(TEXT("textureFolders"), ExportFolders);
		Config->TryGetArrayField(TEXT("listFolders"), ListFolders);
		TArray<FString> Export, List;
		for (const TSharedPtr<FJsonValue>& V : ExportFolders ? *ExportFolders : TArray<TSharedPtr<FJsonValue>>())
		{
			Export.Add(V->AsString());
		}
		for (const TSharedPtr<FJsonValue>& V : ListFolders ? *ListFolders : TArray<TSharedPtr<FJsonValue>>())
		{
			List.AddUnique(V->AsString());
		}
		for (const FString& F : Export)
		{
			List.AddUnique(F);
		}
		IAssetRegistry& Registry = IAssetRegistry::GetChecked();
		Registry.ScanPathsSynchronous(List, true);

		const FString Dir = ScanSkinOutDir() / TEXT("textures");
		IFileManager::Get().MakeDirectory(*Dir, true);
		TArray<TSharedPtr<FJsonValue>> Index;
		TSet<FString> Seen;
		int32 Failures = 0, Written = 0, Kept = 0;
		for (const FString& Folder : List)
		{
			TArray<FAssetData> Found;
			Registry.GetAssetsByPath(FName(*Folder), Found, true);
			Found.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
			for (const FAssetData& Asset : Found)
			{
				if (Asset.AssetClassPath != UTexture2D::StaticClass()->GetClassPathName() || Seen.Contains(Asset.PackageName.ToString()))
				{
					continue;
				}
				Seen.Add(Asset.PackageName.ToString());
				const bool bExport = Export.ContainsByPredicate([&Asset](const FString& F) { return Asset.PackageName.ToString().StartsWith(F + TEXT("/")); });
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
				Entry->SetStringField(TEXT("package"), Asset.PackageName.ToString());
				if (bExport)
				{
					UTexture2D* Texture = Cast<UTexture2D>(Asset.GetAsset());
					FImage Image;
					if (!Texture || !Texture->Source.IsValid() || !Texture->Source.GetMipImage(Image, 0))
					{
						UE_LOG(LogScanSkin, Warning, TEXT("%s has no source art"), *Asset.PackageName.ToString());
						++Failures;
						continue;
					}
					const FString File = Dir / Asset.AssetName.ToString() + TEXT(".png");
					Entry->SetStringField(TEXT("png"), FString(TEXT("textures/")) + Asset.AssetName.ToString() + TEXT(".png"));
					Entry->SetNumberField(TEXT("width"), Image.SizeX);
					Entry->SetNumberField(TEXT("height"), Image.SizeY);
					Entry->SetBoolField(TEXT("srgb"), Texture->SRGB);
					Entry->SetBoolField(TEXT("normalMap"), Texture->CompressionSettings == TC_Normalmap);
					Entry->SetNumberField(TEXT("compression"), static_cast<int32>(Texture->CompressionSettings.GetValue()));
					if (!bForce && IFileManager::Get().FileSize(*File) > 0)
					{
						++Kept;
					}
					else if (ScanSkinSavePng(Image, File))
					{
						++Written;
						UE_LOG(LogScanSkin, Display, TEXT("Texture %s %dx%d -> %s"), *Asset.AssetName.ToString(), Image.SizeX, Image.SizeY, *File);
					}
					else
					{
						UE_LOG(LogScanSkin, Warning, TEXT("Could not write %s"), *File);
						++Failures;
					}
				}
				Index.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("textures"), Index);
		FString Text;
		FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
		FFileHelper::SaveStringToFile(Text, *(ScanSkinOutDir() / TEXT("textures.json")));
		UE_LOG(LogScanSkin, Display, TEXT("Textures: %d listed, %d written, %d kept, %d failed"), Index.Num(), Written, Kept, Failures);
		// A texture without source art is only a warning (the bake names the ones it uses); none at all is a failure.
		return Written + Kept > 0 ? 0 : 1;
	}

	// ---- Synthesized faces -----------------------------------------------------------------------

	// The texture synthesis model with the biggest textures among those the plugin ships (TS-<version>-..._res-<px>_...).
	FString ScanSkinSynthesisModel(const FString& Content)
	{
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(Content / ScanSkinSynthesisFolder / TEXT("*")), false, true);
		FString Best;
		int32 BestRes = 0;
		for (const FString& Name : Found)
		{
			int32 Res = 0;
			const int32 At = Name.Find(TEXT("res-"));
			if (At != INDEX_NONE)
			{
				Res = FCString::Atoi(*Name.Mid(At + 4));
			}
			UE_LOG(LogScanSkin, Display, TEXT("Texture synthesis model %s (%d px)"), *Name, Res);
			if (Res > BestRes)
			{
				BestRes = Res;
				Best = Name;
			}
		}
		return Best.IsEmpty() ? FString() : Content / ScanSkinSynthesisFolder / Best;
	}

	// Each listed Creator preset's own face colour and normal map from the texture synthesis model (as PawnFaces makes
	// the face skins), written as face_<Preset>_albedo.png and face_<Preset>_normal.png.
	int32 ScanSkinFaces(const TSharedPtr<FJsonObject>& Config, bool bForce)
	{
		const TArray<TSharedPtr<FJsonValue>>* Presets = nullptr;
		if (!Config->TryGetArrayField(TEXT("faceSynth"), Presets) || Presets->IsEmpty())
		{
			return 0;
		}
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MetaHumanCharacter"));
		const FString Model = Plugin ? ScanSkinSynthesisModel(Plugin->GetContentDir()) : FString();
		FMetaHumanFaceTextureSynthesizer Synthesizer;
		if (Model.IsEmpty() || !Synthesizer.Init(Model, 6))
		{
			UE_LOG(LogScanSkin, Error, TEXT("No texture synthesis model in the MetaHumanCharacter plugin"));
			return 1;
		}
		int32 Failures = 0;
		for (const TSharedPtr<FJsonValue>& Value : *Presets)
		{
			const FString Name = Value->AsString();
			const FString ColorFile = ScanSkinOutDir() / FString::Printf(TEXT("face_%s_albedo.png"), *Name);
			const FString NormalFile = ScanSkinOutDir() / FString::Printf(TEXT("face_%s_normal.png"), *Name);
			if (!bForce && IFileManager::Get().FileSize(*ColorFile) > 0 && IFileManager::Get().FileSize(*NormalFile) > 0)
			{
				continue;
			}
			const UMetaHumanCharacter* Preset = LoadObject<UMetaHumanCharacter>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), ScanSkinPresetFolder, *Name, *Name));
			if (!Preset)
			{
				UE_LOG(LogScanSkin, Warning, TEXT("Preset %s did not load"), *Name);
				++Failures;
				continue;
			}
			const FMetaHumanCharacterSkinProperties& Skin = Preset->SkinSettings.Skin;
			FMetaHumanFaceTextureSynthesizer::FTextureSynthesisParams Params{FVector2f(Skin.U, Skin.V),
				FMath::Clamp(Skin.FaceTextureIndex, 0, Synthesizer.GetMaxHighFrequencyIndex() - 1), FMetaHumanFaceTextureSynthesizer::EMapType::Base};
			FImage Color(Synthesizer.GetTextureSizeX(), Synthesizer.GetTextureSizeY(), Synthesizer.GetTextureFormat(), Synthesizer.GetTextureColorSpace());
			FImage Normal(Synthesizer.GetTextureSizeX(), Synthesizer.GetTextureSizeY(), Synthesizer.GetTextureFormat(), EGammaSpace::Linear);
			if (!Synthesizer.SynthesizeAlbedo(Params, Color) || !Synthesizer.SelectNormal(Params, Normal))
			{
				UE_LOG(LogScanSkin, Warning, TEXT("Preset %s: texture synthesis failed"), *Name);
				++Failures;
				continue;
			}
			const bool bSaved = ScanSkinSavePng(Color, ColorFile) && ScanSkinSavePng(Normal, NormalFile);
			Failures += bSaved ? 0 : 1;
			UE_LOG(LogScanSkin, Display, TEXT("Face %s: %dx%d, texture %d, tone (%.2f, %.2f)%s"), *Name, Color.SizeX, Color.SizeY, Params.HighFrequencyIndex, Skin.U, Skin.V,
				bSaved ? TEXT("") : TEXT(" (WRITE FAILED)"));
		}
		return Failures;
	}
}

UScanSkinCommandlet::UScanSkinCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UScanSkinCommandlet::Main(const FString& Params)
{
	const FString ConfigPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../assets/scan_skin.json"));
	FString Text;
	TSharedPtr<FJsonObject> Config;
	if (!FFileHelper::LoadFileToString(Text, *ConfigPath) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Config) || !Config)
	{
		UE_LOG(LogScanSkin, Error, TEXT("Could not read %s"), *ConfigPath);
		return 1;
	}
	const bool bForce = Params.Contains(TEXT("-force"));
	IFileManager::Get().MakeDirectory(*ScanSkinOutDir(), true);
	int32 Failures = 0;
	const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
	if (Config->TryGetArrayField(TEXT("meshes"), Meshes))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Meshes)
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			const FString Name = Entry->GetStringField(TEXT("name"));
			Failures += ScanSkinWriteMesh(Name, Entry->GetStringField(TEXT("asset")), ScanSkinOutDir() / TEXT("mesh_") + Name + TEXT(".json")) ? 0 : 1;
		}
	}
	Failures += ScanSkinTextures(Config, bForce);
	Failures += ScanSkinFaces(Config, bForce);
	UE_LOG(LogScanSkin, Display, TEXT("ScanSkin done, %d failures -> %s"), Failures, *ScanSkinOutDir());
	return Failures ? 1 : 0;
}
