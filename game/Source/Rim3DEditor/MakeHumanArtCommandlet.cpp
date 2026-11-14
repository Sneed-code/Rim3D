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
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/TextureFactory.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "ImageCore.h"
#include "Engine/SubsurfaceProfile.h"
#include "Factories/MaterialFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
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
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#include "MakeHumanArt.h"
#include "PawnLooks.h"

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

	// Plain lit and masked materials for models that bring their own textures (local comparison models): colour,
	// normal and (masked) opacity maps, roughness a value; the skin variant is subsurface skin.
	UMaterial* MhBaseMaterial(IAssetTools& Tools, const FString& Name, bool bMasked, bool bSkin, bool bAlphaFromColor = false)
	{
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
		}
		UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		UTexture* Flat = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));
		auto* Color = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -600, -200));
		Color->ParameterName = TEXT("BaseColor");
		Color->Texture = White;
		auto* Tint = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -600, -400));
		Tint->ParameterName = TEXT("Tint");
		Tint->DefaultValue = FLinearColor::White;
		auto* Tinted = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass(), -300, -250);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Color, TEXT("RGB"), Tinted, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT("RGB"), Tinted, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Tinted, TEXT(""), MP_BaseColor);
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
		const UIKRigDefinition* TargetRig, const USkeletalMesh* TargetMesh)
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

		UMaterialInterface* SkinBase = LoadObject<UMaterialInterface>(nullptr, *PawnLooksAssetPath(PawnLooksSkinMaterial));
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
						SkinInstance->PostEditChange();
						Failures += MhSave(SkinInstance) ? 0 : 1;
						Material.MaterialInterface = SkinInstance;
						UE_LOG(LogMakeHumanArt, Display, TEXT("  slot %d %s: skin %s"), Slot, *Key, *SkinInstance->GetName());
						continue;
					}
				}
				const bool bMasked = !Opacity.IsEmpty() || bAlpha;
				UMaterial* Base = MhBaseMaterial(Tools, !bMasked ? (bSkin ? TEXT("M_MH_Skin") : TEXT("M_MH_Lit")) : (Opacity.IsEmpty() ? TEXT("M_MH_MaskedA") : TEXT("M_MH_Masked")),
					bMasked, bSkin, Opacity.IsEmpty() && bAlpha);
				auto* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
				Factory->InitialParent = Base;
				auto* Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(FString::Printf(TEXT("MI_%s_%d"), *Files.Name, Slot), Folder, UMaterialInstanceConstant::StaticClass(), Factory));
				if (!Instance)
				{
					++Failures;
					continue;
				}
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
