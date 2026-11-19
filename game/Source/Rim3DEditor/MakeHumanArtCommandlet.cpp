#include "MakeHumanArtCommandlet.h"

#include "AnimationRuntime.h"
#include "BoneContainer.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Animation/Skeleton.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/TextureFactory.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "ImageCore.h"
#include "Engine/SubsurfaceProfile.h"
#include "Factories/MaterialFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionBumpOffset.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Dom/JsonObject.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Retargeter/IKRetargeter.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "RigEditor/IKRigDefinitionFactory.h"
#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#include "MakeHumanArt.h"
#include "PawnLooks.h"
#include "SkinBloodMaterial.h"

DEFINE_LOG_CATEGORY_STATIC(LogMakeHumanArt, Log, All);

namespace
{
	const TCHAR* const MhSourceRig = TEXT("/Game/Characters/UEFN_Mannequin/Rigs/IK_UEFN_Mannequin.IK_UEFN_Mannequin");
	// The UE4 mannequin's IK rig and its retargeter from the game's (UEFN) mannequin: MPFB's game engine skeleton uses the
	// UE4 mannequin's bone names, so copies of these, pointed at a MakeHuman mesh, are its rig and retargeter.
	const TCHAR* const MhTemplateRig = TEXT("/Game/Characters/UE4_Mannequin/Rigs/IK_UE4_Mannequin_Retarget.IK_UE4_Mannequin_Retarget");
	const TCHAR* const MhTemplateRetargeter = TEXT("/Game/Characters/UE4_Mannequin/Rigs/RTG_UEFN_to_UE4_Mannequin.RTG_UEFN_to_UE4_Mannequin");
	const TCHAR* const MhGenericRetargetAnim = TEXT("/Game/Blueprints/RetargetedCharacters/ABP_GenericRetarget.ABP_GenericRetarget_C");
	const TCHAR* const MhUE4MannequinBlueprint = TEXT("/Game/Blueprints/RetargetedCharacters/BP_UE4_Mannequin.BP_UE4_Mannequin_C");

	bool MhSave(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *File, Args);
		UE_LOG(LogMakeHumanArt, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *File);
		return bSaved;
	}

	void MhLogProperties(const UStruct* Struct, const void* Container, const TCHAR* Indent)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FString Value;
			It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Container), nullptr, nullptr, PPF_None);
			UE_LOG(LogMakeHumanArt, Display, TEXT("%s%s %s = %s"), Indent, *It->GetCPPType(), *It->GetName(), *Value.Left(200));
		}
	}

	int32 MhProbe()
	{
		// -ProbeAnim=<path>,<path>: the first bones' reference pose and their frame-0 pose in each animation.
		FString AnimPaths;
		if (FParse::Value(FCommandLine::Get(), TEXT("ProbeAnim="), AnimPaths, false))
		{
			TArray<FString> Paths;
			AnimPaths.ParseIntoArray(Paths, TEXT(","));
			for (const FString& Path : Paths)
			{
				if (UIKRigDefinition* ProbeRig = LoadObject<UIKRigDefinition>(nullptr, *Path))
				{
					UE_LOG(LogMakeHumanArt, Display, TEXT("Probe rig %s: root %s"), *Path, *ProbeRig->GetPelvis().ToString());
					for (const FBoneChain& Chain : ProbeRig->GetRetargetChains())
					{
						UE_LOG(LogMakeHumanArt, Display, TEXT("  chain %s: %s .. %s"), *Chain.ChainName.ToString(), *Chain.StartBone.BoneName.ToString(), *Chain.EndBone.BoneName.ToString());
					}
					continue;
				}
				if (UIKRetargeter* ProbeRtg = LoadObject<UIKRetargeter>(nullptr, *Path))
				{
					UIKRetargeterController* ProbeController = UIKRetargeterController::GetController(ProbeRtg);
					for (int32 Op = 0; Op < ProbeController->GetNumRetargetOps(); ++Op)
					{
						FInstancedStruct* Struct = ProbeController->GetRetargetOpStructAtIndex(Op);
						FString Text;
						if (Struct && Struct->GetScriptStruct())
						{
							Struct->GetScriptStruct()->ExportText(Text, Struct->GetMemory(), nullptr, nullptr, PPF_None, nullptr);
						}
						UE_LOG(LogMakeHumanArt, Display, TEXT("  op %d %s %s: %s"), Op, Struct && Struct->GetScriptStruct() ? *Struct->GetScriptStruct()->GetName() : TEXT("?"),
							ProbeController->GetRetargetOpEnabled(Op) ? TEXT("on") : TEXT("off"), *Text.Left(1500));
					}
					continue;
				}
				UAnimSequence* Seq = LoadObject<UAnimSequence>(nullptr, *Path);
				if (!Seq || !Seq->GetSkeleton())
				{
					if (USkeletalMesh* ProbeMesh = LoadObject<USkeletalMesh>(nullptr, *Path))
					{
						const FReferenceSkeleton& MeshRef = ProbeMesh->GetRefSkeleton();
						UE_LOG(LogMakeHumanArt, Display, TEXT("Probe mesh %s: %d bones"), *Path, MeshRef.GetNum());
						{
							// Which way it faces: its left thigh and left toe in component space.
							TArray<FTransform> G;
							FAnimationRuntime::FillUpComponentSpaceTransforms(MeshRef, MeshRef.GetRefBonePose(), G);
							for (const TCHAR* Bone : {TEXT("LeftUpLeg"), TEXT("thigh_l"), TEXT("LeftToeBase"), TEXT("ball_l"), TEXT("Head"), TEXT("head")})
							{
								const int32 Index = MeshRef.FindBoneIndex(Bone);
								if (Index != INDEX_NONE)
								{
									UE_LOG(LogMakeHumanArt, Display, TEXT("  component %s at %s"), Bone, *G[Index].GetLocation().ToString());
								}
							}
						}
						for (int32 b = 0; b < FMath::Min(6, MeshRef.GetNum()); ++b)
						{
							UE_LOG(LogMakeHumanArt, Display, TEXT("  %s parent %d ref %s rot %s"), *MeshRef.GetBoneName(b).ToString(), MeshRef.GetParentIndex(b),
								*MeshRef.GetRefBonePose()[b].GetTranslation().ToString(), *MeshRef.GetRefBonePose()[b].GetRotation().Rotator().ToString());
						}
						continue;
					}
					UE_LOG(LogMakeHumanArt, Display, TEXT("Probe %s: no animation"), *Path);
					continue;
				}
				const FReferenceSkeleton& Ref = Seq->GetSkeleton()->GetReferenceSkeleton();
				UE_LOG(LogMakeHumanArt, Display, TEXT("Probe %s: %d bones, %d frames"), *Path, Ref.GetNum(), Seq->GetNumberOfSampledKeys());
				// The lean of the body: pelvis (hips) to head in component space, from straight up, over the clip.
				{
					const int32 Pelvis = Ref.FindBoneIndex(TEXT("pelvis")) != INDEX_NONE ? Ref.FindBoneIndex(TEXT("pelvis")) : Ref.FindBoneIndex(TEXT("Hips"));
					const int32 Head = Ref.FindBoneIndex(TEXT("head")) != INDEX_NONE ? Ref.FindBoneIndex(TEXT("head")) : Ref.FindBoneIndex(TEXT("Head"));
					if (Pelvis != INDEX_NONE && Head != INDEX_NONE)
					{
						FString Leans;
						for (int32 k = -1; k <= 8; ++k)
						{
							const FFrameNumber Frame(FMath::Clamp(Seq->GetNumberOfSampledKeys() * k / 8, 0, Seq->GetNumberOfSampledKeys() - 1));
							TArray<FTransform> Component;
							Component.SetNum(Ref.GetNum());
							for (int32 b = 0; b < Ref.GetNum(); ++b)
							{
								const FName Bone = Ref.GetBoneName(b);
								const FTransform Local = k >= 0 && Seq->GetDataModel()->IsValidBoneTrackName(Bone) ? Seq->GetDataModel()->GetBoneTrackTransform(Bone, Frame) : Ref.GetRefBonePose()[b];  // k -1: the reference pose
								const int32 Parent = Ref.GetParentIndex(b);
								Component[b] = Parent == INDEX_NONE ? Local : Local * Component[Parent];
							}
							if (k <= 0)
							{
								FString Chain;
								for (const TCHAR* Name : {TEXT("root"), TEXT("pelvis"), TEXT("spine_01"), TEXT("spine_03"), TEXT("spine_05"), TEXT("neck_01"), TEXT("MixamoRig"), TEXT("Hips"), TEXT("Spine"), TEXT("Spine1"), TEXT("Spine2"), TEXT("Neck"), TEXT("head"), TEXT("Head")})
								{
									const int32 Index = Ref.FindBoneIndex(Name);
									if (Index != INDEX_NONE)
									{
										const FVector L = Component[Index].GetTranslation();
										const FRotator R = Component[Index].GetRotation().Rotator();
										Chain += FString::Printf(TEXT(" %s(%.0f,%.0f,%.0f|%.0f,%.0f,%.0f)"), Name, L.X, L.Y, L.Z, R.Pitch, R.Yaw, R.Roll);
									}
								}
								UE_LOG(LogMakeHumanArt, Display, TEXT("  %s:%s"), k < 0 ? TEXT("ref") : TEXT("frame0"), *Chain);
							}
							const FVector Up = (Component[Head].GetTranslation() - Component[Pelvis].GetTranslation()).GetSafeNormal();
							Leans += FString::Printf(TEXT(" %.0f"), FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Up.Z, -1.0, 1.0))));
						}
						UE_LOG(LogMakeHumanArt, Display, TEXT("  lean pelvis-head from up (degrees, 9 times over the clip):%s"), *Leans);
					}
				}
				for (int32 b = 0; b < FMath::Min(6, Ref.GetNum()); ++b)
				{
					const FName Bone = Ref.GetBoneName(b);
					const FTransform RefPose = Ref.GetRefBonePose()[b];
					const FTransform Frame = Seq->GetDataModel()->GetBoneTrackTransform(Bone, FFrameNumber(0));
					UE_LOG(LogMakeHumanArt, Display, TEXT("  %s ref %s rot %s | frame0 %s rot %s"), *Bone.ToString(), *RefPose.GetTranslation().ToString(),
						*RefPose.GetRotation().Rotator().ToString(), *Frame.GetTranslation().ToString(), *Frame.GetRotation().Rotator().ToString());
					for (int32 k = 1; k < 4; ++k)
					{
						const FTransform Later = Seq->GetDataModel()->GetBoneTrackTransform(Bone, FFrameNumber(Seq->GetNumberOfSampledKeys() * k / 4));
						UE_LOG(LogMakeHumanArt, Display, TEXT("     at %d/4: %s angle from ref %.1f"), k, *Later.GetTranslation().ToString(),
							FMath::RadiansToDegrees(Later.GetRotation().AngularDistance(RefPose.GetRotation())));
					}
				}
			}
			return 0;
		}
		if (UClass* Anim = LoadClass<UAnimInstance>(nullptr, MhGenericRetargetAnim))
		{
			UE_LOG(LogMakeHumanArt, Display, TEXT("Probe %s (parent %s):"), *Anim->GetPathName(), *Anim->GetSuperClass()->GetName());
			for (TFieldIterator<FProperty> It(Anim, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				FString Value;
				It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Anim->GetDefaultObject()), nullptr, nullptr, PPF_None);
				UE_LOG(LogMakeHumanArt, Display, TEXT("  %s %s = %s"), *It->GetCPPType(), *It->GetName(), *Value.Left(300));
			}
		}
		if (UBlueprintGeneratedClass* Bp = Cast<UBlueprintGeneratedClass>(LoadClass<AActor>(nullptr, MhUE4MannequinBlueprint)))
		{
			UE_LOG(LogMakeHumanArt, Display, TEXT("Probe %s (parent %s):"), *Bp->GetPathName(), *Bp->GetSuperClass()->GetPathName());
			for (TFieldIterator<FProperty> It(Bp, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				FString Value;
				It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Bp->GetDefaultObject()), nullptr, nullptr, PPF_None);
				UE_LOG(LogMakeHumanArt, Display, TEXT("  var %s %s = %s"), *It->GetCPPType(), *It->GetName(), *Value.Left(300));
			}
			for (const UBlueprintGeneratedClass* C = Bp; C; C = Cast<UBlueprintGeneratedClass>(C->GetSuperClass()))
			{
				if (!C->SimpleConstructionScript)
				{
					continue;
				}
				for (USCS_Node* Node : C->SimpleConstructionScript->GetAllNodes())
				{
					if (USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Node->ComponentTemplate))
					{
						UE_LOG(LogMakeHumanArt, Display, TEXT("  component %s: mesh %s, anim %s, leader %s"), *Node->GetVariableName().ToString(),
							Mesh->GetSkeletalMeshAsset() ? *Mesh->GetSkeletalMeshAsset()->GetPathName() : TEXT("none"),
							Mesh->GetAnimClass() ? *Mesh->GetAnimClass()->GetPathName() : TEXT("none"), Mesh->LeaderPoseComponent.IsValid() ? TEXT("yes") : TEXT("no"));
					}
				}
			}
		}
		return 0;
	}

	USkeletalMesh* MhImportFbx(IAssetTools& Tools, const FString& Fbx, const FString& Folder, const FString& Name)
	{
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = Fbx;
		Task->DestinationPath = Folder;
		Task->DestinationName = Name;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = false;
		UFbxFactory* Factory = NewObject<UFbxFactory>();
		Factory->ImportUI = NewObject<UFbxImportUI>(Factory);
		Factory->ImportUI->bImportMesh = true;
		Factory->ImportUI->bImportAsSkeletal = true;
		Factory->ImportUI->MeshTypeToImport = FBXIT_SkeletalMesh;
		Factory->ImportUI->bImportAnimations = false;
		Factory->ImportUI->bImportMaterials = false;
		Factory->ImportUI->bImportTextures = false;
		Factory->ImportUI->bCreatePhysicsAsset = true;
		Factory->ImportUI->Skeleton = nullptr;
		Factory->ImportUI->SkeletalMeshImportData->bImportMorphTargets = true;
		Factory->ImportUI->SkeletalMeshImportData->bConvertScene = true;
		// Blender writes metres; Unreal works in centimetres.
		Factory->ImportUI->SkeletalMeshImportData->ImportUniformScale = 1.0f;  // the export is in centimetres (tools/makehuman)
		Factory->ImportUI->SkeletalMeshImportData->bUseT0AsRefPose = false;
		Task->Factory = Factory;
		Task->Options = Factory->ImportUI;
		Tools.ImportAssetTasks({Task});
		for (UObject* Object : Task->GetObjects())
		{
			if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Object))
			{
				return Mesh;
			}
		}
		UE_LOG(LogMakeHumanArt, Error, TEXT("Importing %s made no skeletal mesh (%d objects)"), *Fbx, Task->GetObjects().Num());
		return nullptr;
	}

	UTexture2D* MhImportTexture(const FString& File, const FString& Folder, const FString& Name, bool bColor)
	{
		TArray64<uint8> Bytes;
		FImage Image;
		if (!FFileHelper::LoadFileToArray(Bytes, *File) || !FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Image))
		{
			UE_LOG(LogMakeHumanArt, Error, TEXT("Could not read %s"), *File);
			return nullptr;
		}
		UPackage* Package = CreatePackage(*(Folder / Name));
		UTexture2D* Texture = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone);
		Texture->PreEditChange(nullptr);
		Texture->Source.Init(Image);
		Texture->SetModernSettingsForNewOrChangedTexture();
		Texture->SRGB = bColor;
		Texture->CompressionSettings = bColor ? TC_Default : TC_Normalmap;
		Texture->LODGroup = bColor ? TEXTUREGROUP_Character : TEXTUREGROUP_CharacterNormalMap;
		Texture->PostEditChange();
		return Texture;
	}

	// The linear average of a texture's lit texels (as PawnLooks takes it for M_PawnSkin's SourceAverage).
	FLinearColor MhAverage(UTexture2D* Texture)
	{
		FImage Image, Linear;
		if (!Texture || !Texture->Source.GetMipImage(Image, 0))
		{
			return FLinearColor(0.5f, 0.4f, 0.35f);
		}
		Image.GammaSpace = Texture->SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
		Image.CopyTo(Linear, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		FLinearColor Sum(0, 0, 0, 0);
		int64 Count = 0;
		const TArrayView64<FLinearColor> P = Linear.AsRGBA32F();
		for (int64 i = 0; i < P.Num(); i += 5)
		{
			const float L = P[i].GetLuminance();
			if (L > 0.03f && L < 0.9f)
			{
				Sum += P[i];
				++Count;
			}
		}
		return Count ? FLinearColor(Sum.R / Count, Sum.G / Count, Sum.B / Count, 1.0f) : FLinearColor(0.5f, 0.4f, 0.35f);
	}

	// assets/makehuman.json: the characters to import. A character's skin colour map is skin.png beside its FBX
	// (mh_export.py copies it there).
	struct FMhCharacter
	{
		FMakeHumanCharacterFiles Files;
		TSharedPtr<FJsonObject> Config;
	};

	// assets/makehuman.json, and assets/makehuman.local.json (not in Git) for local-only comparison models.
	TArray<FMhCharacter> MakeHumanCharacters(const FString& Root)
	{
		TArray<FMhCharacter> Out;
		for (const TCHAR* ListFile : {TEXT("assets/makehuman.json"), TEXT("assets/makehuman.local.json")})
		{
		const bool bLocalList = FString(ListFile).Contains(TEXT(".local."));
		FString Text;
		TSharedPtr<FJsonObject> Json;
		if (!FFileHelper::LoadFileToString(Text, *(Root / ListFile)) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json)
		{
			if (!bLocalList)
			{
				UE_LOG(LogMakeHumanArt, Error, TEXT("Could not read %s"), ListFile);
			}
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (Json->TryGetArrayField(TEXT("characters"), List))
		{
			for (const TSharedPtr<FJsonValue>& Value : *List)
			{
				const TSharedPtr<FJsonObject> C = Value->AsObject();
				FMakeHumanCharacterFiles Files;
				Files.Name = C->GetStringField(TEXT("name"));
				Files.Fbx = C->GetStringField(TEXT("fbx"));
				if (FPaths::IsRelative(Files.Fbx))
				{
					Files.Fbx = Root / Files.Fbx;
				}
				Files.SkinTexture = FPaths::GetPath(Files.Fbx) / TEXT("skin.png");
				if (!FPaths::FileExists(Files.SkinTexture))
				{
					Files.SkinTexture.Reset();
				}
				C->TryGetBoolField(TEXT("localOnly"), Files.bLocalOnly);
				Files.bLocalOnly |= bLocalList;
				Out.Add({Files, C});
			}
		}
		}
		return Out;
	}

	// Wetness (custom primitive data 12: 0 dry .. 1 soaked; the creator's slider, later the weather): a wet surface is
	// darker (water fills the pores and fibres; hair and cloth darken most) and glossier. One shared step on every
	// MakeHuman material, so skin, hair and jewellery all get wet together.
	void MhAddWetness(UMaterial* Material, float Darken, float WetRoughness)
	{
		auto* Wet = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -200, 1500));
		Wet->ParameterName = TEXT("Wetness");
		Wet->DefaultValue = 0.0f;
		Wet->bUseCustomPrimitiveData = true;
		Wet->PrimitiveDataIndex = 12;
		if (UMaterialExpression* BaseColor = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, MP_BaseColor))
		{
			auto* Dark = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -100, -700));
			Dark->OutputType = CMOT_Float3;
			Dark->Description = TEXT("WetColour");
			Dark->Code = FString::Printf(TEXT("return Base * lerp(1.0, %.3f, saturate(Wet));"), Darken);
			Dark->Inputs.SetNum(2);
			Dark->Inputs[0].InputName = TEXT("Base");
			Dark->Inputs[1].InputName = TEXT("Wet");
			Dark->Inputs[0].Input.Connect(Material->GetEditorOnlyData()->BaseColor.OutputIndex, BaseColor);
			Dark->Inputs[1].Input.Connect(0, Wet);
			UMaterialEditingLibrary::ConnectMaterialProperty(Dark, TEXT(""), MP_BaseColor);
		}
		if (UMaterialExpression* Roughness = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, MP_Roughness))
		{
			auto* Gloss = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -100, 1400));
			Gloss->OutputType = CMOT_Float1;
			Gloss->Description = TEXT("WetRoughness");
			Gloss->Code = FString::Printf(TEXT("return lerp(Base, min(Base, %.3f), saturate(Wet));"), WetRoughness);
			Gloss->Inputs.SetNum(2);
			Gloss->Inputs[0].InputName = TEXT("Base");
			Gloss->Inputs[1].InputName = TEXT("Wet");
			Gloss->Inputs[0].Input.Connect(Material->GetEditorOnlyData()->Roughness.OutputIndex, Roughness);
			Gloss->Inputs[1].Input.Connect(0, Wet);
			UMaterialEditingLibrary::ConnectMaterialProperty(Gloss, TEXT(""), MP_Roughness);
		}
	}

	// The squish table (USoftContactSubsystem): 16 by 8 float texels, nearest filtering, no mips, never streamed, so the
	// game can overwrite it every frame (UpdateTextureRegions) and the skin material can Load() it in the vertex shader.
	UTexture2D* MhSoftContactTexture()
	{
		const FString Name = TEXT("T_MH_SoftContacts");
		const FString Path = MakeHumanAssetPath(MakeHumanFolder, Name);
		if (UTexture2D* Existing = FindObject<UTexture2D>(nullptr, *Path))
		{
			return Existing;
		}
		UPackage* Package = CreatePackage(*(FString(MakeHumanFolder) / Name));
		UTexture2D* Texture = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone);
		Texture->PreEditChange(nullptr);
		TArray<FLinearColor> Zeros;
		Zeros.SetNumZeroed(16 * 8);
		Texture->Source.Init(16, 8, 1, 1, TSF_RGBA32F, reinterpret_cast<const uint8*>(Zeros.GetData()));
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_HDR_F32;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->Filter = TF_Nearest;
		Texture->NeverStream = true;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->LODGroup = TEXTUREGROUP_Pixels2D;
		Texture->PostEditChange();
		MhSave(Texture);
		return Texture;
	}

	// Our skin material (M_PawnSkin) with the MetaHumans' skin micro-normal tiled over the skin's own normal: pores and
	// fine lines at any distance, which MakeHuman's painted skins lack (MicroTiling, MicroStrength).
	UMaterialInterface* MhSkinDetailMaterial(IAssetTools& Tools, UMaterialInterface* SkinBase)
	{
		const FString Name = TEXT("M_MH_SkinDetail");
		const FString Path = MakeHumanAssetPath(MakeHumanFolder, Name);
		if (UMaterial* Existing = FindObject<UMaterial>(nullptr, *Path))
		{
			return Existing;
		}
		UMaterial* Source = Cast<UMaterial>(SkinBase);
		UTexture* Micro = LoadObject<UTexture>(nullptr, TEXT("/Game/MetaHumans/Common/Face/Textures/T_SkinMicroNormal001.T_SkinMicroNormal001"));
		if (!Source || !Micro)
		{
			return SkinBase;
		}
		UMaterial* Material = Cast<UMaterial>(Tools.DuplicateAsset(Name, MakeHumanFolder, Source));
		UMaterialExpression* BaseNormal = Material ? UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, MP_Normal) : nullptr;
		if (!BaseNormal)
		{
			return SkinBase;
		}
		auto* Coord = Cast<UMaterialExpressionTextureCoordinate>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -1400, 650));
		auto* Tiling = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -1400, 750));
		Tiling->ParameterName = TEXT("MicroTiling");
		Tiling->DefaultValue = 70.0f;
		auto* Tiled = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass(), -1200, 700));
		Tiled->A.Connect(0, Coord);
		Tiled->B.Connect(0, Tiling);
		auto* MicroSample = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -1000, 700));
		MicroSample->ParameterName = TEXT("MicroNormal");
		MicroSample->Texture = Micro;
		MicroSample->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Micro);
		MicroSample->Coordinates.Connect(0, Tiled);
		auto* Strength = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -1000, 900));
		Strength->ParameterName = TEXT("MicroStrength");
		Strength->DefaultValue = 0.4f;
		auto* Blend = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -600, 600));
		Blend->OutputType = CMOT_Float3;
		Blend->Description = TEXT("SkinMicroNormal");
		Blend->Code = TEXT("float3 m = lerp(float3(0, 0, 1), Micro, Strength);\nreturn normalize(float3(Base.xy + m.xy, Base.z * m.z));");
		Blend->Inputs.SetNum(3);
		Blend->Inputs[0].InputName = TEXT("Base");
		Blend->Inputs[1].InputName = TEXT("Micro");
		Blend->Inputs[2].InputName = TEXT("Strength");
		Blend->Inputs[0].Input.Connect(0, BaseNormal);
		Blend->Inputs[1].Input.Connect(0, MicroSample);
		Blend->Inputs[2].Input.Connect(0, Strength);
		UMaterialEditingLibrary::ConnectMaterialProperty(Blend, TEXT(""), MP_Normal);

		// Wrinkles (mh_common.make_wrinkle_map): four groups of creases as heights in RGBA (forehead, brow furrow, smile,
		// nose), each by how far its face units are on (custom primitive data 8 to 11, set by the creator), turned into a
		// normal over the skin's.
		{
			auto* WrinkleMap = Cast<UMaterialExpressionTextureObjectParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureObjectParameter::StaticClass(), -1000, 1500));
			WrinkleMap->ParameterName = TEXT("WrinkleMap");
			WrinkleMap->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/Black.Black"));
			WrinkleMap->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(WrinkleMap->Texture);
			auto* Weights = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -1000, 1700));
			Weights->ParameterName = TEXT("WrinkleWeights");
			Weights->DefaultValue = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
			Weights->bUseCustomPrimitiveData = true;
			Weights->PrimitiveDataIndex = 8;
			auto* WrinkleStrength = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -1000, 1850));
			WrinkleStrength->ParameterName = TEXT("WrinkleStrength");
			WrinkleStrength->DefaultValue = 6.0f;
			auto* WrinkleUV = Cast<UMaterialExpressionTextureCoordinate>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -1000, 1950));
			auto* Wrinkle = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -400, 1500));
			Wrinkle->OutputType = CMOT_Float3;
			Wrinkle->Description = TEXT("SkinWrinkles");
			Wrinkle->Code = TEXT("float4 w = Weights;\n"
				"if (dot(w, 1) <= 0.001) return Base;\n"
				"float2 px = float2(1.0 / 2048.0, 1.0 / 2048.0);\n"
				"float4 c = Texture2DSample(Tex, TexSampler, UV);\n"
				"float4 sx = Texture2DSample(Tex, TexSampler, UV + float2(px.x, 0));\n"
				"float4 sy = Texture2DSample(Tex, TexSampler, UV + float2(0, px.y));\n"
				"c.rgb = pow(c.rgb, 0.4545); sx.rgb = pow(sx.rgb, 0.4545); sy.rgb = pow(sy.rgb, 0.4545);\n"
				"float h = dot(c, w), hx = dot(sx, w), hy = dot(sy, w);\n"
				"float3 n = normalize(float3((hx - h) * Strength, (hy - h) * Strength, 1.0));\n"
				"return normalize(float3(Base.xy + n.xy, Base.z * n.z));");
			Wrinkle->Inputs.SetNum(5);
			Wrinkle->Inputs[0].InputName = TEXT("Base");
			Wrinkle->Inputs[1].InputName = TEXT("Tex");
			Wrinkle->Inputs[2].InputName = TEXT("UV");
			Wrinkle->Inputs[3].InputName = TEXT("Weights");
			Wrinkle->Inputs[4].InputName = TEXT("Strength");
			Wrinkle->Inputs[0].Input.Connect(0, Blend);
			Wrinkle->Inputs[1].Input.Connect(0, WrinkleMap);
			Wrinkle->Inputs[2].Input.Connect(0, WrinkleUV);
			Wrinkle->Inputs[3].Input.Connect(5, Weights);  // output 5 is RGBA (0 is RGB)
			Wrinkle->Inputs[4].Input.Connect(0, WrinkleStrength);
			UMaterialEditingLibrary::ConnectMaterialProperty(Wrinkle, TEXT(""), MP_Normal);
		}

		// Markings (tools/makehuman/markings.py: a 4 by 4 atlas of designs): up to four stamps by texture coordinates,
		// each MarkNA (u, v, angle, texture across the stamp), MarkNB (design 0-15, opacity, kind, texture up the stamp), MarkNC (colour).
		// Kinds: 0 tattoo ink, 1 scar (paler, raised), 2 brand (darker, raised), 3 paint (opaque colour).
		{
			auto* Atlas = Cast<UMaterialExpressionTextureObjectParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureObjectParameter::StaticClass(), -1400, -900));
			Atlas->ParameterName = TEXT("MarkAtlas");
			Atlas->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/Black.Black"));
			Atlas->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Atlas->Texture);
			auto* MarkUV = Cast<UMaterialExpressionTextureCoordinate>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -1400, -1000));
			TArray<UMaterialExpressionVectorParameter*> Marks;
			for (int32 m = 0; m < 4; ++m)
			{
				for (const TCHAR* Part : {TEXT("A"), TEXT("B"), TEXT("C")})
				{
					auto* P = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -1400, -800 + Marks.Num() * 60));
					P->ParameterName = FName(*FString::Printf(TEXT("Mark%d%s"), m, Part));
					P->DefaultValue = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
					Marks.Add(P);
				}
			}
			const FString Stamp = TEXT("float4 Stamp(float2 uv, float4 A, float4 B, Texture2D Tex, SamplerState S)\n"
				"{\n"
				"  if (B.y <= 0.001 || A.w <= 0.0001) return 0;\n"
				"  float2 size = float2(A.w, B.w > 0.0001 ? B.w : A.w);\n"
				"  float2 d = float2(uv.x - A.x, uv.y - A.y);\n"
				"  float c = cos(A.z), s = sin(A.z);\n"
				"  float2 l = float2(c * d.x - s * d.y, -(s * d.x + c * d.y)) / size + 0.5;\n"
				"  if (any(l < 0.0) || any(l > 1.0)) return 0;\n"
				"  float cell = floor(B.x + 0.5);\n"
				"  float2 at = (float2(fmod(cell, 4.0), floor(cell / 4.0)) + l) / 4.0;\n"
				"  return float4(Tex.SampleLevel(S, at, 0).r * B.y, B.z, 0, 0);\n"
				"}\n");
			// The colour: each stamp over the last.
			auto* MarkColour = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -150, -700));
			MarkColour->OutputType = CMOT_Float3;
			MarkColour->Description = TEXT("SkinMarkings");
			MarkColour->Code = TEXT("struct F { ") + Stamp + TEXT(" } f;\n"
				"float3 col = Base;\n"
				"float4 A[4] = {M0A, M1A, M2A, M3A}; float4 B[4] = {M0B, M1B, M2B, M3B}; float3 C[4] = {M0C.rgb, M1C.rgb, M2C.rgb, M3C.rgb};\n"
				"for (int i = 0; i < 4; ++i)\n"
				"{\n"
				"  float4 st = f.Stamp(UV, A[i], B[i], Atlas, AtlasSampler);\n"
				"  float a = saturate(st.x); float k = floor(st.y + 0.5);\n"
				"  if (a <= 0.0) continue;\n"
				"  if (k < 0.5) col = lerp(col, col * C[i], a);\n"
				"  else if (k < 1.5) col = lerp(col, saturate(col * float3(1.12, 0.98, 0.97) + 0.04), a);\n"
				"  else if (k < 2.5) col = lerp(col, col * float3(0.55, 0.36, 0.33), a);\n"
				"  else col = lerp(col, C[i], a);\n"
				"}\n"
				"return col;");
			// The normal: scars and brands raised a little.
			auto* MarkNormal = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -150, -500));
			MarkNormal->OutputType = CMOT_Float3;
			MarkNormal->Description = TEXT("SkinMarkingsNormal");
			MarkNormal->Code = TEXT("struct F { ") + Stamp + TEXT(" } f;\n"
				"float4 A[4] = {M0A, M1A, M2A, M3A}; float4 B[4] = {M0B, M1B, M2B, M3B};\n"
				"float2 e = float2(0.0005, 0);\n"
				"float h = 0, hx = 0, hy = 0;\n"
				"for (int i = 0; i < 4; ++i)\n"
				"{\n"
				"  float k = floor(B[i].z + 0.5);\n"
				"  if (k < 0.5 || k > 2.5 || B[i].y <= 0.001) continue;\n"
				"  h += f.Stamp(UV, A[i], B[i], Atlas, AtlasSampler).x;\n"
				"  hx += f.Stamp(UV + e.xy, A[i], B[i], Atlas, AtlasSampler).x;\n"
				"  hy += f.Stamp(UV + e.yx, A[i], B[i], Atlas, AtlasSampler).x;\n"
				"}\n"
				"if (h + hx + hy <= 0.0) return Base;\n"
				"float3 n = normalize(float3((h - hx) * 2.5, (h - hy) * 2.5, 1.0));\n"
				"return normalize(float3(Base.xy + n.xy, Base.z * n.z));");
			for (UMaterialExpressionCustom* Node : {MarkColour, MarkNormal})
			{
				Node->Inputs.Reset();
				for (const TCHAR* InputName : {TEXT("Base"), TEXT("UV"), TEXT("Atlas")})
				{
					Node->Inputs.AddDefaulted_GetRef().InputName = InputName;
				}
				Node->Inputs[1].Input.Connect(0, MarkUV);
				Node->Inputs[2].Input.Connect(0, Atlas);
				for (int32 m = 0; m < 4; ++m)
				{
					for (int32 p = 0; p < (Node == MarkColour ? 3 : 2); ++p)
					{
						FCustomInput& In = Node->Inputs.AddDefaulted_GetRef();
						In.InputName = FName(*FString::Printf(TEXT("M%d%s"), m, p == 0 ? TEXT("A") : (p == 1 ? TEXT("B") : TEXT("C"))));
						In.Input.Connect(5, Marks[m * 3 + p]);  // RGBA
					}
				}
			}
			if (UMaterialExpression* BaseColorIn = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, MP_BaseColor))
			{
				MarkColour->Inputs[0].Input.Connect(0, BaseColorIn);
				UMaterialEditingLibrary::ConnectMaterialProperty(MarkColour, TEXT(""), MP_BaseColor);
			}
			if (UMaterialExpression* NormalIn = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, MP_Normal))
			{
				MarkNormal->Inputs[0].Input.Connect(0, NormalIn);
				UMaterialEditingLibrary::ConnectMaterialProperty(MarkNormal, TEXT(""), MP_Normal);
			}
		}

		// Roughness map (the 4K skin's): mid-grey keeps the material's Roughness, lighter rougher, darker smoother, by
		// RoughnessMapAmount (0 on the 2K skin, which has no map).
		if (UMaterialExpression* RoughnessValue = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, MP_Roughness))
		{
			auto* RoughMap = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -1000, 1100));
			RoughMap->ParameterName = TEXT("RoughnessMap");
			RoughMap->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/Black.Black"));
			RoughMap->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(RoughMap->Texture);
			auto* RoughAmount = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -1000, 1300));
			RoughAmount->ParameterName = TEXT("RoughnessMapAmount");
			RoughAmount->DefaultValue = 0.0f;
			auto* Rough = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -500, 1100));
			Rough->OutputType = CMOT_Float1;
			Rough->Description = TEXT("SkinRoughness");
			Rough->Code = TEXT("return saturate(Base + (pow(Map.r, 0.4545) - 0.5) * Amount);");  // the map is an sRGB texture
			Rough->Inputs.SetNum(3);
			Rough->Inputs[0].InputName = TEXT("Base");
			Rough->Inputs[1].InputName = TEXT("Map");
			Rough->Inputs[2].InputName = TEXT("Amount");
			Rough->Inputs[0].Input.Connect(0, RoughnessValue);
			Rough->Inputs[1].Input.Connect(0, RoughMap);
			Rough->Inputs[2].Input.Connect(0, RoughAmount);
			UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		}

		// Pigment: the areolae and genitals (PigmentMask, made by mh_export.py) darker and warmer than the rest, by the
		// pawn's pigment (custom primitive data 3, 0 to 1; the creator's slider). It multiplies the skin's own colour, so
		// it follows the skin tone: a darker skin's areolae are darker still.
		if (UMaterialExpression* BaseColor = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, MP_BaseColor))
		{
			auto* Mask = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -1000, -500));
			Mask->ParameterName = TEXT("PigmentMask");
			Mask->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/Black.Black"));
			Mask->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Mask->Texture);
			auto* Amount = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -1000, -350));
			Amount->ParameterName = TEXT("Pigment");
			Amount->DefaultValue = 0.5f;
			Amount->bUseCustomPrimitiveData = true;
			Amount->PrimitiveDataIndex = 3;
			auto* Pigment = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -300, -300));
			Pigment->OutputType = CMOT_Float3;
			Pigment->Description = TEXT("SkinPigment");
			Pigment->Code = TEXT("float m = saturate(Mask * Amount * 1.4);\nreturn Base * lerp(float3(1, 1, 1), float3(0.5, 0.33, 0.28), m);");
			Pigment->Inputs.SetNum(3);
			Pigment->Inputs[0].InputName = TEXT("Base");
			Pigment->Inputs[1].InputName = TEXT("Mask");
			Pigment->Inputs[2].InputName = TEXT("Amount");
			Pigment->Inputs[0].Input.Connect(0, BaseColor);
			Pigment->Inputs[1].Input.Connect(0, Mask);
			Pigment->Inputs[2].Input.Connect(0, Amount);
			UMaterialEditingLibrary::ConnectMaterialProperty(Pigment, TEXT(""), MP_BaseColor);
		}
		// Skin dents (soft body, docs/design/soft-body.md): up to two hits pushing the skin in along its normal as a smooth
		// bowl round a world point, custom primitive data 16-19 and 21-24 (x, y, z, depth in cm; a negative depth bulges
		// out as the skin springs back) and 20 and 25 (radius in cm), set each frame by UMakeHumanAnimInstance. Added to
		// any world position offset the skin already has.
		{
			auto* Pos = Cast<UMaterialExpressionWorldPosition>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionWorldPosition::StaticClass(), -1000, 2300));
			auto* Nrm = Cast<UMaterialExpressionVertexNormalWS>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVertexNormalWS::StaticClass(), -1000, 2400));
			UMaterialExpression* DentIn[4] = {};
			for (int32 d = 0; d < 2; ++d)
			{
				auto* At = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -1000, 2500 + d * 200));
				At->ParameterName = FName(*FString::Printf(TEXT("Dent%dAt"), d));
				At->DefaultValue = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
				At->bUseCustomPrimitiveData = true;
				At->PrimitiveDataIndex = 16 + d * 5;
				auto* Radius = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -1000, 2600 + d * 200));
				Radius->ParameterName = FName(*FString::Printf(TEXT("Dent%dRadius"), d));
				Radius->DefaultValue = 10.0f;
				Radius->bUseCustomPrimitiveData = true;
				Radius->PrimitiveDataIndex = 20 + d * 5;
				DentIn[d * 2] = At;
				DentIn[d * 2 + 1] = Radius;
			}
			auto* Dent = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -400, 2400));
			Dent->OutputType = CMOT_Float3;
			Dent->Description = TEXT("SkinDent");
			Dent->Code = TEXT("float3 o = Base;\n"
				"float4 A[2] = {A0, A1};\n"
				"float R[2] = {R0, R1};\n"
				"for (int i = 0; i < 2; ++i)\n"
				"{\n"
				"  if (abs(A[i].w) < 0.01 || R[i] < 0.5) continue;\n"
				"  float x = saturate(length(P - A[i].xyz) / R[i]);\n"
				"  float f = 1.0 - x * x;\n"
				"  o -= N * A[i].w * f * f;\n"
				"}\n"
				"return o;");
			const TCHAR* Names[] = {TEXT("Base"), TEXT("P"), TEXT("N"), TEXT("A0"), TEXT("R0"), TEXT("A1"), TEXT("R1")};
			Dent->Inputs.SetNum(UE_ARRAY_COUNT(Names));
			for (int32 i = 0; i < static_cast<int32>(UE_ARRAY_COUNT(Names)); ++i)
			{
				Dent->Inputs[i].InputName = Names[i];
			}
			int32 First = 1;
			if (UMaterialExpression* OldOffset = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, MP_WorldPositionOffset))
			{
				Dent->Inputs[0].Input.Connect(Material->GetEditorOnlyData()->WorldPositionOffset.OutputIndex, OldOffset);
			}
			else
			{
				// No offset before: start from zero (an unconnected input would not compile).
				Dent->Code = Dent->Code.Replace(TEXT("float3 o = Base;"), TEXT("float3 o = 0;"));
				Dent->Inputs.RemoveAt(0);
				First = 0;
			}
			Dent->Inputs[First].Input.Connect(0, Pos);
			Dent->Inputs[First + 1].Input.Connect(0, Nrm);
			Dent->Inputs[First + 2].Input.Connect(5, DentIn[0]);  // RGBA
			Dent->Inputs[First + 3].Input.Connect(0, DentIn[1]);
			Dent->Inputs[First + 4].Input.Connect(5, DentIn[2]);
			Dent->Inputs[First + 5].Input.Connect(0, DentIn[3]);
			UMaterialEditingLibrary::ConnectMaterialProperty(Dent, TEXT(""), MP_WorldPositionOffset);

			// The dent's light: world position offset leaves the normals as they were, so a dent hardly showed (2026-11-16
			// clip). The bowl's walls face in towards its middle: its slope (dDepth/dr) along the skin, turned into tangent
			// space and added to the skin's normal, a little exaggerated (1.5).
			auto* Tilt = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -400, 2700));
			Tilt->OutputType = CMOT_Float3;
			Tilt->Description = TEXT("SkinDentTilt");
			Tilt->Code = TEXT("float3 t = 0;\n"
				"float4 A[2] = {A0, A1};\n"
				"float R[2] = {R0, R1};\n"
				"for (int i = 0; i < 2; ++i)\n"
				"{\n"
				"  if (abs(A[i].w) < 0.01 || R[i] < 0.5) continue;\n"
				"  float3 d = P - A[i].xyz;\n"
				"  d -= N * dot(d, N);\n"
				"  float r = length(d);\n"
				"  float x = saturate(r / R[i]);\n"
				"  if (x >= 1.0 || r < 0.001) continue;\n"
				"  float slope = A[i].w * 4.0 * x * (1.0 - x * x) / R[i];\n"
				"  t -= (d / r) * slope;\n"
				"}\n"
				"return t;");
			const TCHAR* TiltNames[] = {TEXT("P"), TEXT("N"), TEXT("A0"), TEXT("R0"), TEXT("A1"), TEXT("R1")};
			Tilt->Inputs.SetNum(UE_ARRAY_COUNT(TiltNames));
			for (int32 i = 0; i < static_cast<int32>(UE_ARRAY_COUNT(TiltNames)); ++i)
			{
				Tilt->Inputs[i].InputName = TiltNames[i];
			}
			Tilt->Inputs[0].Input.Connect(0, Pos);
			Tilt->Inputs[1].Input.Connect(0, Nrm);
			Tilt->Inputs[2].Input.Connect(5, DentIn[0]);
			Tilt->Inputs[3].Input.Connect(0, DentIn[1]);
			Tilt->Inputs[4].Input.Connect(5, DentIn[2]);
			Tilt->Inputs[5].Input.Connect(0, DentIn[3]);
			auto* ToTangent = Cast<UMaterialExpressionTransform>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTransform::StaticClass(), -200, 2700));
			ToTangent->TransformSourceType = TRANSFORMSOURCE_World;
			ToTangent->TransformType = TRANSFORM_Tangent;
			ToTangent->Input.Connect(0, Tilt);
			if (UMaterialExpression* NormalIn = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, MP_Normal))
			{
				auto* DentNormal = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -50, 2700));
				DentNormal->OutputType = CMOT_Float3;
				DentNormal->Description = TEXT("SkinDentNormal");
				DentNormal->Code = TEXT("return normalize(float3(Base.xy + T.xy * 1.5, Base.z));");
				DentNormal->Inputs.SetNum(2);
				DentNormal->Inputs[0].InputName = TEXT("Base");
				DentNormal->Inputs[1].InputName = TEXT("T");
				DentNormal->Inputs[0].Input.Connect(Material->GetEditorOnlyData()->Normal.OutputIndex, NormalIn);
				DentNormal->Inputs[1].Input.Connect(0, ToTangent);
				UMaterialEditingLibrary::ConnectMaterialProperty(DentNormal, TEXT(""), MP_Normal);
			}
		}

		// Squish on contact (USoftContactSubsystem, docs/design/soft-body.md step 2): the body's row of T_MH_SoftContacts
		// (custom primitive data 13; -1 none) lists up to 8 capsules pressing on it (texel 2i: A.xyz, radius; 2i+1: B.xyz,
		// strength). Skin facing a capsule is pushed out of it along the line from its axis, with a soft edge (softplus over
		// 2.5 cm, at most 10 cm), and its normal turned to lie on the capsule where it is pressed flat.
		if (UTexture* Contacts = MhSoftContactTexture())
		{
			auto* Table = Cast<UMaterialExpressionTextureObjectParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureObjectParameter::StaticClass(), -1000, 3100));
			Table->ParameterName = TEXT("SoftContacts");
			Table->Texture = Contacts;
			Table->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Contacts);
			auto* Row = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -1000, 3250));
			Row->ParameterName = TEXT("SoftContactRow");
			Row->DefaultValue = -1.0f;
			Row->bUseCustomPrimitiveData = true;
			Row->PrimitiveDataIndex = 13;
			auto* SquishPos = Cast<UMaterialExpressionWorldPosition>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionWorldPosition::StaticClass(), -1000, 3350));
			SquishPos->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;  // the skin before any offset, in both shaders
			auto* SquishNrm = Cast<UMaterialExpressionVertexNormalWS>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVertexNormalWS::StaticClass(), -1000, 3450));
			const FString Loop = TEXT(
				"int y = (int)(Row + 0.5);\n"
				"for (int i = 0; i < 8; ++i)\n"
				"{\n"
				"  float4 a = Tex.Load(int3(i * 2, y, 0));\n"
				"  float4 b = Tex.Load(int3(i * 2 + 1, y, 0));\n"
				"  if (b.w <= 0.0 || a.w <= 0.0) continue;\n"
				"  float3 ab = b.xyz - a.xyz;\n"
				"  float t = saturate(dot(P - a.xyz, ab) / max(dot(ab, ab), 0.0001));\n"
				"  float3 v = P - (a.xyz + ab * t);\n"
				"  float d = length(v);\n"
				"  if (d < 0.001) continue;\n"
				"  float3 dir = v / d;\n"
				"  float pen = a.w - d;\n"
				"  if (pen < -10.0) continue;\n"
				"  float face = saturate(dot(N, -dir) * 1.5 + 0.5) * b.w;\n"
				"  float amt = min(2.5 * log(1.0 + exp(pen / 2.5)), 3.0) * face;\n"
				"  BODY\n"
				"}\n");
			auto* Squish = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -300, 3200));
			Squish->OutputType = CMOT_Float3;
			Squish->Description = TEXT("SkinSquish");
			Squish->Code = TEXT("float3 o = Base;\nif (Row < -0.5) return o;\n") + Loop.Replace(TEXT("BODY"), TEXT("o += dir * amt;")) + TEXT("return o;");
			auto* SquishTilt = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -300, 3400));
			SquishTilt->OutputType = CMOT_Float3;
			SquishTilt->Description = TEXT("SkinSquishTilt");
			SquishTilt->Code = TEXT("float3 n = N;\nif (Row < -0.5) return 0;\n")
				+ Loop.Replace(TEXT("BODY"), TEXT("n = lerp(n, -dir, saturate(pen / 2.5) * face * 0.35);"))
				+ TEXT("return normalize(n) - N;");
			for (UMaterialExpressionCustom* Node : {Squish, SquishTilt})
			{
				const bool bOffset = Node == Squish;
				Node->Inputs.Reset();  // a new Custom node comes with one unnamed input
				const TCHAR* Names[] = {TEXT("Base"), TEXT("P"), TEXT("N"), TEXT("Tex"), TEXT("Row")};
				for (int32 i = bOffset ? 0 : 1; i < static_cast<int32>(UE_ARRAY_COUNT(Names)); ++i)
				{
					Node->Inputs.AddDefaulted_GetRef().InputName = Names[i];
				}
				const int32 First = bOffset ? 1 : 0;
				Node->Inputs[First].Input.Connect(0, SquishPos);
				Node->Inputs[First + 1].Input.Connect(0, SquishNrm);
				Node->Inputs[First + 2].Input.Connect(0, Table);
				Node->Inputs[First + 3].Input.Connect(0, Row);
			}
			if (UMaterialExpression* OldOffset = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, MP_WorldPositionOffset))
			{
				Squish->Inputs[0].Input.Connect(Material->GetEditorOnlyData()->WorldPositionOffset.OutputIndex, OldOffset);
				UMaterialEditingLibrary::ConnectMaterialProperty(Squish, TEXT(""), MP_WorldPositionOffset);
			}
			auto* SquishToTangent = Cast<UMaterialExpressionTransform>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTransform::StaticClass(), -150, 3400));
			SquishToTangent->TransformSourceType = TRANSFORMSOURCE_World;
			SquishToTangent->TransformType = TRANSFORM_Tangent;
			SquishToTangent->Input.Connect(0, SquishTilt);
			if (UMaterialExpression* NormalIn = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Material, MP_Normal))
			{
				auto* SquishNormal = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -50, 3400));
				SquishNormal->OutputType = CMOT_Float3;
				SquishNormal->Description = TEXT("SkinSquishNormal");
				SquishNormal->Code = TEXT("return normalize(float3(Base.xy + T.xy, Base.z));");
				SquishNormal->Inputs.SetNum(2);
				SquishNormal->Inputs[0].InputName = TEXT("Base");
				SquishNormal->Inputs[1].InputName = TEXT("T");
				SquishNormal->Inputs[0].Input.Connect(Material->GetEditorOnlyData()->Normal.OutputIndex, NormalIn);
				SquishNormal->Inputs[1].Input.Connect(0, SquishToTangent);
				UMaterialEditingLibrary::ConnectMaterialProperty(SquishNormal, TEXT(""), MP_Normal);
			}
		}
		MhAddWetness(Material, 0.78f, 0.12f);
		SkinBloodMaterial::Add(Material);  // blood painted into the skin (USkinBlood), off until a pawn bleeds
		Material->PostEditChange();
		MhSave(Material);
		UE_LOG(LogMakeHumanArt, Display, TEXT("skin material with micro detail and dents: %s"), *Material->GetName());
		return Material;
	}

	// Plain lit and masked materials for models that bring their own textures (local comparison models): colour,
	// normal and (masked) opacity maps, roughness a value; the skin variant is subsurface skin.
	// The grown pubic hair (mh_common.grow_pubic_hair): one colour (Tint, the creator's hair colour), two-sided, and cut
	// out hair by hair by the pawn's pubic density and coverage (custom primitive data 4 and 5, 0 to 1): each hair carries
	// a random number in its texture U (kept while under the density) and how far out it grows in V (kept while inside
	// the coverage), so thinning is even and coverage shrinks from the edges and the top.
	UMaterial* MhPubicMaterial(IAssetTools& Tools)
	{
		const FString Name = TEXT("M_MH_PubicHair");
		const FString Path = MakeHumanAssetPath(MakeHumanFolder, Name);
		if (UMaterial* Existing = FindObject<UMaterial>(nullptr, *Path))
		{
			return Existing;
		}
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(Name, MakeHumanFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		bool bRecompile = false;
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_SkeletalMesh, bRecompile);
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_MorphTargets, bRecompile);
		Material->BlendMode = BLEND_Masked;
		Material->TwoSided = true;
		auto* Tint = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -600, -300));
		Tint->ParameterName = TEXT("Tint");
		Tint->DefaultValue = FLinearColor(0.03f, 0.025f, 0.022f);
		UMaterialEditingLibrary::ConnectMaterialProperty(Tint, TEXT("RGB"), MP_BaseColor);
		auto* Rough = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -600, -150));
		Rough->ParameterName = TEXT("Roughness");
		Rough->DefaultValue = 0.6f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		auto* Coord = Cast<UMaterialExpressionTextureCoordinate>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -900, 100));
		auto* Density = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -900, 200));
		Density->ParameterName = TEXT("PubicDensity");
		Density->DefaultValue = 1.0f;
		Density->bUseCustomPrimitiveData = true;
		Density->PrimitiveDataIndex = 4;
		auto* Coverage = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -900, 300));
		Coverage->ParameterName = TEXT("PubicCoverage");
		Coverage->DefaultValue = 1.0f;
		Coverage->bUseCustomPrimitiveData = true;
		Coverage->PrimitiveDataIndex = 5;
		auto* Cut = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -500, 150));
		Cut->OutputType = CMOT_Float1;
		Cut->Description = TEXT("PubicCut");
		Cut->Code = TEXT("return (UV.x < Density && UV.y <= Coverage) ? 1.0 : 0.0;");
		Cut->Inputs.SetNum(3);
		Cut->Inputs[0].InputName = TEXT("UV");
		Cut->Inputs[1].InputName = TEXT("Density");
		Cut->Inputs[2].InputName = TEXT("Coverage");
		Cut->Inputs[0].Input.Connect(0, Coord);
		Cut->Inputs[1].Input.Connect(0, Density);
		Cut->Inputs[2].Input.Connect(0, Coverage);
		UMaterialEditingLibrary::ConnectMaterialProperty(Cut, TEXT(""), MP_OpacityMask);
		MhAddWetness(Material, 0.55f, 0.3f);
		Material->PostEditChange();
		MhSave(Material);
		return Material;
	}

	// Eyes (MakeHuman's eyeballs: both eyes on one texture, the iris round the middle of each half): glossy and wet, the
	// iris set back behind the cornea (parallax, so it shifts as the eye turns to the light and the view), a dark ring at
	// its edge (the limbus), cut out where the texture is clear (the cornea's own area).
	UMaterial* MhEyeMaterial(IAssetTools& Tools)
	{
		const FString Name = TEXT("M_MH_Eye");
		const FString Path = MakeHumanAssetPath(MakeHumanFolder, Name);
		if (UMaterial* Existing = FindObject<UMaterial>(nullptr, *Path))
		{
			return Existing;
		}
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(Name, MakeHumanFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		bool bRecompile = false;
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_SkeletalMesh, bRecompile);
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_MorphTargets, bRecompile);
		Material->BlendMode = BLEND_Masked;
		const auto Make = [Material](UClass* Class, int32 X, int32 Y) { return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y); };
		auto* Coord = Cast<UMaterialExpressionTextureCoordinate>(Make(UMaterialExpressionTextureCoordinate::StaticClass(), -1400, 0));
		// Where the iris is: x its mask (1 inside), y the limbal ring.
		auto* Iris = Cast<UMaterialExpressionCustom>(Make(UMaterialExpressionCustom::StaticClass(), -1150, 0));
		Iris->OutputType = CMOT_Float2;
		Iris->Description = TEXT("EyeIris");
		Iris->Code = TEXT("float2 c = UV.x < 0.5 ? float2(0.293, 0.713) : float2(0.707, 0.297);\n"
			"float d = distance(UV, c);\n"
			"float m = 1.0 - smoothstep(0.104, 0.118, d);\n"
			"float ring = exp(-pow((d - 0.111) / 0.011, 2.0));\n"
			"return float2(m, ring);");
		Iris->Inputs.SetNum(1);
		Iris->Inputs[0].InputName = TEXT("UV");
		Iris->Inputs[0].Input.Connect(0, Coord);
		// The iris lower than the cornea: the sclera at 1, the iris at 0.
		auto* Depth = Cast<UMaterialExpressionCustom>(Make(UMaterialExpressionCustom::StaticClass(), -900, 150));
		Depth->OutputType = CMOT_Float1;
		Depth->Description = TEXT("EyeDepth");
		Depth->Code = TEXT("return 1.0 - Iris.x;");
		Depth->Inputs.SetNum(1);
		Depth->Inputs[0].InputName = TEXT("Iris");
		Depth->Inputs[0].Input.Connect(0, Iris);
		auto* Bump = Cast<UMaterialExpressionBumpOffset>(Make(UMaterialExpressionBumpOffset::StaticClass(), -700, 100));
		Bump->Coordinate.Connect(0, Coord);
		Bump->Height.Connect(0, Depth);
		Bump->HeightRatio = 0.035f;
		Bump->ReferencePlane = 1.0f;
		auto* EyeUV = Cast<UMaterialExpressionCustom>(Make(UMaterialExpressionCustom::StaticClass(), -500, 0));
		EyeUV->OutputType = CMOT_Float2;
		EyeUV->Description = TEXT("EyeUV");
		EyeUV->Code = TEXT("return lerp(UV, Bumped, Iris.x);");
		EyeUV->Inputs.SetNum(3);
		EyeUV->Inputs[0].InputName = TEXT("UV");
		EyeUV->Inputs[1].InputName = TEXT("Bumped");
		EyeUV->Inputs[2].InputName = TEXT("Iris");
		EyeUV->Inputs[0].Input.Connect(0, Coord);
		EyeUV->Inputs[1].Input.Connect(0, Bump);
		EyeUV->Inputs[2].Input.Connect(0, Iris);
		auto* Color = Cast<UMaterialExpressionTextureSampleParameter2D>(Make(UMaterialExpressionTextureSampleParameter2D::StaticClass(), -300, -100));
		Color->ParameterName = TEXT("BaseColor");
		Color->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		Color->Coordinates.Connect(0, EyeUV);
		auto* Tint = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -300, -300));
		Tint->ParameterName = TEXT("Tint");
		Tint->DefaultValue = FLinearColor::White;
		// The iris in a colour of its own (the creator's eye colour, the Drow's red), by IrisAmount: the texture's
		// lightness kept as the iris's detail, the colour laid over it.
		auto* IrisColour = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -300, -450));
		IrisColour->ParameterName = TEXT("IrisColour");
		IrisColour->DefaultValue = FLinearColor(0.5f, 0.35f, 0.2f);
		auto* IrisAmount = Cast<UMaterialExpressionScalarParameter>(Make(UMaterialExpressionScalarParameter::StaticClass(), -300, -550));
		IrisAmount->ParameterName = TEXT("IrisAmount");
		IrisAmount->DefaultValue = 0.0f;
		auto* Shade = Cast<UMaterialExpressionCustom>(Make(UMaterialExpressionCustom::StaticClass(), -50, -150));
		Shade->OutputType = CMOT_Float3;
		Shade->Description = TEXT("EyeColour");
		Shade->Code = TEXT("float3 base = Tex * Tint;\n"
			"float lum = dot(Tex, float3(0.3, 0.59, 0.11));\n"
			"float3 own = IrisColour * (0.35 + 1.5 * lum);\n"
			"return lerp(base, own, Iris.x * saturate(Amount)) * (1.0 - 0.4 * Iris.y);");
		Shade->Inputs.SetNum(5);
		Shade->Inputs[0].InputName = TEXT("Tex");
		Shade->Inputs[1].InputName = TEXT("Tint");
		Shade->Inputs[2].InputName = TEXT("Iris");
		Shade->Inputs[3].InputName = TEXT("IrisColour");
		Shade->Inputs[4].InputName = TEXT("Amount");
		Shade->Inputs[0].Input.Connect(0, Color);
		Shade->Inputs[1].Input.Connect(0, Tint);
		Shade->Inputs[2].Input.Connect(0, Iris);
		Shade->Inputs[3].Input.Connect(0, IrisColour);
		Shade->Inputs[4].Input.Connect(0, IrisAmount);
		UMaterialEditingLibrary::ConnectMaterialProperty(Shade, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Color, TEXT("A"), MP_OpacityMask);
		// Wet: very smooth (sharp highlights, the sky and the sun's catchlight), the iris smoother still.
		auto* Rough = Cast<UMaterialExpressionCustom>(Make(UMaterialExpressionCustom::StaticClass(), -50, 100));
		Rough->OutputType = CMOT_Float1;
		Rough->Description = TEXT("EyeRoughness");
		Rough->Code = TEXT("return lerp(0.16, 0.06, Iris.x);");
		Rough->Inputs.SetNum(1);
		Rough->Inputs[0].InputName = TEXT("Iris");
		Rough->Inputs[0].Input.Connect(0, Iris);
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		auto* Specular = Cast<UMaterialExpressionScalarParameter>(Make(UMaterialExpressionScalarParameter::StaticClass(), -50, 250));
		Specular->ParameterName = TEXT("Specular");
		Specular->DefaultValue = 0.7f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Specular, TEXT(""), MP_Specular);
		Material->PostEditChange();
		MhSave(Material);
		return Material;
	}

	// Jewellery (mh_common.add_jewellery): polished metal in one colour (Tint: gold, silver, steel...).
	UMaterial* MhMetalMaterial(IAssetTools& Tools)
	{
		const FString Name = TEXT("M_MH_Metal");
		const FString Path = MakeHumanAssetPath(MakeHumanFolder, Name);
		if (UMaterial* Existing = FindObject<UMaterial>(nullptr, *Path))
		{
			return Existing;
		}
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(Name, MakeHumanFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		bool bRecompile = false;
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_SkeletalMesh, bRecompile);
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_MorphTargets, bRecompile);
		auto* Tint = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -500, -200));
		Tint->ParameterName = TEXT("Tint");
		Tint->DefaultValue = FLinearColor(0.8f, 0.62f, 0.3f);
		UMaterialEditingLibrary::ConnectMaterialProperty(Tint, TEXT("RGB"), MP_BaseColor);
		auto* Metal = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -500, 0));
		Metal->ParameterName = TEXT("Metallic");
		Metal->DefaultValue = 1.0f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Metal, TEXT(""), MP_Metallic);
		auto* Rough = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -500, 150));
		Rough->ParameterName = TEXT("Roughness");
		Rough->DefaultValue = 0.25f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		MhAddWetness(Material, 1.0f, 0.1f);
		Material->PostEditChange();
		MhSave(Material);
		return Material;
	}

	UMaterial* MhBaseMaterial(IAssetTools& Tools, const FString& BaseName, bool bMasked, bool bSkin, bool bAlphaFromColor = false)
	{
		const FString Name = BaseName + TEXT("_Wet2");  // the current version (wetness, hair colour); earlier ones stay on disk unused
		const FString Path = MakeHumanAssetPath(MakeHumanFolder, Name);
		if (UMaterial* Existing = FindObject<UMaterial>(nullptr, *Path))
		{
			return Existing;
		}
		if (FPackageName::DoesPackageExist(FString(MakeHumanFolder) / Name))
		{
			if (UMaterial* Loaded = LoadObject<UMaterial>(nullptr, *Path))
			{
				return Loaded;
			}
		}
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(Name, MakeHumanFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		bool bRecompile = false;
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_SkeletalMesh, bRecompile);
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_MorphTargets, bRecompile);
		if (bSkin)
		{
			Material->SetShadingModel(MSM_SubsurfaceProfile);
			Material->SubsurfaceProfile = LoadObject<USubsurfaceProfile>(nullptr, TEXT("/Game/MetaHumans/Common/Face/SubsurfaceProfiles/SSP_Head.SSP_Head"));
		}
		if (bMasked)
		{
			Material->BlendMode = BLEND_Masked;
			Material->TwoSided = true;
			// Hair cards, brows and lashes: soft edges (the cut-out dithered and smoothed by TSR, not a hard edge) and a
			// lower cut-off so the fine tips stay, which makes brows and lashes fuller.
			Material->DitherOpacityMask = true;
			Material->OpacityMaskClipValue = 0.2f;
		}
		UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		UTexture* Flat = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));
		auto* Color = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -600, -200));
		Color->ParameterName = TEXT("BaseColor");
		Color->Texture = White;
		auto* Tint = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -600, -400));
		Tint->ParameterName = TEXT("Tint");
		Tint->DefaultValue = FLinearColor::White;
		if (bMasked)
		{
			// Hair, brows and lashes: the colour is the tint, the texture only its strand detail (its lightness). Tint times
			// the brown texture made white or platinum hair (the Drow's) come out light brown.
			auto* HairColour = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -300, -250));
			HairColour->OutputType = CMOT_Float3;
			HairColour->Description = TEXT("HairColour");
			HairColour->Code = TEXT("return Tint * lerp(0.5, 1.1, saturate(dot(Tex, float3(0.3, 0.59, 0.11)) * 3.0));");
			HairColour->Inputs.SetNum(2);
			HairColour->Inputs[0].InputName = TEXT("Tex");
			HairColour->Inputs[1].InputName = TEXT("Tint");
			HairColour->Inputs[0].Input.Connect(0, Color);
			HairColour->Inputs[1].Input.Connect(0, Tint);
			UMaterialEditingLibrary::ConnectMaterialProperty(HairColour, TEXT(""), MP_BaseColor);
		}
		else
		{
			auto* Tinted = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass(), -300, -250);
			UMaterialEditingLibrary::ConnectMaterialExpressions(Color, TEXT("RGB"), Tinted, TEXT("A"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT("RGB"), Tinted, TEXT("B"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Tinted, TEXT(""), MP_BaseColor);
		}
		if (bMasked && bAlphaFromColor)
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(Color, TEXT("A"), MP_OpacityMask);
		}
		auto* Normal = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -600, 100));
		Normal->ParameterName = TEXT("Normal");
		Normal->Texture = Flat;
		Normal->SamplerType = SAMPLERTYPE_Normal;
		UMaterialEditingLibrary::ConnectMaterialProperty(Normal, TEXT("RGB"), MP_Normal);
		auto* Rough = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -300, 300));
		Rough->ParameterName = TEXT("Roughness");
		Rough->DefaultValue = bSkin ? 0.55f : 0.75f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		if (bMasked && !bAlphaFromColor)
		{
			auto* Opacity = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -600, 400));
			Opacity->ParameterName = TEXT("Opacity");
			Opacity->Texture = White;
			Opacity->SamplerType = SAMPLERTYPE_LinearColor;
			UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT("R"), MP_OpacityMask);
		}
		// Hair and cards darken and gloss the most when wet; plain parts (horns, the cat's fur) a little less.
		MhAddWetness(Material, bMasked ? 0.55f : 0.7f, bSkin ? 0.12f : 0.3f);
		Material->PostEditChange();
		MhSave(Material);
		return Material;
	}

	// The bones of a chain, start to end (walking up from the end).
	TArray<int32> MhChainBones(const FReferenceSkeleton& Ref, const FBoneChain& Chain)
	{
		TArray<int32> Out;
		const int32 Start = Ref.FindBoneIndex(Chain.StartBone.BoneName);
		for (int32 b = Ref.FindBoneIndex(Chain.EndBone.BoneName); b != INDEX_NONE; b = Ref.GetParentIndex(b))
		{
			Out.Insert(b, 0);
			if (b == Start)
			{
				return Out;
			}
		}
		return {};
	}

	// The target's retarget pose, turned so each chain bone points where the source's matching bone points in the
	// source's reference pose (an A pose's arms and legs differ between skeletons; unaligned, the retargeted walk
	// keeps the target's own spread). The engine's auto align needs the editor's processor; this is the same idea:
	// local deltas, NewLocal = RefLocal * Delta.
	void MhAlignRetargetPose(UIKRetargeterController* Controller, const UIKRigDefinition* SourceRig, const USkeletalMesh* SourceMesh,
		const UIKRigDefinition* TargetRig, const USkeletalMesh* TargetMesh, bool bAlignSpine = true)
	{
		const FReferenceSkeleton& SRef = SourceMesh->GetRefSkeleton();
		const FReferenceSkeleton& TRef = TargetMesh->GetRefSkeleton();
		// The source as its retarget pose has it (the template's source pose may turn bones off its reference pose).
		TArray<FTransform> SLocal = SRef.GetRefBonePose();
		int32 SourceTurned = 0;
		for (int32 b = 0; b < SRef.GetNum(); ++b)
		{
			const FQuat Delta = Controller->GetRotationOffsetForRetargetPoseBone(SRef.GetBoneName(b), ERetargetSourceOrTarget::Source);
			if (!Delta.Equals(FQuat::Identity, 1.0e-4f))
			{
				SLocal[b].SetRotation(SLocal[b].GetRotation() * Delta);
				++SourceTurned;
			}
		}
		TArray<FTransform> SGlobal;
		FAnimationRuntime::FillUpComponentSpaceTransforms(SRef, SLocal, SGlobal);
		UE_LOG(LogMakeHumanArt, Display, TEXT("  source retarget pose %s: %d bones off the reference pose"),
			*Controller->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Source).ToString(), SourceTurned);
		TArray<FTransform> Local = TRef.GetRefBonePose();
		const auto Globals = [&]()
		{
			TArray<FTransform> G;
			FAnimationRuntime::FillUpComponentSpaceTransforms(TRef, Local, G);
			return G;
		};
		const FName Aligned[] = {TEXT("Spine"), TEXT("Neck"), TEXT("LeftClavicle"), TEXT("RightClavicle"), TEXT("LeftArm"), TEXT("RightArm"),
			TEXT("LeftLeg"), TEXT("RightLeg")};
		int32 Turned = 0;
		for (const FName& Name : Aligned)
		{
			if (!bAlignSpine && (Name == TEXT("Spine") || Name == TEXT("Neck")))
			{
				continue;
			}
			const FBoneChain* TChain = TargetRig->GetRetargetChainByName(Name);
			const FBoneChain* SChain = SourceRig->GetRetargetChainByName(Name);
			if (!TChain || !SChain)
			{
				continue;
			}
			const TArray<int32> TBones = MhChainBones(TRef, *TChain);
			const TArray<int32> SBones = MhChainBones(SRef, *SChain);
			// A one-bone chain (clavicle) points at its first child in the chain below; use the next chain's start.
			TArray<int32> TPath = TBones, SPath = SBones;
			if (TPath.Num() == 1 || SPath.Num() == 1)
			{
				const FName Next = Name == TEXT("LeftClavicle") ? FName(TEXT("LeftArm")) : Name == TEXT("RightClavicle") ? FName(TEXT("RightArm")) : NAME_None;
				const FBoneChain* TN = Next.IsNone() ? nullptr : TargetRig->GetRetargetChainByName(Next);
				const FBoneChain* SN = Next.IsNone() ? nullptr : SourceRig->GetRetargetChainByName(Next);
				if (!TN || !SN)
				{
					continue;
				}
				TPath = {TBones[0], TRef.FindBoneIndex(TN->StartBone.BoneName)};
				SPath = {SBones[0], SRef.FindBoneIndex(SN->StartBone.BoneName)};
			}
			if (TPath.Num() < 2 || SPath.Num() < 2 || TPath.Contains(INDEX_NONE) || SPath.Contains(INDEX_NONE))
			{
				continue;
			}
			for (int32 k = 0; k + 1 < TPath.Num(); ++k)
			{
				// The source segment at the same place along its chain.
				const float T = TPath.Num() > 2 ? static_cast<float>(k) / (TPath.Num() - 2) : 0.0f;
				const int32 j = FMath::Clamp(FMath::RoundToInt32(T * (SPath.Num() - 2)), 0, SPath.Num() - 2);
				const FVector SDir = (SGlobal[SPath[j + 1]].GetLocation() - SGlobal[SPath[j]].GetLocation()).GetSafeNormal();
				const TArray<FTransform> G = Globals();
				const int32 Bone = TPath[k];
				const FVector TDir = (G[TPath[k + 1]].GetLocation() - G[Bone].GetLocation()).GetSafeNormal();
				if (SDir.IsNearlyZero() || TDir.IsNearlyZero())
				{
					continue;
				}
				const FQuat Turn = FQuat::FindBetweenNormals(TDir, SDir);
				const FQuat NewGlobal = Turn * G[Bone].GetRotation();
				const int32 Parent = TRef.GetParentIndex(Bone);
				const FQuat ParentGlobal = Parent == INDEX_NONE ? FQuat::Identity : G[Parent].GetRotation();
				Local[Bone].SetRotation((ParentGlobal.Inverse() * NewGlobal).GetNormalized());
				++Turned;
			}
		}
		const TArray<FTransform>& RefLocal = TRef.GetRefBonePose();
		for (int32 b = 0; b < TRef.GetNum(); ++b)
		{
			const FQuat Delta = (RefLocal[b].GetRotation().Inverse() * Local[b].GetRotation()).GetNormalized();
			if (!Delta.Equals(FQuat::Identity, 1.0e-4f))
			{
				Controller->SetRotationOffsetForRetargetPoseBone(TRef.GetBoneName(b), Delta, ERetargetSourceOrTarget::Target);
			}
		}
		UE_LOG(LogMakeHumanArt, Display, TEXT("  retarget pose: %d bones turned to the source's directions"), Turned);
	}

	const TCHAR* const MhClipsFolder = TEXT("/Game/Rim3D/Generated/Animations");

	// A Mixamo skeleton's retarget chains by the game rigs' chain names (lower case): bone pairs, and the root.
	bool MhMixamoChains(const FReferenceSkeleton& Ref, TMap<FString, TPair<FName, FName>>& Out, FName& OutRoot)
	{
		FString Prefix;
		bool bMixamo = false;
		for (const TCHAR* Try : {TEXT("mixamorig:"), TEXT("mixamorig_"), TEXT("mixamorig"), TEXT("")})
		{
			if (Ref.FindBoneIndex(FName(*(FString(Try) + TEXT("Hips")))) != INDEX_NONE && Ref.FindBoneIndex(FName(*(FString(Try) + TEXT("LeftUpLeg")))) != INDEX_NONE)
			{
				Prefix = Try;
				bMixamo = true;
				break;
			}
		}
		if (!bMixamo)
		{
			return false;
		}
		const auto B = [&](const FString& Name) { return FName(*(Prefix + Name)); };
		Out.Add(TEXT("root"), {Ref.GetBoneName(0), Ref.GetBoneName(0)});
		Out.Add(TEXT("spine"), {B(TEXT("Spine")), B(TEXT("Spine2"))});
		Out.Add(TEXT("neck"), {B(TEXT("Neck")), B(TEXT("Neck"))});
		Out.Add(TEXT("head"), {B(TEXT("Head")), B(TEXT("Head"))});
		for (const FString S : {FString(TEXT("Left")), FString(TEXT("Right"))})
		{
			Out.Add((S + TEXT("Clavicle")).ToLower(), {B(S + TEXT("Shoulder")), B(S + TEXT("Shoulder"))});
			Out.Add((S + TEXT("Arm")).ToLower(), {B(S + TEXT("Arm")), B(S + TEXT("Hand"))});
			Out.Add((S + TEXT("Leg")).ToLower(), {B(S + TEXT("UpLeg")), B(S + TEXT("Foot"))});
			Out.Add((S + TEXT("Toe")).ToLower(), {B(S + TEXT("ToeBase")), B(S + TEXT("ToeBase"))});
			for (const TCHAR* Finger : {TEXT("Thumb"), TEXT("Index"), TEXT("Middle"), TEXT("Ring"), TEXT("Pinky")})
			{
				if (Ref.FindBoneIndex(B(S + TEXT("Hand") + Finger + TEXT("1"))) != INDEX_NONE)
				{
					Out.Add((S + Finger).ToLower(), {B(S + TEXT("Hand") + Finger + TEXT("1")), B(S + TEXT("Hand") + Finger + TEXT("3"))});
				}
			}
		}
		OutRoot = B(TEXT("Hips"));
		return true;
	}

	// A Mixamo skeleton's IK rig (the mannequin rig's chain names on its bones) and a retargeter from it onto the game's
	// mannequin, in Source. Null if the skeleton is not Mixamo's or the mannequin's assets are missing.
	UIKRetargeter* MhClipRetargeter(IAssetTools& Tools, const FString& Name, USkeletalMesh* Mesh, const FString& Source, int32& Failures,
		USkeletalMesh*& TargetMesh)
	{
		TMap<FString, TPair<FName, FName>> Chains;
		FName Root;
		if (!MhMixamoChains(Mesh->GetRefSkeleton(), Chains, Root))
		{
			UE_LOG(LogMakeHumanArt, Error, TEXT("Clip %s: not a Mixamo skeleton (only Mixamo clips are set up)"), *Name);
			return nullptr;
		}
		UIKRigDefinition* Target = LoadObject<UIKRigDefinition>(nullptr, MhSourceRig);
		TargetMesh = Target ? Target->GetPreviewMesh() : nullptr;
		UIKRetargeter* TemplateRetargeter = LoadObject<UIKRetargeter>(nullptr, MhTemplateRetargeter);
		if (!Target || !TargetMesh || !TemplateRetargeter)
		{
			UE_LOG(LogMakeHumanArt, Error, TEXT("Clip %s: no mannequin IK rig, its mesh or the template retargeter"), *Name);
			return nullptr;
		}
		// Already made (a -Clips -Only run adds clips to what is there): used as it is.
		const FString MadeName = TEXT("RTG_") + Name + TEXT("_to_UEFN");
		if (UIKRetargeter* Made = LoadObject<UIKRetargeter>(nullptr, *(Source / MadeName + TEXT(".") + MadeName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Made;
		}
		// The clip's IK rig: the mannequin rig's chain names on the Mixamo bones.
		UIKRigDefinition* Rig = Cast<UIKRigDefinition>(Tools.CreateAsset(TEXT("IK_") + Name + TEXT("_Source"), Source, UIKRigDefinition::StaticClass(), NewObject<UIKRigDefinitionFactory>()));
		UIKRigController* RigController = UIKRigController::GetController(Rig);
		RigController->SetSkeletalMesh(Mesh);
		int32 Mapped = 0;
		for (const FBoneChain& Chain : Target->GetRetargetChains())
		{
			if (const TPair<FName, FName>* Bones = Chains.Find(Chain.ChainName.ToString().ToLower()))
			{
				RigController->AddRetargetChain(Chain.ChainName, Bones->Key, Bones->Value, NAME_None);
				++Mapped;
			}
		}
		RigController->SetRetargetRoot(Root);
		Failures += MhSave(Rig) ? 0 : 1;
		UE_LOG(LogMakeHumanArt, Display, TEXT("Clip %s: IK rig with %d of the mannequin's %d chains"), *Name, Mapped, Target->GetRetargetChains().Num());
		// The retargeter: the template's ops, Mixamo to the mannequin.
		UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Tools.DuplicateAsset(TEXT("RTG_") + Name + TEXT("_to_UEFN"), Source, TemplateRetargeter));
		UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
		Controller->SetIKRig(ERetargetSourceOrTarget::Source, Rig);
		Controller->SetPreviewMesh(ERetargetSourceOrTarget::Source, Mesh);
		Controller->SetIKRig(ERetargetSourceOrTarget::Target, Target);
		Controller->SetPreviewMesh(ERetargetSourceOrTarget::Target, TargetMesh);
		Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, Rig);
		Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, Target);
		for (int32 Op = 0; Op < Controller->GetNumRetargetOps(); ++Op)
		{
			FInstancedStruct* Struct = Controller->GetRetargetOpStructAtIndex(Op);
			const FString Type = Struct && Struct->GetScriptStruct() ? Struct->GetScriptStruct()->GetName() : FString();
			// The IK step off as well (-ClipIK keeps it): the mannequin's full-body IK bent its upper spine to reach the hand
			// goals, so every clip came out leaning 30 to 90 degrees more than its source (bodies folded at the waist, or
			// "sideways into the ground"). FK alone matches the source's lean; each body's own retargeter still plants feet.
			const bool bNoIK = !FParse::Param(FCommandLine::Get(), TEXT("ClipIK")) && (Type == TEXT("IKRetargetRunIKRigOp") || Type == TEXT("IKRetargetSpeedPlantingOp"));
			if (Type == TEXT("IKRetargetBlendToSourceOp") || Type == TEXT("IKRetargetOffsetGoalsOp") || bNoIK)
			{
				Controller->SetRetargetOpEnabled(Op, false);
			}
			FStructProperty* SettingsProperty = Struct && Struct->GetScriptStruct() ? FindFProperty<FStructProperty>(Struct->GetScriptStruct(), TEXT("Settings")) : nullptr;
			void* Settings = SettingsProperty ? SettingsProperty->ContainerPtrToValuePtr<void>(Struct->GetMutableMemory()) : nullptr;
			for (const TPair<const TCHAR*, FName>& Bone : {TPair<const TCHAR*, FName>(TEXT("SourcePelvisBone"), Root), TPair<const TCHAR*, FName>(TEXT("TargetPelvisBone"), FName(TEXT("pelvis")))})
			{
				FStructProperty* Property = SettingsProperty ? FindFProperty<FStructProperty>(SettingsProperty->Struct, Bone.Key) : nullptr;
				if (Property && Property->Struct == FBoneReference::StaticStruct())
				{
					Property->ContainerPtrToValuePtr<FBoneReference>(Settings)->BoneName = Bone.Value;
				}
			}
		}
		Controller->AutoMapChains(EAutoMapChainType::Exact, true);
		// Mixamo stands in a T pose, the mannequin in an A pose: the mannequin's retarget pose is turned to match.
		TArray<FName> Poses;
		Controller->GetRetargetPoses(ERetargetSourceOrTarget::Target).GetKeys(Poses);
		const FName Current = Controller->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target);
		for (const FName& Pose : Poses)
		{
			Controller->SetCurrentRetargetPose(Pose, ERetargetSourceOrTarget::Target);
			Controller->ResetRetargetPose(Pose, {}, ERetargetSourceOrTarget::Target);
			// Limbs only: both spines stand upright already, and turning the mannequin's five spine bones to the source's
			// curved three left the mannequin's retarget pose leaning, so every clip came out leaning 30 to 60 degrees.
			MhAlignRetargetPose(Controller, Rig, Mesh, Target, TargetMesh, false);
		}
		Controller->SetCurrentRetargetPose(Current, ERetargetSourceOrTarget::Target);
		Failures += MhSave(Retargeter) ? 0 : 1;
		return Retargeter;
	}

	// One animation clip (assets/animations.json): its FBX imported with its skeleton, an IK rig for that skeleton (a
	// Mixamo one), a retargeter from it onto the game's mannequin (UEFN), and the clip retargeted onto the mannequin as
	// <folder>/A_<name>. The pawns play mannequin animations, and every body takes them through its own retargeter.
	int32 MhMakeClip(IAssetTools& Tools, const FString& Name, const FString& Fbx)
	{
		const FString Folder = FString(MhClipsFolder) / Name;
		const FString Source = Folder / TEXT("Source");
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = Fbx;
		Task->DestinationPath = Source;
		Task->DestinationName = TEXT("SK_") + Name + TEXT("_Source");
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = false;
		UFbxFactory* Factory = NewObject<UFbxFactory>();
		Factory->ImportUI = NewObject<UFbxImportUI>(Factory);
		Factory->ImportUI->bImportMesh = true;
		Factory->ImportUI->bImportAsSkeletal = true;
		Factory->ImportUI->MeshTypeToImport = FBXIT_SkeletalMesh;
		Factory->ImportUI->bImportAnimations = true;
		Factory->ImportUI->bImportMaterials = false;
		Factory->ImportUI->bImportTextures = false;
		Factory->ImportUI->bCreatePhysicsAsset = false;
		Factory->ImportUI->SkeletalMeshImportData->bImportMorphTargets = false;
		Factory->ImportUI->SkeletalMeshImportData->bConvertScene = true;
		Task->Factory = Factory;
		Task->Options = Factory->ImportUI;
		Tools.ImportAssetTasks({Task});
		USkeletalMesh* Mesh = nullptr;
		for (UObject* Object : Task->GetObjects())
		{
			Mesh = Mesh ? Mesh : Cast<USkeletalMesh>(Object);
		}
		if (!Mesh)
		{
			UE_LOG(LogMakeHumanArt, Error, TEXT("Clip %s: importing %s made no skeletal mesh"), *Name, *Fbx);
			return 1;
		}
		int32 Failures = 0;
		Failures += MhSave(Mesh) ? 0 : 1;
		if (Mesh->GetSkeleton())
		{
			Failures += MhSave(Mesh->GetSkeleton()) ? 0 : 1;
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Found;
		Registry.ScanPathsSynchronous({Source}, true);
		Registry.GetAssetsByPath(FName(*Source), Found, true);
		TArray<FAssetData> Sequences;
		for (const FAssetData& Asset : Found)
		{
			if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset.GetAsset()))
			{
				Failures += MhSave(Sequence) ? 0 : 1;
				Sequences.Add(FAssetData(Sequence));
				UE_LOG(LogMakeHumanArt, Display, TEXT("Clip %s: %s, %.2f s, %d keys"), *Name, *Sequence->GetName(), Sequence->GetPlayLength(),
					Sequence->GetNumberOfSampledKeys());
			}
		}
		if (Sequences.IsEmpty())
		{
			UE_LOG(LogMakeHumanArt, Error, TEXT("Clip %s: no animation in %s"), *Name, *Fbx);
			return Failures + 1;
		}
		USkeletalMesh* TargetMesh = nullptr;
		UIKRetargeter* Retargeter = MhClipRetargeter(Tools, Name, Mesh, Source, Failures, TargetMesh);
		if (!Retargeter)
		{
			return Failures + 1;
		}
		// The clip on the mannequin.
		FIKRetargetBatchOperationInputs Inputs;
		Inputs.AssetsToRetarget = Sequences;
		Inputs.SourceMesh = Mesh;
		Inputs.TargetMesh = TargetMesh;
		Inputs.IKRetargetAsset = Retargeter;
		Inputs.TargetPath = Folder;
		Inputs.Prefix = TEXT("A_");
		Inputs.bIncludeReferencedAssets = false;
		Inputs.bOverwriteExistingFiles = true;
		const TArray<FAssetData> Made = UIKRetargetBatchOperation::RunBatchRetarget(Inputs);
		int32 Saved = 0;
		for (const FAssetData& Asset : Made)
		{
			if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset.GetAsset()))
			{
				// One name per clip for the game: a copy as <folder>/A_<name>.
				UAnimSequence* Named = Cast<UAnimSequence>(Tools.DuplicateAsset(TEXT("A_") + Name, Folder, Sequence));
				Sequence = Named ? Named : Sequence;
				Failures += MhSave(Sequence) ? 0 : 1;
				++Saved;
				UE_LOG(LogMakeHumanArt, Display, TEXT("Clip %s: retargeted onto the mannequin as %s (%s), %.2f s"), *Name, *Sequence->GetName(),
					*Sequence->GetOutermost()->GetName(), Sequence->GetPlayLength());
				break;
			}
		}
		if (Saved == 0)
		{
			UE_LOG(LogMakeHumanArt, Error, TEXT("Clip %s: the batch retarget made nothing"), *Name);
			++Failures;
		}
		return Failures;
	}

	// A set of clips on one skeleton (assets/animations.json "sets": Mixamo's Y Bot): the skeleton's mesh imported once
	// with one retargeter, every clip's FBX imported as an animation of that skeleton, and all retargeted onto the
	// mannequin in one batch, as <folder>/<category>/A_<name>.
	int32 MhMakeClipSet(IAssetTools& Tools, const FString& SetName, const FString& SkeletonFbx, const TArray<TTuple<FString, FString, FString>>& Clips)
	{
		const FString Source = FString(MhClipsFolder) / SetName / TEXT("Source");
		int32 Failures = 0;
		UAssetImportTask* MeshTask = NewObject<UAssetImportTask>();
		MeshTask->Filename = SkeletonFbx;
		MeshTask->DestinationPath = Source;
		MeshTask->DestinationName = TEXT("SK_") + SetName;
		MeshTask->bAutomated = true;
		MeshTask->bReplaceExisting = true;
		MeshTask->bSave = false;
		UFbxFactory* MeshFactory = NewObject<UFbxFactory>();
		MeshFactory->ImportUI = NewObject<UFbxImportUI>(MeshFactory);
		MeshFactory->ImportUI->bImportMesh = true;
		MeshFactory->ImportUI->bImportAsSkeletal = true;
		MeshFactory->ImportUI->MeshTypeToImport = FBXIT_SkeletalMesh;
		MeshFactory->ImportUI->bImportAnimations = false;
		MeshFactory->ImportUI->bImportMaterials = false;
		MeshFactory->ImportUI->bImportTextures = false;
		MeshFactory->ImportUI->bCreatePhysicsAsset = false;
		MeshFactory->ImportUI->SkeletalMeshImportData->bConvertScene = true;
		MeshTask->Factory = MeshFactory;
		MeshTask->Options = MeshFactory->ImportUI;
		Tools.ImportAssetTasks({MeshTask});
		USkeletalMesh* Mesh = nullptr;
		for (UObject* Object : MeshTask->GetObjects())
		{
			Mesh = Mesh ? Mesh : Cast<USkeletalMesh>(Object);
		}
		if (!Mesh || !Mesh->GetSkeleton())
		{
			UE_LOG(LogMakeHumanArt, Error, TEXT("Clip set %s: importing %s made no skeletal mesh"), *SetName, *SkeletonFbx);
			return 1;
		}
		Failures += MhSave(Mesh) ? 0 : 1;
		Failures += MhSave(Mesh->GetSkeleton()) ? 0 : 1;
		USkeletalMesh* TargetMesh = nullptr;
		UIKRetargeter* Retargeter = MhClipRetargeter(Tools, SetName, Mesh, Source, Failures, TargetMesh);
		if (!Retargeter)
		{
			return Failures + 1;
		}
		// Every clip onto that skeleton, then retargeted, a category at a time.
		TMap<FString, TArray<TPair<FString, UAnimSequence*>>> ByCategory;
		int32 Imported = 0;
		for (const TTuple<FString, FString, FString>& Clip : Clips)
		{
			const FString& Name = Clip.Get<0>();
			const FString& Category = Clip.Get<1>();
			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = Clip.Get<2>();
			Task->DestinationPath = Source / TEXT("Raw");
			Task->DestinationName = Name;
			Task->bAutomated = true;
			Task->bReplaceExisting = true;
			Task->bSave = false;
			UFbxFactory* Factory = NewObject<UFbxFactory>();
			Factory->ImportUI = NewObject<UFbxImportUI>(Factory);
			Factory->ImportUI->bImportMesh = false;
			Factory->ImportUI->bImportAsSkeletal = true;
			Factory->ImportUI->MeshTypeToImport = FBXIT_Animation;
			Factory->ImportUI->bImportAnimations = true;
			Factory->ImportUI->bImportMaterials = false;
			Factory->ImportUI->bImportTextures = false;
			Factory->ImportUI->Skeleton = Mesh->GetSkeleton();
			Factory->ImportUI->AnimSequenceImportData->bConvertScene = true;
			Task->Factory = Factory;
			Task->Options = Factory->ImportUI;
			Tools.ImportAssetTasks({Task});
			UAnimSequence* Sequence = nullptr;
			for (UObject* Object : Task->GetObjects())
			{
				Sequence = Sequence ? Sequence : Cast<UAnimSequence>(Object);
			}
			if (!Sequence)
			{
				// An FBX with a skinned mesh in it (Mixamo's "with skin" downloads) makes <name>_Anim beside the mesh.
				const FString AnimPath = Source / TEXT("Raw") / (Name + TEXT("_Anim"));
				Sequence = LoadObject<UAnimSequence>(nullptr, *(AnimPath + TEXT(".") + Name + TEXT("_Anim")));
				if (Sequence && Sequence->GetSkeleton() != Mesh->GetSkeleton())
				{
					UE_LOG(LogMakeHumanArt, Warning, TEXT("Clip %s: on its own skeleton, not the set's"), *Name);
					Sequence = nullptr;
				}
			}
			if (!Sequence)
			{
				UE_LOG(LogMakeHumanArt, Warning, TEXT("Clip %s: no animation from %s"), *Name, *Clip.Get<2>());
				continue;
			}
			MhSave(Sequence);  // the raw clip on the set's skeleton, kept for checks (-ProbeAnim)
			ByCategory.FindOrAdd(Category).Add({Name, Sequence});
			++Imported;
		}
		int32 Saved = 0;
		for (TPair<FString, TArray<TPair<FString, UAnimSequence*>>>& Category : ByCategory)
		{
			const FString Folder = FString(MhClipsFolder) / SetName / Category.Key.Replace(TEXT(" "), TEXT("")).Replace(TEXT("/"), TEXT(""));
			FIKRetargetBatchOperationInputs Inputs;
			for (const TPair<FString, UAnimSequence*>& Clip : Category.Value)
			{
				Inputs.AssetsToRetarget.Add(FAssetData(Clip.Value));
			}
			Inputs.SourceMesh = Mesh;
			Inputs.TargetMesh = TargetMesh;
			Inputs.IKRetargetAsset = Retargeter;
			Inputs.TargetPath = Folder;
			Inputs.Prefix = TEXT("A_");
			Inputs.bIncludeReferencedAssets = false;
			Inputs.bOverwriteExistingFiles = true;
			for (const FAssetData& Asset : UIKRetargetBatchOperation::RunBatchRetarget(Inputs))
			{
				if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset.GetAsset()))
				{
					Failures += MhSave(Sequence) ? 0 : 1;
					++Saved;
				}
			}
			UE_LOG(LogMakeHumanArt, Display, TEXT("Clip set %s: %s, %d clips"), *SetName, *Category.Key, Category.Value.Num());
		}
		UE_LOG(LogMakeHumanArt, Display, TEXT("Clip set %s: %d of %d clips imported, %d retargeted onto the mannequin"), *SetName, Imported, Clips.Num(), Saved);
		return Failures;
	}

	// One character: the mesh, its skin, its IK rig and the retargeter from the game's mannequin.
	int32 MhMakeCharacter(IAssetTools& Tools, const FMakeHumanCharacterFiles& Files, const TSharedPtr<FJsonObject>& Config, const FString& Folder)
	{
		int32 Failures = 0;
		USkeletalMesh* Mesh = MhImportFbx(Tools, Files.Fbx, Folder, MakeHumanMeshName(Files.Name));
		if (!Mesh)
		{
			return 1;
		}
		UE_LOG(LogMakeHumanArt, Display, TEXT("%s: %d bones, %d LODs, %d material slots, %d morphs"), *Mesh->GetName(), Mesh->GetRefSkeleton().GetNum(),
			Mesh->GetLODNum(), Mesh->GetMaterials().Num(), Mesh->GetMorphTargets().Num());

		UMaterialInterface* SkinBase = MhSkinDetailMaterial(Tools, LoadObject<UMaterialInterface>(nullptr, *PawnLooksAssetPath(PawnLooksSkinMaterial)));
		{
			FString Bones;
			for (int32 b = 0; b < Mesh->GetRefSkeleton().GetNum(); ++b)
			{
				Bones += Mesh->GetRefSkeleton().GetBoneName(b).ToString() + TEXT(" ");
			}
			UE_LOG(LogMakeHumanArt, Display, TEXT("  bones: %s"), *Bones);
		}
		// A model that brings its own textures: one material instance per slot, from the "materials" map
		// (slot name -> {"color", "normal", "opacity", "skin"}; texture files relative to "textures").
		const TSharedPtr<FJsonObject>* MaterialMap = nullptr;
		TSharedPtr<FJsonObject> Manifest;
		{
			FString Text;
			if (FFileHelper::LoadFileToString(Text, *(FPaths::GetPath(Files.Fbx) / TEXT("materials.json"))) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Manifest) && Manifest)
			{
				MaterialMap = &Manifest;
			}
		}
		if (MaterialMap || (Config && Config->TryGetObjectField(TEXT("materials"), MaterialMap)))
		{
			FString TextureDir;
			if (Config)
			{
				Config->TryGetStringField(TEXT("textures"), TextureDir);
			}
			TMap<FString, UTexture2D*> Imported;
			const auto Texture = [&](const FString& File, bool bColor) -> UTexture2D*
			{
				if (File.IsEmpty())
				{
					return nullptr;
				}
				if (UTexture2D** Found = Imported.Find(File))
				{
					return *Found;
				}
				const FString Name = TEXT("T_") + Files.Name + TEXT("_") + FPaths::GetBaseFilename(File).Replace(TEXT(" "), TEXT("_")).Replace(TEXT("."), TEXT("_"));
				UTexture2D* T = MhImportTexture(FPaths::IsRelative(File) && !TextureDir.IsEmpty() ? TextureDir / File : File, Folder, Name, bColor);
				if (T)
				{
					MhSave(T);
				}
				Imported.Add(File, T);
				return T;
			};
			for (int32 Slot = 0; Slot < Mesh->GetMaterials().Num(); ++Slot)
			{
				FSkeletalMaterial& Material = Mesh->GetMaterials()[Slot];
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				const FString Key = Material.MaterialSlotName.ToString();
				if (!(*MaterialMap)->TryGetObjectField(Key, Entry))
				{
					UE_LOG(LogMakeHumanArt, Warning, TEXT("  slot %d %s: not in the materials map"), Slot, *Key);
					continue;
				}
				FString Opacity;
				(*Entry)->TryGetStringField(TEXT("opacity"), Opacity);
				bool bSkin = false;
				(*Entry)->TryGetBoolField(TEXT("skin"), bSkin);
				bool bAlpha = false;
				(*Entry)->TryGetBoolField(TEXT("alpha"), bAlpha);
				FString ColorFile, NormalFile;
				(*Entry)->TryGetStringField(TEXT("color"), ColorFile);
				(*Entry)->TryGetStringField(TEXT("normal"), NormalFile);
				if (bSkin && SkinBase)
				{
					// Skin: our skin material, so the pawn's skin colour drives it like the MetaHumans'.
					UTexture2D* SkinColor = Texture(ColorFile.IsEmpty() ? Files.SkinTexture : ColorFile, true);
					auto* SkinFactory = NewObject<UMaterialInstanceConstantFactoryNew>();
					SkinFactory->InitialParent = SkinBase;
					auto* SkinInstance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(MakeHumanSkinName(Files.Name), Folder, UMaterialInstanceConstant::StaticClass(), SkinFactory));
					if (SkinInstance && SkinColor)
					{
						SkinInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamBaseColor), SkinColor);
						SkinInstance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamSourceAverage), MhAverage(SkinColor));
						UTexture2D* SkinNormal = Texture(NormalFile, false);
						UTexture* NormalToUse = SkinNormal ? static_cast<UTexture*>(SkinNormal) : LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));
						SkinInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamNormal), NormalToUse);
						// The areolae and genitals, darker and warmer by the pawn's pigment (custom primitive data 3).
						// The markings atlas (tools/makehuman/markings.py), shared by every skin.
						{
							const FString AtlasFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MakeHuman/Markings/markings.png"));
							static TWeakObjectPtr<UTexture2D> AtlasTexture;
							if (!AtlasTexture.IsValid() && FPaths::FileExists(AtlasFile))
							{
								AtlasTexture = MhImportTexture(AtlasFile, MakeHumanFolder, TEXT("T_MH_Markings"), true);
								if (AtlasTexture.IsValid())
								{
									AtlasTexture->SRGB = false;
									AtlasTexture->CompressionSettings = TC_Default;
									AtlasTexture->PostEditChange();
									MhSave(AtlasTexture.Get());
								}
							}
							if (AtlasTexture.IsValid())
							{
								SkinInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MarkAtlas")), AtlasTexture.Get());
							}
						}
						FString WrinkleFile;
						UTexture2D* WrinkleTexture = nullptr;
						if ((*Entry)->TryGetStringField(TEXT("wrinkles"), WrinkleFile) && (WrinkleTexture = Texture(WrinkleFile, true)) != nullptr)
						{
							SkinInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("WrinkleMap")), WrinkleTexture);
						}
						FString PigmentFile;
						if ((*Entry)->TryGetStringField(TEXT("pigment"), PigmentFile))
						{
							if (UTexture2D* Mask = Texture(PigmentFile, true))  // a colour texture, as the parameter's default
							{
								SkinInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("PigmentMask")), Mask);
							}
						}
						SkinInstance->PostEditChange();
						Failures += MhSave(SkinInstance) ? 0 : 1;
						Material.MaterialInterface = SkinInstance;
						// The 4K skin (tools/makehuman/skin_4k.py: skin_4k.png, skin_normal_4k.png, skin_rough_4k.png beside the
						// FBX): a second instance, MI_MH_<name>_Skin4K, the creator's switch. The 2K one stays the mesh's own.
						const FString Dir = FPaths::GetPath(Files.Fbx);
						if (FPaths::FileExists(Dir / TEXT("skin_4k.png")))
						{
							auto* Factory4K = NewObject<UMaterialInstanceConstantFactoryNew>();
							Factory4K->InitialParent = SkinBase;
							auto* Skin4K = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(MakeHumanSkinName(Files.Name) + TEXT("4K"), Folder, UMaterialInstanceConstant::StaticClass(), Factory4K));
							UTexture2D* Color4K = MhImportTexture(Dir / TEXT("skin_4k.png"), Folder, TEXT("T_") + Files.Name + TEXT("_Skin4K"), true);
							UTexture2D* Normal4K = FPaths::FileExists(Dir / TEXT("skin_normal_4k.png")) ? MhImportTexture(Dir / TEXT("skin_normal_4k.png"), Folder, TEXT("T_") + Files.Name + TEXT("_SkinNormal4K"), false) : nullptr;
							UTexture2D* Rough4K = FPaths::FileExists(Dir / TEXT("skin_rough_4k.png")) ? MhImportTexture(Dir / TEXT("skin_rough_4k.png"), Folder, TEXT("T_") + Files.Name + TEXT("_SkinRough4K"), true) : nullptr;
							if (Skin4K && Color4K)
							{
								for (UTexture2D* T : {Color4K, Normal4K, Rough4K})
								{
									if (T)
									{
										Failures += MhSave(T) ? 0 : 1;
									}
								}
								Skin4K->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamBaseColor), Color4K);
								Skin4K->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamSourceAverage), MhAverage(Color4K));
								Skin4K->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamNormal), Normal4K ? static_cast<UTexture*>(Normal4K) : NormalToUse);
								if (Rough4K)
								{
									Skin4K->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("RoughnessMap")), Rough4K);
									Skin4K->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("RoughnessMapAmount")), 0.35f);
								}
								if (WrinkleTexture)
								{
									Skin4K->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("WrinkleMap")), WrinkleTexture);
								}
								UTexture* AtlasValue = nullptr;
								if (SkinInstance->GetTextureParameterValue(FMaterialParameterInfo(TEXT("MarkAtlas")), AtlasValue) && AtlasValue)
								{
									Skin4K->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("MarkAtlas")), AtlasValue);
								}
								FMaterialParameterInfo PigmentInfo(TEXT("PigmentMask"));
								UTexture* PigmentTexture = nullptr;
								if (SkinInstance->GetTextureParameterValue(PigmentInfo, PigmentTexture) && PigmentTexture)
								{
									Skin4K->SetTextureParameterValueEditorOnly(PigmentInfo, PigmentTexture);
								}
								Skin4K->PostEditChange();
								Failures += MhSave(Skin4K) ? 0 : 1;
								UE_LOG(LogMakeHumanArt, Display, TEXT("  4K skin %s"), *Skin4K->GetName());
							}
						}
						UE_LOG(LogMakeHumanArt, Display, TEXT("  slot %d %s: skin %s"), Slot, *Key, *SkinInstance->GetName());
						continue;
					}
				}
				const bool bMasked = !Opacity.IsEmpty() || bAlpha;
				bool bPubic = false;
				(*Entry)->TryGetBoolField(TEXT("pubic"), bPubic);
				const bool bEye = Key.Contains(TEXT("high_poly")) || Key.Contains(TEXT("low_poly"));
				bool bMetal = false;
				(*Entry)->TryGetBoolField(TEXT("metal"), bMetal);
				UMaterial* Base = bPubic ? MhPubicMaterial(Tools) : bMetal ? MhMetalMaterial(Tools) : bEye ? MhEyeMaterial(Tools) : MhBaseMaterial(Tools, !bMasked ? (bSkin ? TEXT("M_MH_Skin") : TEXT("M_MH_Lit")) : (Opacity.IsEmpty() ? TEXT("M_MH_MaskedA") : TEXT("M_MH_Masked")),
					bMasked, bSkin, Opacity.IsEmpty() && bAlpha);
				auto* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
				Factory->InitialParent = Base;
				auto* Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(FString::Printf(TEXT("MI_%s_%d"), *Files.Name, Slot), Folder, UMaterialInstanceConstant::StaticClass(), Factory));
				if (!Instance)
				{
					++Failures;
					continue;
				}
				Instance->SetParentEditorOnly(Base);  // an instance already on disk keeps its old parent otherwise
				const TArray<TSharedPtr<FJsonValue>>* TintValues = nullptr;
				if ((*Entry)->TryGetArrayField(TEXT("tint"), TintValues) && TintValues->Num() >= 3)
				{
					Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Tint")),
						FLinearColor((*TintValues)[0]->AsNumber(), (*TintValues)[1]->AsNumber(), (*TintValues)[2]->AsNumber()));
				}
				if (UTexture2D* T = Texture(ColorFile, true))
				{
					Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("BaseColor")), T);
				}
				if (UTexture2D* T = Texture(NormalFile, false))
				{
					Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Normal")), T);
				}
				if (UTexture2D* T = Texture(Opacity, false))
				{
					T->SRGB = false;
					T->CompressionSettings = TC_Masks;
					T->PostEditChange();
					MhSave(T);
					Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Opacity")), T);
				}
				Instance->PostEditChange();
				Failures += MhSave(Instance) ? 0 : 1;
				Material.MaterialInterface = Instance;
				UE_LOG(LogMakeHumanArt, Display, TEXT("  slot %d %s: %s"), Slot, *Key, *Instance->GetName());
			}
		}

		// Skin on our skin material (the pawn's skin colour drives it, as on the MetaHumans).
		UTexture2D* Color = Files.SkinTexture.IsEmpty() ? nullptr : MhImportTexture(Files.SkinTexture, Folder, TEXT("T_") + Files.Name + TEXT("_Skin"), true);
		UTexture* FlatNormal = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));
		if (Color)
		{
			Failures += MhSave(Color) ? 0 : 1;
		}
		UMaterialInstanceConstant* Skin = nullptr;
		if (MaterialMap)
		{
		}
		else if (SkinBase && Color)
		{
			auto* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
			Factory->InitialParent = SkinBase;
			Skin = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(MakeHumanSkinName(Files.Name), Folder, UMaterialInstanceConstant::StaticClass(), Factory));
			if (Skin)
			{
				Skin->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamBaseColor), Color);
				Skin->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamSourceAverage), MhAverage(Color));
				if (FlatNormal)
				{
					Skin->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamNormal), FlatNormal);
				}
				Skin->PostEditChange();
				Failures += MhSave(Skin) ? 0 : 1;
			}
		}
		else
		{
			UE_LOG(LogMakeHumanArt, Warning, TEXT("%s: no skin material (run PawnLooks first; skin texture %s)"), *Files.Name, *Files.SkinTexture);
		}
		for (int32 Slot = 0; Slot < Mesh->GetMaterials().Num(); ++Slot)
		{
			FSkeletalMaterial& Material = Mesh->GetMaterials()[Slot];
			const FString SlotName = Material.MaterialSlotName.ToString();
			UE_LOG(LogMakeHumanArt, Display, TEXT("  slot %d %s"), Slot, *SlotName);
			if (Skin && !MaterialMap && !SlotName.Contains(TEXT("hair"), ESearchCase::IgnoreCase))
			{
				Material.MaterialInterface = Skin;
			}
		}
		Mesh->PostEditChange();
		Failures += MhSave(Mesh) ? 0 : 1;
		if (Mesh->GetSkeleton())
		{
			Failures += MhSave(Mesh->GetSkeleton()) ? 0 : 1;
		}
		if (Mesh->GetPhysicsAsset())
		{
			Failures += MhSave(Mesh->GetPhysicsAsset()) ? 0 : 1;
		}

		// The IK rig and the retargeter, copies of the UE4 mannequin's.
		UIKRigDefinition* TemplateRig = LoadObject<UIKRigDefinition>(nullptr, MhTemplateRig);
		UIKRetargeter* TemplateRetargeter = LoadObject<UIKRetargeter>(nullptr, MhTemplateRetargeter);
		// Which bones make each retarget chain: the UE4 mannequin's names (MPFB's game engine skeleton), a Mixamo
		// skeleton's (built in), or a config's own ("chains": {"LeftArm": ["start", "end"], ...}, "retargetRoot").
		FString RetargetRoot = TEXT("pelvis");
		const TSharedPtr<FJsonObject>* Chains = nullptr;
		TSharedPtr<FJsonObject> MixamoChains;
		{
			// A Mixamo skeleton (MPFB's mixamo rig, or Mixamo's own): its bone names, with ":" or "_" after "mixamorig".
			const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
			// (Unreal's FBX import drops the "mixamorig:" namespace.)
			FString Prefix;
			bool bMixamo = false;
			for (const TCHAR* Try : {TEXT("mixamorig:"), TEXT("mixamorig_"), TEXT("mixamorig"), TEXT("")})
			{
				if (Ref.FindBoneIndex(FName(*(FString(Try) + TEXT("Hips")))) != INDEX_NONE && Ref.FindBoneIndex(FName(*(FString(Try) + TEXT("LeftUpLeg")))) != INDEX_NONE)
				{
					Prefix = Try;
					bMixamo = true;
					break;
				}
			}
			if (bMixamo && !(Config && Config->HasField(TEXT("chains"))))
			{
				const auto B = [&](const TCHAR* Name) { return MakeShared<FJsonValueString>(Prefix + Name); };
				const auto Pair = [&](const TCHAR* A, const TCHAR* Bn) { return TArray<TSharedPtr<FJsonValue>>{B(A), B(Bn)}; };
				MixamoChains = MakeShared<FJsonObject>();
				const FString Top = Ref.GetBoneName(0).ToString();
				MixamoChains->SetArrayField(TEXT("root"), TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(Top), MakeShared<FJsonValueString>(Top)});
				MixamoChains->SetArrayField(TEXT("Spine"), Pair(TEXT("Spine"), TEXT("Spine2")));
				MixamoChains->SetArrayField(TEXT("Neck"), Pair(TEXT("Neck"), TEXT("Neck")));
				MixamoChains->SetArrayField(TEXT("head"), Pair(TEXT("Head"), TEXT("Head")));
				for (const TCHAR* Side : {TEXT("Left"), TEXT("Right")})
				{
					const FString S(Side);
					MixamoChains->SetArrayField(S + TEXT("Clavicle"), Pair(*(S + TEXT("Shoulder")), *(S + TEXT("Shoulder"))));
					MixamoChains->SetArrayField(S + TEXT("Arm"), Pair(*(S + TEXT("Arm")), *(S + TEXT("Hand"))));
					MixamoChains->SetArrayField(S + TEXT("Leg"), Pair(*(S + TEXT("UpLeg")), *(S + TEXT("Foot"))));
					MixamoChains->SetArrayField(S + TEXT("Toe"), Pair(*(S + TEXT("ToeBase")), *(S + TEXT("ToeBase"))));
					for (const TCHAR* Finger : {TEXT("Thumb"), TEXT("Index"), TEXT("Middle"), TEXT("Ring"), TEXT("Pinky")})
					{
						MixamoChains->SetArrayField(S + Finger, Pair(*(S + TEXT("Hand") + Finger + TEXT("1")), *(S + TEXT("Hand") + Finger + TEXT("3"))));
					}
				}
				Chains = &MixamoChains;
				RetargetRoot = Prefix + TEXT("Hips");
				UE_LOG(LogMakeHumanArt, Display, TEXT("  Mixamo skeleton (%s): built-in chains"), *Prefix);
			}
		}
		if (!Chains && Config && Config->TryGetObjectField(TEXT("chains"), Chains))
		{
			Config->TryGetStringField(TEXT("retargetRoot"), RetargetRoot);
		}
		UIKRigDefinition* Rig = nullptr;
		if (Chains)
		{
			// Another skeleton: a fresh IK rig with the template's chain names (so the retargeter maps them), FK only.
			Rig = Cast<UIKRigDefinition>(Tools.CreateAsset(MakeHumanRigName(Files.Name), Folder, UIKRigDefinition::StaticClass(), NewObject<UIKRigDefinitionFactory>()));
		}
		else if (TemplateRig)
		{
			Rig = Cast<UIKRigDefinition>(Tools.DuplicateAsset(MakeHumanRigName(Files.Name), Folder, TemplateRig));
		}
		UIKRetargeter* Retargeter = TemplateRetargeter ? Cast<UIKRetargeter>(Tools.DuplicateAsset(MakeHumanRetargeterName(Files.Name), Folder, TemplateRetargeter)) : nullptr;
		if (!Rig || !Retargeter)
		{
			UE_LOG(LogMakeHumanArt, Error, TEXT("Could not make the IK rig or copy the UE4 mannequin's retargeter"));
			return Failures + 1;
		}
		UIKRigController* RigController = UIKRigController::GetController(Rig);
		RigController->SetSkeletalMesh(Mesh);
		if (Chains && TemplateRig)
		{
			for (const FBoneChain& Chain : TemplateRig->GetRetargetChains())
			{
				const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
				if ((*Chains)->TryGetArrayField(Chain.ChainName.ToString(), Bones) && Bones->Num() == 2)
				{
					RigController->AddRetargetChain(Chain.ChainName, FName(*(*Bones)[0]->AsString()), FName(*(*Bones)[1]->AsString()), NAME_None);
				}
			}
		}
		RigController->SetRetargetRoot(FName(*RetargetRoot));
		UE_LOG(LogMakeHumanArt, Display, TEXT("  IK rig %s: %d retarget chains"), *Rig->GetName(), Rig->GetRetargetChains().Num());
		for (const FBoneChain& Chain : Rig->GetRetargetChains())
		{
			const bool bStart = Mesh->GetRefSkeleton().FindBoneIndex(Chain.StartBone.BoneName) != INDEX_NONE;
			const bool bEnd = Mesh->GetRefSkeleton().FindBoneIndex(Chain.EndBone.BoneName) != INDEX_NONE;
			UE_LOG(LogMakeHumanArt, Display, TEXT("    chain %s: %s (%s) to %s (%s)"), *Chain.ChainName.ToString(), *Chain.StartBone.BoneName.ToString(),
				bStart ? TEXT("ok") : TEXT("MISSING"), *Chain.EndBone.BoneName.ToString(), bEnd ? TEXT("ok") : TEXT("MISSING"));
		}
		Failures += MhSave(Rig) ? 0 : 1;
		UIKRetargeterController* RetargetController = UIKRetargeterController::GetController(Retargeter);
		RetargetController->SetIKRig(ERetargetSourceOrTarget::Target, Rig);
		RetargetController->SetPreviewMesh(ERetargetSourceOrTarget::Target, Mesh);
		// The template's ops still name the UE4 mannequin's IK rig; and two of them are tuned for the UE4 mannequin
		// (feet planted where the source's feet are, goals offset sideways), which spread a smaller body's legs.
		RetargetController->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, Rig);
		for (int32 Op = 0; Op < RetargetController->GetNumRetargetOps(); ++Op)
		{
			const FInstancedStruct* Struct = RetargetController->GetRetargetOpStructAtIndex(Op);
			const FString Type = Struct && Struct->GetScriptStruct() ? Struct->GetScriptStruct()->GetName() : FString();
			if (Type == TEXT("IKRetargetBlendToSourceOp") || Type == TEXT("IKRetargetOffsetGoalsOp"))
			{
				RetargetController->SetRetargetOpEnabled(Op, false);
				UE_LOG(LogMakeHumanArt, Display, TEXT("  op %d %s off"), Op, *Type);
			}
		}
		// The pelvis op names the target's pelvis bone: the retarget root of this skeleton.
		for (int32 Op = 0; Op < RetargetController->GetNumRetargetOps(); ++Op)
		{
			FInstancedStruct* Struct = RetargetController->GetRetargetOpStructAtIndex(Op);
			FStructProperty* SettingsProperty = Struct && Struct->GetScriptStruct() ? FindFProperty<FStructProperty>(Struct->GetScriptStruct(), TEXT("Settings")) : nullptr;
			FStructProperty* PelvisProperty = SettingsProperty ? FindFProperty<FStructProperty>(SettingsProperty->Struct, TEXT("TargetPelvisBone")) : nullptr;
			if (PelvisProperty && PelvisProperty->Struct == FBoneReference::StaticStruct())
			{
				void* Settings = SettingsProperty->ContainerPtrToValuePtr<void>(Struct->GetMutableMemory());
				FBoneReference* Pelvis = PelvisProperty->ContainerPtrToValuePtr<FBoneReference>(Settings);
				Pelvis->BoneName = FName(*RetargetRoot);
				UE_LOG(LogMakeHumanArt, Display, TEXT("  op %d: target pelvis %s"), Op, *Pelvis->BoneName.ToString());
			}
		}
		RetargetController->AutoMapChains(EAutoMapChainType::Exact, true);
		// The template's target pose is the UE4 mannequin's: a fresh pose for this skeleton, its bones turned to line
		// up with the source's chains (the arms and legs of an A pose differ between skeletons).
		// Every target pose the template has (a retarget profile may ask for one by name) is reset and aligned.
		TArray<FName> Poses;
		RetargetController->GetRetargetPoses(ERetargetSourceOrTarget::Target).GetKeys(Poses);
		const FName Current = RetargetController->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target);
		const UIKRigDefinition* SourceRig = LoadObject<UIKRigDefinition>(nullptr, MhSourceRig);
		const USkeletalMesh* SourceMesh = SourceRig ? SourceRig->GetPreviewMesh() : nullptr;
		if (SourceRig && SourceMesh)
		{
			for (const FName& Pose : Poses)
			{
				RetargetController->SetCurrentRetargetPose(Pose, ERetargetSourceOrTarget::Target);
				RetargetController->ResetRetargetPose(Pose, {}, ERetargetSourceOrTarget::Target);
				MhAlignRetargetPose(RetargetController, SourceRig, SourceMesh, Rig, Mesh);
				UE_LOG(LogMakeHumanArt, Display, TEXT("  target pose %s aligned: thigh_l offset %s"), *Pose.ToString(),
					*RetargetController->GetRotationOffsetForRetargetPoseBone(TEXT("thigh_l"), ERetargetSourceOrTarget::Target).Rotator().ToString());
			}
			RetargetController->SetCurrentRetargetPose(Current, ERetargetSourceOrTarget::Target);
		}
		else
		{
			UE_LOG(LogMakeHumanArt, Warning, TEXT("  no source IK rig or its preview mesh; the retarget pose is not aligned"));
		}
		UE_LOG(LogMakeHumanArt, Display, TEXT("  retargeter %s: target pose %s, source pose %s"), *Retargeter->GetName(),
			*RetargetController->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target).ToString(),
			*RetargetController->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Source).ToString());
		for (const FInstancedStruct& Op : Retargeter->GetRetargetOps())
		{
			FString Text;
			Op.GetScriptStruct()->ExportText(Text, Op.GetMemory(), nullptr, nullptr, PPF_None, nullptr);
			UE_LOG(LogMakeHumanArt, Display, TEXT("  op %s: %s"), *Op.GetScriptStruct()->GetName(), *Text.Left(260));
		}
		Failures += MhSave(Retargeter) ? 0 : 1;
		return Failures;
	}
}

UMakeHumanArtCommandlet::UMakeHumanArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMakeHumanArtCommandlet::Main(const FString& Params)
{
	if (Params.Contains(TEXT("-Probe")))
	{
		return MhProbe();
	}
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	int32 Failures = 0;
	// -Clips: only the animation clips (assets/animations.json), retargeted onto the mannequin.
	if (Params.Contains(TEXT("-Clips")))
	{
		// -Only=<name>,<name>: just these clips, added to the ones already there (nothing else is deleted or redone).
		FString OnlyText;
		TArray<FString> Only;
		if (FParse::Value(FCommandLine::Get(), TEXT("Only="), OnlyText, false))
		{
			OnlyText.ParseIntoArray(Only, TEXT(","));
		}
		if (Only.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(MhClipsFolder), false, true);
		}
		// The asset registry still lists what was there (scanned at startup): rescanned, or the batch retarget's
		// overwrite finds assets with no file and crashes.
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanPathsSynchronous({MhClipsFolder}, true);
		const FString RepoRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".."));
		FString Text;
		TSharedPtr<FJsonObject> Json;
		if (!FFileHelper::LoadFileToString(Text, *(RepoRoot / TEXT("assets/animations.json"))) ||
			!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json)
		{
			UE_LOG(LogMakeHumanArt, Error, TEXT("No assets/animations.json"));
			return 1;
		}
		for (const TSharedPtr<FJsonValue>& Value : Json->GetArrayField(TEXT("clips")))
		{
			const TSharedPtr<FJsonObject> Clip = Value->AsObject();
			FString Name, Fbx;
			if (!Clip || !Clip->TryGetStringField(TEXT("name"), Name) || !Clip->TryGetStringField(TEXT("fbx"), Fbx) || (!Only.IsEmpty() && !Only.Contains(Name)))
			{
				continue;
			}
			Fbx = FPaths::IsRelative(Fbx) ? RepoRoot / Fbx : Fbx;
			if (!FPaths::FileExists(Fbx))
			{
				UE_LOG(LogMakeHumanArt, Warning, TEXT("Clip %s: no %s; skipped"), *Name, *Fbx);
				continue;
			}
			Failures += MhMakeClip(Tools, Name, Fbx);
		}
		const TArray<TSharedPtr<FJsonValue>>* Sets = nullptr;
		if (Json->TryGetArrayField(TEXT("sets"), Sets))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Sets)
			{
				const TSharedPtr<FJsonObject> Set = Value->AsObject();
				FString SetName, Manifest;
				if (!Set || !Set->TryGetStringField(TEXT("name"), SetName) || !Set->TryGetStringField(TEXT("manifest"), Manifest))
				{
					continue;
				}
				// The set's clips: a manifest written by its prep script (mixamo_prep.py: skeleton, clips: name, category, fbx).
				Manifest = FPaths::IsRelative(Manifest) ? RepoRoot / Manifest : Manifest;
				FString ManifestText;
				TSharedPtr<FJsonObject> ManifestJson;
				if (!FFileHelper::LoadFileToString(ManifestText, *Manifest) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestText), ManifestJson) || !ManifestJson)
				{
					UE_LOG(LogMakeHumanArt, Warning, TEXT("Clip set %s: no %s; skipped"), *SetName, *Manifest);
					continue;
				}
				FString Skeleton = ManifestJson->GetStringField(TEXT("skeleton"));
				Skeleton = FPaths::IsRelative(Skeleton) ? RepoRoot / Skeleton : Skeleton;
				TArray<TTuple<FString, FString, FString>> Clips;
				for (const TSharedPtr<FJsonValue>& ClipValue : ManifestJson->GetArrayField(TEXT("clips")))
				{
					const TSharedPtr<FJsonObject> Clip = ClipValue->AsObject();
					FString File = Clip->GetStringField(TEXT("fbx"));
					File = FPaths::IsRelative(File) ? RepoRoot / File : File;
					if (Only.IsEmpty() || Only.Contains(Clip->GetStringField(TEXT("name"))))
					{
						Clips.Add(MakeTuple(Clip->GetStringField(TEXT("name")), Clip->GetStringField(TEXT("category")), File));
					}
				}
				if (!Clips.IsEmpty())
				{
					Failures += MhMakeClipSet(Tools, SetName, Skeleton, Clips);
				}
			}
		}
		UE_LOG(LogMakeHumanArt, Display, TEXT("Animation clips: done, %d problems"), Failures);
		return Failures == 0 ? 0 : 1;
	}
	// Start clean: both folders hold only what this run makes.
	for (const TCHAR* Folder : {MakeHumanFolder, MakeHumanLocalFolder})
	{
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Folder), false, true);
	}
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".."));
	for (const FMhCharacter& Character : MakeHumanCharacters(Root))
	{
		const FMakeHumanCharacterFiles& Files = Character.Files;
		const FString Folder = Files.bLocalOnly ? FString(MakeHumanLocalFolder) : FString(MakeHumanFolder);
		if (!FPaths::FileExists(Files.Fbx))
		{
			UE_LOG(LogMakeHumanArt, Warning, TEXT("%s: no %s (export it with tools/makehuman first); skipped"), *Files.Name, *Files.Fbx);
			continue;
		}
		Failures += MhMakeCharacter(Tools, Files, Character.Config, Folder);
	}
	UE_LOG(LogMakeHumanArt, Display, TEXT("MakeHuman art: done, %d problems"), Failures);
	return Failures == 0 ? 0 : 1;
}
