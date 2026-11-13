#include "PawnLooksCommandlet.h"

#include "AnimationRuntime.h"
#include "AssetToolsModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "DNAReader.h"
#include "DNAToSkelMeshMap.h"
#include "DNAUtils.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/SubsurfaceProfile.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "ImageCore.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkelMeshDNAUtils.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "PawnLooks.h"
#include "PawnSkinSeam.h"

DEFINE_LOG_CATEGORY_STATIC(LogPawnLooks, Log, All);

namespace
{
	const TCHAR* const LooksKellanClass = TEXT("/Game/MetaHumans/Kellan/BP_Kellan.BP_Kellan_C");
	const TCHAR* const LooksFaceMesh = TEXT("/Game/MetaHumans/Kellan/Face/Kellan_FaceMesh.Kellan_FaceMesh");
	const TCHAR* const LooksSkinProfile = TEXT("/Game/MetaHumans/Common/Face/SubsurfaceProfiles/SSP_Head.SSP_Head");
	// The body Kellan has; the other body types are measured against it.
	const TCHAR* const LooksReferenceBody = TEXT("m_med_nrw");
	// The engine's whole MetaHuman body (the Creator's template), all of it skin.
	const TCHAR* const LooksFullBodyMesh = TEXT("/MetaHumanCharacter/Body/IdentityTemplate/SKM_Body.SKM_Body");
	// The Creator's bare body skin (its light skin tone colour map, no underwear; patched into Kellan's where his has
	// underwear) and one of its body surface normal maps.
	const TCHAR* const LooksBareBodyColor = TEXT("/MetaHumanCharacter/Optional/BodyTextures/T_Skin_V1_Body_BC.T_Skin_V1_Body_BC");
	const TCHAR* const LooksBareBodyNormal = TEXT("/MetaHumanCharacter/Optional/BodyTextures/SurfaceDetail/T_Chr0005_Body_N.T_Chr0005_Body_N");
	// The whole body takes the face mesh's skin weights within this distance of it, and its points within the second.
	constexpr double LooksSeamWeldCm = 4.0;
	constexpr double LooksSeamSnapCm = 0.6;
	// Clothes follow the average of this many nearest body vertices.
	constexpr int32 LooksClothNeighbours = 4;

	FString LooksBodyDnaPath(const FString& Name)
	{
		return FPaths::EnginePluginsDir() / TEXT("MetaHuman/MetaHumanCharacter/Content/Optional/Body/FixedCompatibility") / Name + TEXT(".dna");
	}

	// ---- Nearest points ------------------------------------------------------------------------

	struct FLooksGrid
	{
		double Cell = 2.0;
		TArray<FVector> Points;
		TMap<FIntVector, TArray<int32>> Cells;

		FIntVector Key(const FVector& P) const
		{
			return FIntVector(FMath::FloorToInt32(P.X / Cell), FMath::FloorToInt32(P.Y / Cell), FMath::FloorToInt32(P.Z / Cell));
		}

		void Build(TArray<FVector> InPoints, double InCell)
		{
			Cell = InCell;
			Points = MoveTemp(InPoints);
			Cells.Reset();
			for (int32 i = 0; i < Points.Num(); ++i)
			{
				Cells.FindOrAdd(Key(Points[i])).Add(i);
			}
		}

		// The K nearest points (nearest first), searched out to MaxDistance.
		void Nearest(const FVector& Q, int32 K, double MaxDistance, TArray<TPair<int32, double>>& Out) const
		{
			Out.Reset();
			const FIntVector C = Key(Q);
			const int32 MaxRing = FMath::CeilToInt32(MaxDistance / Cell) + 1;
			for (int32 Ring = 0; Ring <= MaxRing; ++Ring)
			{
				for (int32 x = -Ring; x <= Ring; ++x)
				{
					for (int32 y = -Ring; y <= Ring; ++y)
					{
						for (int32 z = -Ring; z <= Ring; ++z)
						{
							if (FMath::Max3(FMath::Abs(x), FMath::Abs(y), FMath::Abs(z)) != Ring)
							{
								continue;
							}
							if (const TArray<int32>* Found = Cells.Find(C + FIntVector(x, y, z)))
							{
								for (const int32 i : *Found)
								{
									Out.Add({i, FVector::Dist(Q, Points[i])});
								}
							}
						}
					}
				}
				// Everything closer than Ring * Cell has been seen.
				if (Out.Num() >= K)
				{
					Out.Sort([](const TPair<int32, double>& A, const TPair<int32, double>& B) { return A.Value < B.Value; });
					if (Out[K - 1].Value <= Ring * Cell)
					{
						Out.SetNum(K);
						return;
					}
				}
			}
			Out.Sort([](const TPair<int32, double>& A, const TPair<int32, double>& B) { return A.Value < B.Value; });
			Out.SetNum(FMath::Min(K, Out.Num()));
		}

		int32 Nearest(const FVector& Q, double MaxDistance, double& OutDistance) const
		{
			TArray<TPair<int32, double>> Found;
			Nearest(Q, 1, MaxDistance, Found);
			OutDistance = Found.Num() ? Found[0].Value : TNumericLimits<double>::Max();
			return Found.Num() ? Found[0].Key : INDEX_NONE;
		}
	};

	// ---- Body type files -----------------------------------------------------------------------

	// One body type from its DNA file: the full-detail mesh (body and head together), its skin
	// weights by bone name, and its joints as a skeleton in Unreal's terms. The lower details
	// (the file's meshes 1 to 3) are kept too, for the whole body's lower LODs.
	struct FLooksBody
	{
		FString Name;
		TArray<FVector> Positions;
		TArray<TArray<TPair<int32, float>>> Skin;  // per vertex: bone index in Bones, weight
		TArray<FName> Bones;
		TArray<FTransform> BoneWorld;  // bind pose, component space, by Bones
		TArray<TArray<FVector>> LodPositions;  // by DNA mesh (LOD); [0] is Positions
		TArray<TArray<TArray<TPair<int32, float>>>> LodSkin;
	};

	// How a DNA joint's three angles become a rotation: as a rotator (pitch, yaw, roll) or as the
	// engine's DNA code reads them (Euler roll, pitch, yaw). The generator keeps the one that
	// rebuilds Kellan's own skeleton from his body type's file.
	enum class ELooksAngles : uint8 { Rotator, Euler };

	bool LooksReadBody(const FString& Name, ELooksAngles Angles, FLooksBody& Out)
	{
		Out.Name = Name;
		TSharedPtr<IDNAReader> Reader = ReadDNAFromFile(LooksBodyDnaPath(Name), EDNADataLayer::All, 0);
		if (!Reader || Reader->GetMeshCount() == 0)
		{
			UE_LOG(LogPawnLooks, Error, TEXT("Could not read %s"), *LooksBodyDnaPath(Name));
			return false;
		}
		Out.LodPositions.SetNum(Reader->GetMeshCount());
		Out.LodSkin.SetNum(Reader->GetMeshCount());
		for (uint16 Mesh = 0; Mesh < Reader->GetMeshCount(); ++Mesh)
		{
			const uint32 Count = Reader->GetVertexPositionCount(Mesh);
			TArray<FVector>& Positions = Out.LodPositions[Mesh];
			TArray<TArray<TPair<int32, float>>>& Skin = Out.LodSkin[Mesh];
			Positions.SetNum(Count);
			Skin.SetNum(Count);
			for (uint32 i = 0; i < Count; ++i)
			{
				Positions[i] = Reader->GetVertexPosition(Mesh, i);
			}
			for (uint32 i = 0; i < Reader->GetSkinWeightsCount(Mesh) && i < Count; ++i)
			{
				const TArrayView<const float> Weights = Reader->GetSkinWeightsValues(Mesh, i);
				const TArrayView<const uint16> Joints = Reader->GetSkinWeightsJointIndices(Mesh, i);
				for (int32 k = 0; k < Weights.Num() && k < Joints.Num(); ++k)
				{
					Skin[i].Add({Joints[k], Weights[k]});
				}
			}
		}
		Out.Positions = Out.LodPositions[0];
		Out.Skin = Out.LodSkin[0];

		// The joints in component space, each joint times its parent. The root's own rotation is
		// left out: the vertices are already in Unreal's axes.
		Out.Bones.SetNum(Reader->GetJointCount());
		Out.BoneWorld.SetNum(Reader->GetJointCount());
		const bool bRadians = Reader->GetRotationUnit() != ERotationUnit::Degrees;
		for (uint16 j = 0; j < Reader->GetJointCount(); ++j)
		{
			Out.Bones[j] = FName(*Reader->GetJointName(j));
			FVector Euler = Reader->GetNeutralJointRotation(j);
			if (bRadians)
			{
				Euler = FVector(FMath::RadiansToDegrees(Euler.X), FMath::RadiansToDegrees(Euler.Y), FMath::RadiansToDegrees(Euler.Z));
			}
			const FQuat Rotation = Angles == ELooksAngles::Rotator ? FRotator(Euler.X, Euler.Y, Euler.Z).Quaternion() : FQuat::MakeFromEuler(Euler);
			const uint16 Parent = Reader->GetJointParentIndex(j);
			if (Parent >= j)
			{
				Out.BoneWorld[j] = FTransform(Reader->GetNeutralJointTranslation(j));
				continue;
			}
			Out.BoneWorld[j] = FTransform(Rotation, Reader->GetNeutralJointTranslation(j)) * Out.BoneWorld[Parent];
			Out.BoneWorld[j].NormalizeRotation();
		}
		return true;
	}

	// How far each vertex of `Body` moves to have `Target`'s shape on `Reference`'s skeleton: the
	// target's vertices are taken off its own skeleton and put on the reference one by their skin
	// weights, so a shorter body keeps its shape but not its height (heights come from scaling
	// the whole pawn).
	TArray<FVector> LooksShapeDelta(const FLooksBody& Reference, const FLooksBody& Target, int32 Mesh = 0)
	{
		const TArray<FVector>& RefPositions = Reference.LodPositions[Mesh];
		const TArray<FVector>& TargetPositions = Target.LodPositions[Mesh];
		const TArray<TArray<TPair<int32, float>>>& TargetSkin = Target.LodSkin[Mesh];
		TArray<FVector> Delta;
		Delta.SetNumZeroed(RefPositions.Num());
		for (int32 i = 0; i < RefPositions.Num() && i < TargetPositions.Num(); ++i)
		{
			FVector Moved = FVector::ZeroVector;
			float Total = 0.0f;
			for (const TPair<int32, float>& W : TargetSkin[i])
			{
				if (!Target.BoneWorld.IsValidIndex(W.Key) || !Reference.BoneWorld.IsValidIndex(W.Key))
				{
					continue;
				}
				// Each bone carries the point by its own move only, not its turn: the body types' joints are turned a
				// little differently, and turning a breast or belly about a spine joint 15 cm away bent their shape.
				Moved += (TargetPositions[i] - Target.BoneWorld[W.Key].GetLocation() + Reference.BoneWorld[W.Key].GetLocation()) * W.Value;
				Total += W.Value;
			}
			Delta[i] = Total > 0.0f ? Moved / Total - RefPositions[i] : FVector::ZeroVector;
		}
		return Delta;
	}

	// ---- Assets --------------------------------------------------------------------------------

	bool LooksSave(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *File, Args);
		UE_LOG(LogPawnLooks, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *File);
		return bSaved;
	}

	TArray<FVector> LooksVertexPositions(const FMeshDescription& Description)
	{
		FSkeletalMeshConstAttributes Attributes(Description);
		TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TArray<FVector> Out;
		Out.SetNum(Description.Vertices().GetArraySize());
		for (const FVertexID V : Description.Vertices().GetElementIDs())
		{
			Out[V.GetValue()] = FVector(Positions[V]);
		}
		return Out;
	}

	// Adds the morphs to every LOD of `Mesh`: each vertex takes the average delta of its nearest
	// vertices on the full-detail DNA body (one for the body itself, a few for clothes).
	void LooksAddMorphs(USkeletalMesh* Mesh, const FLooksGrid& BodyGrid, const TMap<FName, TArray<FVector>>& BodyDeltas, int32 Neighbours)
	{
		for (int32 Lod = 0; Lod < Mesh->GetLODNum(); ++Lod)
		{
			if (!Mesh->HasMeshDescription(Lod))
			{
				UE_LOG(LogPawnLooks, Warning, TEXT("%s LOD %d has no mesh description; no morphs there"), *Mesh->GetName(), Lod);
				continue;
			}
			FMeshDescription* Description = Mesh->GetMeshDescription(Lod);
			const TArray<FVector> Positions = LooksVertexPositions(*Description);
			FSkeletalMeshAttributes Attributes(*Description);
			double Farthest = 0.0;
			TArray<TPair<int32, double>> Found;
			TArray<TArray<TPair<int32, float>>> Weights;
			Weights.SetNum(Positions.Num());
			for (const FVertexID V : Description->Vertices().GetElementIDs())
			{
				BodyGrid.Nearest(Positions[V.GetValue()], Neighbours, 40.0, Found);
				float Total = 0.0f;
				for (const TPair<int32, double>& F : Found)
				{
					const float W = 1.0f / static_cast<float>(FMath::Max(F.Value, 0.05));
					Weights[V.GetValue()].Add({F.Key, W});
					Total += W;
				}
				for (TPair<int32, float>& W : Weights[V.GetValue()])
				{
					W.Value /= Total;
				}
				Farthest = FMath::Max(Farthest, Found.Num() ? Found[0].Value : 999.0);
			}
			for (const TPair<FName, TArray<FVector>>& Morph : BodyDeltas)
			{
				Attributes.UnregisterMorphTargetAttribute(Morph.Key);
				Attributes.RegisterMorphTargetAttribute(Morph.Key, false);
				TVertexAttributesRef<FVector3f> Deltas = Attributes.GetVertexMorphPositionDelta(Morph.Key);
				double Largest = 0.0;
				for (const FVertexID V : Description->Vertices().GetElementIDs())
				{
					FVector D = FVector::ZeroVector;
					for (const TPair<int32, float>& W : Weights[V.GetValue()])
					{
						D += Morph.Value[W.Key] * W.Value;
					}
					Deltas[V] = FVector3f(D);
					Largest = FMath::Max(Largest, D.Size());
				}
				UE_LOG(LogPawnLooks, Display, TEXT("  %s LOD %d morph %s: largest move %.2f cm"), *Mesh->GetName(), Lod, *Morph.Key.ToString(), Largest);
			}
			UE_LOG(LogPawnLooks, Display, TEXT("  %s LOD %d: %d vertices, farthest from the body %.2f cm"), *Mesh->GetName(), Lod,
				Description->Vertices().Num(), Farthest);
			Mesh->CommitMeshDescription(Lod);
		}
		Mesh->PostEditChange();
	}

	// The average colour of a texture's lit texels (skin, not the empty parts of the UV layout), linear.
	FLinearColor LooksAverageColor(UTexture* Texture)
	{
		FImage Image;
		if (!Texture || !Texture->Source.IsValid() || !Texture->Source.GetMipImage(Image, 0))
		{
			UE_LOG(LogPawnLooks, Warning, TEXT("No source pixels in %s; average taken as mid grey"), Texture ? *Texture->GetPathName() : TEXT("(none)"));
			return FLinearColor(0.5f, 0.5f, 0.5f);
		}
		Image.GammaSpace = Texture->SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
		FImage Linear;
		Image.CopyTo(Linear, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		const TArrayView64<FLinearColor> Pixels = Linear.AsRGBA32F();
		FLinearColor Sum(0, 0, 0, 0);
		int64 Count = 0;
		for (int64 i = 0; i < Pixels.Num(); i += 7)
		{
			const FLinearColor& P = Pixels[i];
			const float Luminance = P.GetLuminance();
			if (Luminance > 0.02f && Luminance < 0.9f)
			{
				Sum += P;
				++Count;
			}
		}
		const FLinearColor Average = Count ? Sum / static_cast<float>(Count) : FLinearColor(0.5f, 0.5f, 0.5f);
		UE_LOG(LogPawnLooks, Display, TEXT("  average of %s: %s (%lld texels)"), *Texture->GetName(), *Average.ToString(), Count);
		return FLinearColor(Average.R, Average.G, Average.B, 1.0f);
	}

	UMaterialExpression* LooksExpression(UMaterial* Material, UClass* Class, int32 X, int32 Y)
	{
		return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
	}

	// Our skin: the MetaHuman's colour texture taken as detail around its own average colour,
	// times the pawn's skin colour (custom primitive data 0-2, set per pawn), so any skin colour,
	// red included, keeps the texture's pores, veins and shading.
	UMaterial* LooksMakeSkinMaterial(IAssetTools& Tools, UTexture* DefaultColor, UTexture* DefaultNormal)
	{
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(TEXT("M_PawnSkin"), PawnLooksFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		Material->SetShadingModel(MSM_SubsurfaceProfile);
		Material->SubsurfaceProfile = LoadObject<USubsurfaceProfile>(nullptr, LooksSkinProfile);
		bool bRecompile = false;
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_SkeletalMesh, bRecompile);
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_MorphTargets, bRecompile);

		auto* Color = Cast<UMaterialExpressionTextureSampleParameter2D>(LooksExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -900, -200));
		Color->ParameterName = PawnLooksParamBaseColor;
		Color->Texture = DefaultColor;
		Color->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(DefaultColor);
		auto* Skin = Cast<UMaterialExpressionVectorParameter>(LooksExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -900, 50));
		Skin->ParameterName = PawnLooksParamSkinColor;
		Skin->DefaultValue = FLinearColor(0.5f, 0.35f, 0.28f);
		Skin->bUseCustomPrimitiveData = true;
		Skin->PrimitiveDataIndex = PawnLooksSkinDataIndex;
		auto* Average = Cast<UMaterialExpressionVectorParameter>(LooksExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -900, 250));
		Average->ParameterName = PawnLooksParamSourceAverage;
		Average->DefaultValue = FLinearColor(0.5f, 0.35f, 0.28f);
		// The texture's own colour against its average, mostly as lightness: taken channel by channel, a dark brown
		// feature (nipples, lips) on a red-brown texture turned blue-grey on pale skin. Darker features lean warm.
		auto* Tint = Cast<UMaterialExpressionCustom>(LooksExpression(Material, UMaterialExpressionCustom::StaticClass(), -450, 0));
		Tint->OutputType = CMOT_Float3;
		Tint->Description = TEXT("PawnSkinTint");
		Tint->Code = TEXT("float3 r = Tex / max(Average, 0.0001);\n"
			"float l = max(dot(r, float3(0.3, 0.59, 0.11)), 0.0001);\n"
			"float3 w = float3(pow(l, 0.6), l, pow(l, 1.25));\n"
			"return lerp(r, w, 0.7) * Skin;");
		Tint->Inputs.SetNum(3);
		Tint->Inputs[0].InputName = TEXT("Tex");
		Tint->Inputs[1].InputName = TEXT("Average");
		Tint->Inputs[2].InputName = TEXT("Skin");
		Tint->Inputs[0].Input.Connect(0, Color);  // output 0 is RGB
		Tint->Inputs[1].Input.Connect(0, Average);
		Tint->Inputs[2].Input.Connect(0, Skin);
		UMaterialEditingLibrary::ConnectMaterialProperty(Tint, TEXT(""), MP_BaseColor);

		auto* Normal = Cast<UMaterialExpressionTextureSampleParameter2D>(LooksExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -900, 450));
		Normal->ParameterName = PawnLooksParamNormal;
		Normal->Texture = DefaultNormal;
		Normal->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(DefaultNormal);
		UMaterialEditingLibrary::ConnectMaterialProperty(Normal, TEXT("RGB"), MP_Normal);

		auto* Roughness = Cast<UMaterialExpressionScalarParameter>(LooksExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -350, 300));
		Roughness->ParameterName = TEXT("Roughness");
		Roughness->DefaultValue = 0.55f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);
		auto* Specular = Cast<UMaterialExpressionScalarParameter>(LooksExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -350, 400));
		Specular->ParameterName = TEXT("Specular");
		Specular->DefaultValue = 0.35f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Specular, TEXT(""), MP_Specular);

		Material->PostEditChange();
		return Material;
	}

	// Horns and other lineage parts: one colour from custom primitive data 0-2.
	UMaterial* LooksMakePartMaterial(IAssetTools& Tools)
	{
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(TEXT("M_PawnPart"), PawnLooksFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		auto* Color = Cast<UMaterialExpressionVectorParameter>(LooksExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -500, 0));
		Color->ParameterName = PawnLooksParamPartColor;
		Color->DefaultValue = FLinearColor(0.02f, 0.015f, 0.012f);
		Color->bUseCustomPrimitiveData = true;
		Color->PrimitiveDataIndex = PawnLooksSkinDataIndex;
		UMaterialEditingLibrary::ConnectMaterialProperty(Color, TEXT("RGB"), MP_BaseColor);
		auto* Roughness = Cast<UMaterialExpressionScalarParameter>(LooksExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -500, 200));
		Roughness->ParameterName = TEXT("Roughness");
		Roughness->DefaultValue = 0.4f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);
		Material->PostEditChange();
		return Material;
	}

	// A copy of one of Kellan's skin instances on our skin material: same textures, plus their average.
	UMaterialInstanceConstant* LooksMakeSkinInstance(IAssetTools& Tools, UMaterial* Skin, UMaterialInterface* Original)
	{
		UTexture* Color = nullptr;
		UTexture* Normal = nullptr;
		Original->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColor")), Color);
		Original->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Normal")), Normal);
		auto* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Skin;
		auto* Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(PawnLooksSkinInstanceName(Original->GetName()), PawnLooksFolder,
			UMaterialInstanceConstant::StaticClass(), Factory));
		if (!Instance)
		{
			return nullptr;
		}
		if (Color)
		{
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamBaseColor), Color);
			Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamSourceAverage), LooksAverageColor(Color));
		}
		if (Normal)
		{
			Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamNormal), Normal);
		}
		Instance->PostEditChange();
		return Instance;
	}

	// A horn one metre long along +Z with a base radius of 0.1 m, bending back (-Y) towards the
	// tip; the pawn scales it to the lineage's length and radius.
	UStaticMesh* LooksMakeHorn(UMaterial* PartMaterial)
	{
		const FString PackageName = FString(PawnLooksFolder) / TEXT("SM_PawnHorn");
		UPackage* Package = CreatePackage(*PackageName);
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, TEXT("SM_PawnHorn"), RF_Public | RF_Standalone);
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		constexpr int32 Rings = 9;
		constexpr int32 Sides = 10;
		TArray<FVertexID> Vertices;
		for (int32 r = 0; r < Rings; ++r)
		{
			const float T = static_cast<float>(r) / Rings;
			const float Radius = 0.1f * (1.0f - T * 0.92f);
			const FVector3f Centre(0.0f, -0.35f * T * T, T);
			for (int32 s = 0; s < Sides; ++s)
			{
				const float A = 2.0f * PI * s / Sides;
				const FVertexID V = Description.CreateVertex();
				Attributes.GetVertexPositions()[V] = Centre + FVector3f(FMath::Cos(A) * Radius, FMath::Sin(A) * Radius, 0.0f);
				Vertices.Add(V);
			}
		}
		const FVertexID Tip = Description.CreateVertex();
		Attributes.GetVertexPositions()[Tip] = FVector3f(0.0f, -0.35f, 1.0f);
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Horn");
		const auto Corner = [&](FVertexID V)
		{
			const FVertexInstanceID I = Description.CreateVertexInstance(V);
			Attributes.GetVertexInstanceUVs().Set(I, 0, FVector2f(0.0f, 0.0f));
			return I;
		};
		for (int32 r = 0; r + 1 < Rings; ++r)
		{
			for (int32 s = 0; s < Sides; ++s)
			{
				const int32 n = (s + 1) % Sides;
				Description.CreateTriangle(Group, {Corner(Vertices[r * Sides + s]), Corner(Vertices[(r + 1) * Sides + s]), Corner(Vertices[(r + 1) * Sides + n])});
				Description.CreateTriangle(Group, {Corner(Vertices[r * Sides + s]), Corner(Vertices[(r + 1) * Sides + n]), Corner(Vertices[r * Sides + n])});
			}
		}
		for (int32 s = 0; s < Sides; ++s)
		{
			const int32 n = (s + 1) % Sides;
			Description.CreateTriangle(Group, {Corner(Vertices[(Rings - 1) * Sides + s]), Corner(Tip), Corner(Vertices[(Rings - 1) * Sides + n])});
		}
		Mesh->GetStaticMaterials().Add(FStaticMaterial(PartMaterial, TEXT("Horn"), TEXT("Horn")));
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bBuildSimpleCollision = false;
		Params.bFastBuild = false;
		Mesh->BuildFromMeshDescriptions({&Description}, Params);
		return Mesh;
	}

	// Kellan's skinned components, by variable name, from the Blueprint.
	TMap<FName, USkinnedMeshComponent*> LooksKellanComponents()
	{
		TMap<FName, USkinnedMeshComponent*> Out;
		UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(LoadClass<AActor>(nullptr, LooksKellanClass));
		if (!Generated || !Generated->SimpleConstructionScript)
		{
			return Out;
		}
		for (USCS_Node* Node : Generated->SimpleConstructionScript->GetAllNodes())
		{
			if (USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Node->ComponentTemplate))
			{
				Out.Add(Node->GetVariableName(), Skinned);
			}
		}
		return Out;
	}

	// ---- The whole body ------------------------------------------------------------------------

	// For each vertex of one LOD of the Creator's body template (its own DNA, body only), the
	// vertex of a body type file's combined mesh (body and head) that is the same point of the
	// body: the one with the same texture coordinates, the nearest if several share them (seams,
	// and the head, whose layout may overlap the body's). The two are in different shapes, so
	// position alone can't pair them.
	TArray<int32> LooksMapByUV(IDNAReader& Own, uint16 OwnMesh, IDNAReader& Combined, uint16 CombinedMesh, const TArray<FVector>& CombinedPositions, int32& OutUnmatched)
	{
		const auto Key = [](const FTextureCoordinate& T) { return FIntPoint(FMath::RoundToInt32(T.U * 100000.0f), FMath::RoundToInt32(T.V * 100000.0f)); };
		TMap<FIntPoint, TArray<int32>> ByUV;
		for (uint32 l = 0; l < Combined.GetVertexLayoutCount(CombinedMesh); ++l)
		{
			const FVertexLayout L = Combined.GetVertexLayout(CombinedMesh, l);
			ByUV.FindOrAdd(Key(Combined.GetVertexTextureCoordinate(CombinedMesh, L.TextureCoordinate))).AddUnique(L.Position);
		}
		const uint32 Count = Own.GetVertexPositionCount(OwnMesh);
		TArray<TArray<int32>> Candidates;
		Candidates.SetNum(Count);
		TArray<TArray<FIntPoint>> OwnUV;
		OwnUV.SetNum(Count);
		for (uint32 l = 0; l < Own.GetVertexLayoutCount(OwnMesh); ++l)
		{
			const FVertexLayout L = Own.GetVertexLayout(OwnMesh, l);
			if (OwnUV.IsValidIndex(L.Position))
			{
				OwnUV[L.Position].AddUnique(Key(Own.GetVertexTextureCoordinate(OwnMesh, L.TextureCoordinate)));
			}
			if (const TArray<int32>* Found = ByUV.Find(Key(Own.GetVertexTextureCoordinate(OwnMesh, L.TextureCoordinate))); Found && Candidates.IsValidIndex(L.Position))
			{
				for (const int32 C : *Found)
				{
					Candidates[L.Position].AddUnique(C);
				}
			}
		}
		FLooksGrid Grid;
		Grid.Build(CombinedPositions, 2.0);
		TArray<int32> Map;
		Map.SetNum(Count);
		OutUnmatched = 0;
		for (uint32 i = 0; i < Count; ++i)
		{
			const FVector P = FVector(Own.GetVertexPosition(OwnMesh, i));
			double Best = TNumericLimits<double>::Max();
			Map[i] = INDEX_NONE;
			for (const int32 C : Candidates[i])
			{
				const double D = FVector::DistSquared(P, CombinedPositions[C]);
				if (D < Best)
				{
					Best = D;
					Map[i] = C;
				}
			}
			if (Map[i] == INDEX_NONE)
			{
				// Lower LODs don't always keep a vertex's exact texture coordinates: the nearest ones,
				// then the nearest point among those that share them.
				++OutUnmatched;
				double BestUV = TNumericLimits<double>::Max();
				for (const TPair<FIntPoint, TArray<int32>>& Entry : ByUV)
				{
					for (const FIntPoint& Mine : OwnUV[i])
					{
						const double D = FVector2D(Entry.Key - Mine).SizeSquared();
						if (D < BestUV)
						{
							BestUV = D;
							Best = TNumericLimits<double>::Max();
						}
						if (D <= BestUV)
						{
							for (const int32 C : Entry.Value)
							{
								const double DP = FVector::DistSquared(P, CombinedPositions[C]);
								if (DP < Best)
								{
									Best = DP;
									Map[i] = C;
								}
							}
						}
					}
				}
				if (Map[i] == INDEX_NONE)
				{
					double Distance = 0.0;
					Map[i] = Grid.Nearest(P, 30.0, Distance);
					Map[i] = Map[i] == INDEX_NONE ? 0 : Map[i];
				}
			}
		}
		return Map;
	}

	// `Copy` (of Kellan's body, his skeleton and skin material) becomes the engine's whole MetaHuman
	// body, every LOD: the Creator template's topology and texture layout, in Kellan's body type's
	// shape and skin weights, with each body type's morph made exactly from its own file.
	bool LooksMakeFullBody(USkeletalMesh* Copy, const FLooksBody& Reference, const TMap<FName, TArray<TArray<FVector>>>& LodDeltas)
	{
		USkeletalMesh* Template = LoadObject<USkeletalMesh>(nullptr, LooksFullBodyMesh);
		TSharedPtr<IDNAReader> Own = Template ? USkelMeshDNAUtils::GetDNAReader(Template) : nullptr;
		TSharedPtr<IDNAReader> Combined = ReadDNAFromFile(LooksBodyDnaPath(LooksReferenceBody), EDNADataLayer::All, 0);
		if (!Template || !Own || !Combined || Copy->GetMaterials().IsEmpty())
		{
			UE_LOG(LogPawnLooks, Error, TEXT("The MetaHuman Creator's body template (%s) or its DNA is missing"), LooksFullBodyMesh);
			return false;
		}
		const TArray<FVector> KellanHands = LooksVertexPositions(*Copy->GetMeshDescription(0));
		const FReferenceSkeleton& Skeleton = Copy->GetRefSkeleton();
		TArray<int32> BoneOf;  // DNA joint -> Kellan's bone
		for (const FName& Bone : Reference.Bones)
		{
			BoneOf.Add(Skeleton.FindBoneIndex(Bone));
		}
		const FName Slot = Copy->GetMaterials()[0].ImportedMaterialSlotName;

		// Where the body meets Kellan's face mesh (its neck and the top of the chest), the body takes the face's own
		// points and skin weights, fading to its own over LooksSeamWeldCm, so the two move together and no crack opens.
		USkeletalMesh* Face = LoadObject<USkeletalMesh>(nullptr, LooksFaceMesh);
		TArray<FVector> FacePositions;
		TArray<TArray<TPair<int32, float>>> FaceWeights;  // by Kellan's body bone
		if (Face && Face->HasMeshDescription(0))
		{
			const FMeshDescription& FaceDescription = *Face->GetMeshDescription(0);
			FacePositions = LooksVertexPositions(FaceDescription);
			FaceWeights.SetNum(FacePositions.Num());
			FSkeletalMeshConstAttributes FaceAttributes(FaceDescription);
			FSkinWeightsVertexAttributesConstRef FaceSkin = FaceAttributes.GetVertexSkinWeights();
			const FReferenceSkeleton& FaceSkeleton = Face->GetRefSkeleton();
			for (const FVertexID V : FaceDescription.Vertices().GetElementIDs())
			{
				for (const UE::AnimationCore::FBoneWeight W : FaceSkin.Get(V))
				{
					const int32 Bone = W.GetBoneIndex() < FaceSkeleton.GetNum() ? Skeleton.FindBoneIndex(FaceSkeleton.GetBoneName(W.GetBoneIndex())) : INDEX_NONE;
					if (Bone != INDEX_NONE && W.GetWeight() > 0.0f)
					{
						FaceWeights[V.GetValue()].Add({Bone, W.GetWeight()});
					}
				}
			}
		}
		FLooksGrid FaceGrid;
		FaceGrid.Build(FacePositions, 2.0);
		const int32 Lods = FMath::Min(FMath::Min(Template->GetLODNum(), Copy->GetLODNum()), FMath::Min(static_cast<int32>(Own->GetMeshCount()), Reference.LodPositions.Num()));
		for (int32 Lod = 0; Lod < Lods; ++Lod)
		{
			if (!Template->HasMeshDescription(Lod))
			{
				UE_LOG(LogPawnLooks, Error, TEXT("The body template's LOD %d has no mesh description"), Lod);
				return false;
			}
			FMeshDescription Description = *Template->GetMeshDescription(Lod);
			if (Description.Vertices().Num() != static_cast<int32>(Own->GetVertexPositionCount(Lod)))
			{
				UE_LOG(LogPawnLooks, Error, TEXT("The body template's LOD %d has %d vertices, its DNA %d"), Lod, Description.Vertices().Num(), Own->GetVertexPositionCount(Lod));
				return false;
			}
			int32 Unmatched = 0;
			const TArray<int32> Map = LooksMapByUV(*Own, Lod, *Combined, Lod, Reference.LodPositions[Lod], Unmatched);
			TSet<int32> Distinct(Map);
			UE_LOG(LogPawnLooks, Display, TEXT("Whole body LOD %d: %d vertices, %d distinct in %s's file, %d paired by the nearest texture coordinates"), Lod, Map.Num(), Distinct.Num(),
				LooksReferenceBody, Unmatched);

			FSkeletalMeshAttributes Attributes(Description);
			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			FSkinWeightsVertexAttributesRef Weights = Attributes.GetVertexSkinWeights();
			int32 LostWeights = 0;
			int32 Welded = 0;
			int32 Snapped = 0;
			for (const FVertexID V : Description.Vertices().GetElementIDs())
			{
				const int32 Source = Map[V.GetValue()];
				Positions[V] = FVector3f(Reference.LodPositions[Lod][Source]);
				double FaceDistance = TNumericLimits<double>::Max();
				const int32 FaceVertex = FacePositions.IsEmpty() ? INDEX_NONE : FaceGrid.Nearest(Reference.LodPositions[Lod][Source], LooksSeamWeldCm + 1.0, FaceDistance);
				TArray<UE::AnimationCore::FBoneWeight> Bones;
				float Total = 0.0f;
				for (const TPair<int32, float>& W : Reference.LodSkin[Lod][Source])
				{
					if (BoneOf.IsValidIndex(W.Key) && BoneOf[W.Key] != INDEX_NONE && W.Value > 0.0f)
					{
						Total += W.Value;
					}
				}
				for (const TPair<int32, float>& W : Reference.LodSkin[Lod][Source])
				{
					if (BoneOf.IsValidIndex(W.Key) && BoneOf[W.Key] != INDEX_NONE && W.Value > 0.0f)
					{
						Bones.Add(UE::AnimationCore::FBoneWeight(static_cast<FBoneIndexType>(BoneOf[W.Key]), W.Value / Total));
					}
				}
				if (Bones.IsEmpty())
				{
					Bones.Add(UE::AnimationCore::FBoneWeight(static_cast<FBoneIndexType>(0), 1.0f));
					++LostWeights;
				}
				if (FaceVertex != INDEX_NONE && FaceDistance < LooksSeamWeldCm && !FaceWeights[FaceVertex].IsEmpty())
				{
					const float BodyShare = FMath::SmoothStep(0.0f, 1.0f, static_cast<float>(FaceDistance / LooksSeamWeldCm));
					TMap<int32, float> Blend;
					for (const UE::AnimationCore::FBoneWeight& W : Bones)
					{
						Blend.FindOrAdd(W.GetBoneIndex()) += W.GetWeight() * BodyShare;
					}
					float FaceTotal = 0.0f;
					for (const TPair<int32, float>& W : FaceWeights[FaceVertex])
					{
						FaceTotal += W.Value;
					}
					for (const TPair<int32, float>& W : FaceWeights[FaceVertex])
					{
						Blend.FindOrAdd(W.Key) += W.Value / FaceTotal * (1.0f - BodyShare);
					}
					Blend.ValueSort([](float A, float B) { return A > B; });
					Bones.Reset();
					float Kept = 0.0f;
					for (const TPair<int32, float>& W : Blend)
					{
						if (Bones.Num() < 8 && W.Value > 0.001f)
						{
							Bones.Add(UE::AnimationCore::FBoneWeight(static_cast<FBoneIndexType>(W.Key), W.Value));
							Kept += W.Value;
						}
					}
					for (UE::AnimationCore::FBoneWeight& W : Bones)
					{
						W.SetWeight(W.GetWeight() / Kept);
					}
					++Welded;
					if (FaceDistance < LooksSeamSnapCm)
					{
						Positions[V] = FVector3f(FacePositions[FaceVertex]);
						++Snapped;
					}
				}
				Weights.Set(V, Bones);
			}
			UE_LOG(LogPawnLooks, Display, TEXT("  %d vertices near the face take its skin weights, %d of them its points"), Welded, Snapped);
			if (LostWeights)
			{
				UE_LOG(LogPawnLooks, Warning, TEXT("  %d vertices had no skin weights on Kellan's bones (put on the root)"), LostWeights);
			}

			// The template's own bones, if it lists them, become Kellan's (same 342 bones, another order).
			if (Attributes.HasBones())
			{
				if (Attributes.GetNumBones() != Skeleton.GetRawBoneNum())
				{
					UE_LOG(LogPawnLooks, Error, TEXT("The body template lists %d bones, Kellan has %d"), Attributes.GetNumBones(), Skeleton.GetRawBoneNum());
					return false;
				}
				FSkeletalMeshAttributes::FBoneNameAttributesRef Names = Attributes.GetBoneNames();
				FSkeletalMeshAttributes::FBoneParentIndexAttributesRef Parents = Attributes.GetBoneParentIndices();
				FSkeletalMeshAttributes::FBonePoseAttributesRef Poses = Attributes.GetBonePoses();
				for (int32 b = 0; b < Skeleton.GetRawBoneNum(); ++b)
				{
					Names.Set(FBoneID(b), Skeleton.GetRawRefBoneInfo()[b].Name);
					Parents.Set(FBoneID(b), Skeleton.GetRawRefBoneInfo()[b].ParentIndex);
					Poses.Set(FBoneID(b), Skeleton.GetRawRefBonePose()[b]);
				}
			}
			for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs())
			{
				Attributes.GetPolygonGroupMaterialSlotNames()[Group] = Slot;
			}

			for (const TPair<FName, TArray<TArray<FVector>>>& Morph : LodDeltas)
			{
				if (!Morph.Value.IsValidIndex(Lod))
				{
					continue;
				}
				Attributes.RegisterMorphTargetAttribute(Morph.Key, false);
				TVertexAttributesRef<FVector3f> Deltas = Attributes.GetVertexMorphPositionDelta(Morph.Key);
				double Largest = 0.0;
				for (const FVertexID V : Description.Vertices().GetElementIDs())
				{
					const FVector D = Morph.Value[Lod][Map[V.GetValue()]];
					Deltas[V] = FVector3f(D);
					Largest = FMath::Max(Largest, D.Size());
				}
				UE_LOG(LogPawnLooks, Display, TEXT("  morph %s: largest move %.2f cm"), *Morph.Key.ToString(), Largest);
			}

			// The template's normals are for its own shape.
			FStaticMeshOperations::ComputeTriangleTangentsAndNormals(Description);
			FStaticMeshOperations::ComputeTangentsAndNormals(Description, EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents | EComputeNTBsFlags::WeightedNTBs);

			Copy->CreateMeshDescription(Lod, MoveTemp(Description));
			Copy->CommitMeshDescription(Lod);
		}
		Copy->PostEditChange();

		// Kellan's hands and feet are part of this body: they should be exactly where they were.
		FLooksGrid Grid;
		Grid.Build(LooksVertexPositions(*Copy->GetMeshDescription(0)), 2.0);
		double Worst = 0.0;
		for (const FVector& P : KellanHands)
		{
			double Distance = 0.0;
			Grid.Nearest(P, 20.0, Distance);
			Worst = FMath::Max(Worst, Distance);
		}
		UE_LOG(LogPawnLooks, Display, TEXT("Whole body made (%d LODs); Kellan's hands and feet (%d vertices) are on it within %.4f cm"), Lods, KellanHands.Num(), Worst);
		return Worst < 0.5;
	}

	// Kellan's body colour map with the underwear painted on it taken out: where it is grey (the briefs, the waistband)
	// the Creator's bare skin map shows instead, at Kellan's skin tone, blended over a few texels. The rest (nipples,
	// navel, hands, feet, the skin's detail) stays Kellan's.
	UTexture2D* LooksMakeBareBodyColor(UTexture2D* Kellan)
	{
		UTexture2D* Bare = LoadObject<UTexture2D>(nullptr, LooksBareBodyColor);
		FImage KellanImage, BareImage;
		if (!Kellan || !Bare || !Kellan->Source.GetMipImage(KellanImage, 0) || !Bare->Source.GetMipImage(BareImage, 0))
		{
			return nullptr;
		}
		KellanImage.GammaSpace = Kellan->SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
		BareImage.GammaSpace = EGammaSpace::sRGB;  // stored as display values
		FImage K, B;
		KellanImage.CopyTo(K, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		BareImage.ResizeTo(B, K.SizeX, K.SizeY, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		const int32 W = K.SizeX, H = K.SizeY;
		TArrayView64<FLinearColor> KP = K.AsRGBA32F();
		const TArrayView64<FLinearColor> BP = B.AsRGBA32F();
		// Grey (unsaturated, lit) texels are underwear.
		TArray<float> Mask;
		Mask.SetNumZeroed(W * H);
		FLinearColor SkinSum(0, 0, 0, 0), BareSum(0, 0, 0, 0);
		int64 SkinCount = 0;
		for (int64 i = 0; i < W * H; ++i)
		{
			const FLinearColor& P = KP[i];
			const float Max = FMath::Max3(P.R, P.G, P.B), Min = FMath::Min3(P.R, P.G, P.B);
			const float Saturation = Max > 0.01f ? (Max - Min) / Max : 0.0f;
			Mask[i] = Max > 0.03f ? 1.0f - FMath::SmoothStep(0.12f, 0.30f, Saturation) : 0.0f;
			if (Mask[i] < 0.01f && Max > 0.03f && Max < 0.9f)
			{
				SkinSum += P;
				BareSum += BP[i];
				++SkinCount;
			}
		}
		// Grow it by a few texels and soften the edge.
		const auto Pass = [&](const TArray<float>& In, int32 Radius, bool bMax)
		{
			TArray<float> Out;
			Out.SetNumZeroed(W * H);
			for (int32 y = 0; y < H; ++y)
			{
				for (int32 x = 0; x < W; ++x)
				{
					float V = 0.0f;
					int32 N = 0;
					for (int32 dy = -Radius; dy <= Radius; ++dy)
					{
						for (int32 dx = -Radius; dx <= Radius; ++dx)
						{
							const int32 xx = FMath::Clamp(x + dx, 0, W - 1), yy = FMath::Clamp(y + dy, 0, H - 1);
							const float S = In[yy * W + xx];
							V = bMax ? FMath::Max(V, S) : V + S;
							++N;
						}
					}
					Out[y * W + x] = bMax ? V : V / N;
				}
			}
			return Out;
		};
		Mask = Pass(Pass(Mask, 3, true), 3, false);
		const FLinearColor Ratio = SkinCount ? FLinearColor(SkinSum.R / FMath::Max(BareSum.R, 1e-4f), SkinSum.G / FMath::Max(BareSum.G, 1e-4f), SkinSum.B / FMath::Max(BareSum.B, 1e-4f)) : FLinearColor::White;
		int64 Replaced = 0;
		for (int64 i = 0; i < W * H; ++i)
		{
			const FLinearColor Skin(BP[i].R * Ratio.R, BP[i].G * Ratio.G, BP[i].B * Ratio.B, 1.0f);
			KP[i] = FMath::Lerp(KP[i], Skin, Mask[i]);
			KP[i].A = 1.0f;
			Replaced += Mask[i] > 0.5f ? 1 : 0;
		}
		UE_LOG(LogPawnLooks, Display, TEXT("Bare body colour map: %lld of %d texels were underwear and are skin now"), Replaced, W * H);
		FImage Out;
		K.CopyTo(Out, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const FString Name = TEXT("T_PawnBody_BaseColor");
		UPackage* Package = CreatePackage(*(FString(PawnLooksFolder) / Name));
		UTexture2D* Texture = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone);
		Texture->PreEditChange(nullptr);
		Texture->Source.Init(Out);
		Texture->SetModernSettingsForNewOrChangedTexture();
		Texture->CompressionSettings = TC_Default;
		Texture->LODGroup = TEXTUREGROUP_Character;
		Texture->SRGB = true;
		Texture->MipGenSettings = TMGS_FromTextureGroup;
		Texture->PostEditChange();
		return Texture;
	}

	// A copy of a plugin texture in our folder (the packaged game has no editor plugins), as colour or data.
	UTexture2D* LooksCopyTexture(IAssetTools& Tools, const TCHAR* From, const TCHAR* Name, bool bColor)
	{
		UTexture2D* Source = LoadObject<UTexture2D>(nullptr, From);
		UTexture2D* Copy = Source ? Cast<UTexture2D>(Tools.DuplicateAsset(Name, PawnLooksFolder, Source)) : nullptr;
		if (!Copy)
		{
			UE_LOG(LogPawnLooks, Error, TEXT("Could not copy %s"), From);
			return nullptr;
		}
		Copy->VirtualTextureStreaming = false;
		if (bColor)
		{
			// Stored as display values (the Creator tints it with its own bias and gain).
			Copy->SRGB = true;
			Copy->CompressionSettings = TC_Default;
		}
		Copy->LODGroup = bColor ? TEXTUREGROUP_Character : TEXTUREGROUP_CharacterNormalMap;
		Copy->PostEditChange();
		return Copy;
	}

	// A skin texture's average where `Mesh` (its UVs) meets `Others` (the face's neck on the body, or the body's on the
	// face), so the face and the whole body have the same colour at the seam (PawnSkinSeam.h); the whole texture's
	// average without them.
	FLinearColor LooksSeamAverage(UTexture* Texture, USkeletalMesh* Mesh, const TArray<FVector>& Others)
	{
		const FLinearColor Whole = LooksAverageColor(Texture);
		FImage Image;
		if (!Texture || !Mesh || !Mesh->HasMeshDescription(0) || !Texture->Source.IsValid() || !Texture->Source.GetMipImage(Image, 0))
		{
			return Whole;
		}
		Image.GammaSpace = Texture->SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
		const TArray<FVector2f> UVs = PawnSeamUVs(*Mesh->GetMeshDescription(0), Others, 3.0);
		const FLinearColor Seam = PawnAverageAt(Image, UVs, Whole);
		UE_LOG(LogPawnLooks, Display, TEXT("  %s at the neck seam (%d texture points): %s, whole texture %s"), *Texture->GetName(), UVs.Num(), *Seam.ToString(), *Whole.ToString());
		return Seam;
	}

	// The body type files' combined mesh points that are the body (the whole body's LOD 0 pairs with them), not the head.
	TArray<int32> LooksBodyIndices(const FLooksBody& Reference)
	{
		USkeletalMesh* Template = LoadObject<USkeletalMesh>(nullptr, LooksFullBodyMesh);
		TSharedPtr<IDNAReader> Own = Template ? USkelMeshDNAUtils::GetDNAReader(Template) : nullptr;
		TSharedPtr<IDNAReader> Combined = ReadDNAFromFile(LooksBodyDnaPath(LooksReferenceBody), EDNADataLayer::All, 0);
		if (!Own || !Combined)
		{
			return {};
		}
		int32 Unmatched = 0;
		TArray<int32> Map = LooksMapByUV(*Own, 0, *Combined, 0, Reference.LodPositions[0], Unmatched);
		return TSet<int32>(Map).Array();
	}

	int32 LooksGenerate(bool bProbe)
	{
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		const TMap<FName, USkinnedMeshComponent*> Kellan = LooksKellanComponents();
		USkinnedMeshComponent* const* BodyComponent = Kellan.Find(TEXT("Body"));
		USkeletalMesh* KellanBody = BodyComponent ? Cast<USkeletalMesh>((*BodyComponent)->GetSkinnedAsset()) : nullptr;
		USkeletalMesh* Face = LoadObject<USkeletalMesh>(nullptr, LooksFaceMesh);
		if (!KellanBody || !Face || !KellanBody->HasMeshDescription(0) || !Face->HasMeshDescription(0))
		{
			UE_LOG(LogPawnLooks, Error, TEXT("Kellan (the Game Animation Sample's MetaHuman) is missing; see assets/manifest.json"));
			return 1;
		}

		// The reference body and each body type, and how far each vertex moves.
		// Check which way of reading the angles gives Kellan's own skeleton from his body type.
		FLooksBody Reference;
		ELooksAngles Angles = ELooksAngles::Rotator;
		double BestWorst = TNumericLimits<double>::Max();
		for (const ELooksAngles Try : {ELooksAngles::Rotator, ELooksAngles::Euler})
		{
			FLooksBody Body;
			if (!LooksReadBody(LooksReferenceBody, Try, Body))
			{
				return 1;
			}
			double Worst = 0.0;
			const FReferenceSkeleton& Ref = KellanBody->GetRefSkeleton();
			for (int32 j = 0; j < Body.Bones.Num(); ++j)
			{
				const int32 Bone = Ref.FindBoneIndex(Body.Bones[j]);
				if (Bone != INDEX_NONE)
				{
					const FTransform KellanBone = FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Bone);
					Worst = FMath::Max(Worst, FVector::Dist(Body.BoneWorld[j].GetLocation(), KellanBone.GetLocation()));
					Worst = FMath::Max(Worst, 10.0 * Body.BoneWorld[j].GetRotation().AngularDistance(KellanBone.GetRotation()));
				}
			}
			UE_LOG(LogPawnLooks, Display, TEXT("Skeleton from %s read as %s against Kellan's: worst joint %.4f (cm, or radians x 10)"), LooksReferenceBody,
				Try == ELooksAngles::Rotator ? TEXT("rotator") : TEXT("Euler"), Worst);
			if (Worst < BestWorst)
			{
				BestWorst = Worst;
				Angles = Try;
				Reference = MoveTemp(Body);
			}
		}
		if (BestWorst > 0.1)
		{
			UE_LOG(LogPawnLooks, Error, TEXT("The body type files' skeleton doesn't match Kellan's"));
			return 1;
		}

		// Kellan's body mesh is only what his clothes leave showing (hands, a little of the feet);
		// the clothes take their shape from the whole body in the DNA file. Check Kellan's
		// vertices are that body's.
		const TArray<FVector> BodyPositions = LooksVertexPositions(*KellanBody->GetMeshDescription(0));
		FLooksGrid DnaGrid;
		DnaGrid.Build(Reference.Positions, 2.0);
		double WorstMatch = 0.0;
		for (const FVector& P : BodyPositions)
		{
			double Distance = 0.0;
			DnaGrid.Nearest(P, 10.0, Distance);
			WorstMatch = FMath::Max(WorstMatch, Distance);
		}
		UE_LOG(LogPawnLooks, Display, TEXT("Kellan's body (%d vertices) against %s (%d): worst match %.4f cm"), BodyPositions.Num(), LooksReferenceBody,
			Reference.Positions.Num(), WorstMatch);

		TMap<FName, TArray<FVector>> BodyDeltas;  // per vertex of the reference DNA body
		TMap<FName, TArray<TArray<FVector>>> LodDeltas;  // the same per DNA mesh (LOD), for the whole body
		for (const TPair<FName, FString>& Type : PawnLooksBodyTypes())
		{
			FLooksBody Target;
			if (!LooksReadBody(Type.Value, Angles, Target))
			{
				return 1;
			}
			TArray<TArray<FVector>>& PerLod = LodDeltas.Add(Type.Key);
			for (int32 Mesh = 0; Mesh < Reference.LodPositions.Num() && Mesh < Target.LodPositions.Num(); ++Mesh)
			{
				// No fade at the neck any more: the face's neck takes the same morphs (PawnFaces), so the seam moves together.
				PerLod.Add(LooksShapeDelta(Reference, Target, Mesh));
			}
			BodyDeltas.Add(Type.Key, PerLod[0]);
		}
		// Clothes follow the body part of the body type files only (not their heads), the same points the whole body
		// and the face's neck follow, so a collar moves with the neck under it.
		FLooksGrid ClothGrid;
		TMap<FName, TArray<FVector>> ClothDeltas;
		{
			const TArray<int32> BodyOnly = LooksBodyIndices(Reference);
			TArray<FVector> Points;
			for (const int32 i : BodyOnly)
			{
				Points.Add(Reference.Positions[i]);
			}
			for (const TPair<FName, TArray<FVector>>& Morph : BodyDeltas)
			{
				TArray<FVector>& Out = ClothDeltas.Add(Morph.Key);
				for (const int32 i : BodyOnly)
				{
					Out.Add(Morph.Value[i]);
				}
			}
			UE_LOG(LogPawnLooks, Display, TEXT("Clothes follow %d body points of %d in the body type files"), Points.Num(), Reference.Positions.Num());
			ClothGrid.Build(Points.IsEmpty() ? Reference.Positions : Points, 2.0);
			if (Points.IsEmpty())
			{
				ClothDeltas = BodyDeltas;
			}
		}
		if (bProbe)
		{
			for (const TPair<FName, TArray<FVector>>& Morph : BodyDeltas)
			{
				double Largest = 0.0;
				for (const FVector& D : Morph.Value)
				{
					Largest = FMath::Max(Largest, D.Size());
				}
				UE_LOG(LogPawnLooks, Display, TEXT("Morph %s: largest move %.2f cm"), *Morph.Key.ToString(), Largest);
			}
			return 0;
		}

		// Start clean: the generated folder holds only what this run makes.
		const FString Folder = FPackageName::LongPackageNameToFilename(PawnLooksFolder);
		IFileManager::Get().DeleteDirectory(*Folder, false, true);

		int32 Failures = 0;
		USkeletalMesh* WholeBody = nullptr;
		for (const TPair<FName, USkinnedMeshComponent*>& Component : Kellan)
		{
			USkeletalMesh* Original = Cast<USkeletalMesh>(Component.Value->GetSkinnedAsset());
			if (!Original || !Cast<USkeletalMeshComponent>(Component.Value) || Original == Face)
			{
				continue;
			}
			USkeletalMesh* Copy = Cast<USkeletalMesh>(Tools.DuplicateAsset(PawnLooksMorphMeshName(Original->GetName()), PawnLooksFolder, Original));
			// (WholeBody: the whole body once made, for the skin tones at the neck below.)
			if (!Copy)
			{
				UE_LOG(LogPawnLooks, Error, TEXT("Could not copy %s"), *Original->GetPathName());
				++Failures;
				continue;
			}
			if (Original == KellanBody)
			{
				// Kellan's body is only his hands and feet: his copy becomes the engine's whole body instead.
				if (LooksMakeFullBody(Copy, Reference, LodDeltas))
				{
					WholeBody = Copy;
				}
				else
				{
					UE_LOG(LogPawnLooks, Error, TEXT("Could not make the whole body; the copy keeps Kellan's hands and feet"));
					++Failures;
					LooksAddMorphs(Copy, DnaGrid, BodyDeltas, 1);
				}
			}
			else
			{
				LooksAddMorphs(Copy, ClothGrid, ClothDeltas, LooksClothNeighbours);
			}
			Failures += LooksSave(Copy) ? 0 : 1;
		}

		// Kellan's clothes materials were never meant for morph targets: their instances, on copies
		// of their base materials that may be.
		TMap<UMaterial*, UMaterial*> ClothBases;
		for (const TPair<FName, USkinnedMeshComponent*>& Component : Kellan)
		{
			USkeletalMesh* Mesh = Cast<USkeletalMesh>(Component.Value->GetSkinnedAsset());
			if (!Mesh || Mesh == Face || Mesh == KellanBody)
			{
				continue;
			}
			for (int32 Slot = 0; Slot < Mesh->GetMaterials().Num(); ++Slot)
			{
				UMaterialInterface* Material = Component.Value->OverrideMaterials.IsValidIndex(Slot) && Component.Value->OverrideMaterials[Slot]
					? Component.Value->OverrideMaterials[Slot].Get() : Mesh->GetMaterials()[Slot].MaterialInterface.Get();
				UMaterialInstance* Instance = Cast<UMaterialInstance>(Material);
				UMaterial* Base = Instance ? Instance->GetMaterial() : nullptr;
				if (!Base || Base->bUsedWithMorphTargets)
				{
					continue;
				}
				UMaterial*& Ours = ClothBases.FindOrAdd(Base);
				if (!Ours)
				{
					Ours = Cast<UMaterial>(Tools.DuplicateAsset(TEXT("M_PawnCloth_") + Base->GetName(), PawnLooksFolder, Base));
					if (!Ours)
					{
						++Failures;
						continue;
					}
					bool bRecompile = false;
					UMaterialEditingLibrary::SetMaterialUsage(Ours, MATUSAGE_SkeletalMesh, bRecompile);
					UMaterialEditingLibrary::SetMaterialUsage(Ours, MATUSAGE_MorphTargets, bRecompile);
					Ours->PostEditChange();
					Failures += LooksSave(Ours) ? 0 : 1;
				}
				// A new instance straight off our copy, with every value the original's chain of instances sets.
				auto* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
				Factory->InitialParent = Ours;
				auto* Copy = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(PawnLooksClothInstanceName(Instance->GetName()), PawnLooksFolder,
					UMaterialInstanceConstant::StaticClass(), Factory));
				if (!Copy)
				{
					++Failures;
					continue;
				}
				Copy->CopyMaterialUniformParametersEditorOnly(Instance, true);
				Copy->PostEditChange();
				Failures += LooksSave(Copy) ? 0 : 1;
			}
		}

		// Our skin material and Kellan's skin instances on it, then the horn.
		UTexture* BodyColor = nullptr;
		UTexture* BodyNormal = nullptr;
		UMaterialInterface* BodySkin = KellanBody->GetMaterials().Num() ? KellanBody->GetMaterials()[0].MaterialInterface : nullptr;
		if (BodySkin)
		{
			BodySkin->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColor")), BodyColor);
			BodySkin->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Normal")), BodyNormal);
		}
		UMaterial* Skin = LooksMakeSkinMaterial(Tools, BodyColor, BodyNormal);
		UMaterial* Part = LooksMakePartMaterial(Tools);
		if (!Skin || !Part)
		{
			UE_LOG(LogPawnLooks, Error, TEXT("Could not make the materials"));
			return 1;
		}
		Failures += LooksSave(Skin) ? 0 : 1;
		Failures += LooksSave(Part) ? 0 : 1;
		TArray<UMaterialInterface*> SkinSlots;
		if (BodySkin)
		{
			SkinSlots.Add(BodySkin);
		}
		for (const FSkeletalMaterial& Slot : Face->GetMaterials())
		{
			if (Slot.MaterialSlotName.ToString().StartsWith(TEXT("head_")) && Slot.MaterialInterface)
			{
				SkinSlots.AddUnique(Slot.MaterialInterface);
			}
		}
		for (UMaterialInterface* Original : SkinSlots)
		{
			UMaterialInstanceConstant* Instance = LooksMakeSkinInstance(Tools, Skin, Original);
			// The whole body: Kellan's body texture has underwear painted on, so the body takes the Creator's own
			// bare skin colour map and a skin normal map instead (copied in: the game can't read the plugin).
			if (Instance && Original == BodySkin)
			{
				UTexture* KellanColor = nullptr;
				Original->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColor")), KellanColor);
				UTexture2D* Color = LooksMakeBareBodyColor(Cast<UTexture2D>(KellanColor));
				UTexture2D* Normal = LooksCopyTexture(Tools, LooksBareBodyNormal, TEXT("T_PawnBody_Normal"), false);
				if (Color && Normal)
				{
					Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamBaseColor), Color);
					Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamSourceAverage), LooksSeamAverage(Color,
						WholeBody, LooksVertexPositions(*Face->GetMeshDescription(0))));
					Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamNormal), Normal);
					Instance->PostEditChange();
					Failures += LooksSave(Color) && LooksSave(Normal) ? 0 : 1;
				}
				else
				{
					UE_LOG(LogPawnLooks, Error, TEXT("The Creator's bare body textures are missing; the body keeps Kellan's (with underwear)"));
					++Failures;
				}
			}
			else if (Instance && WholeBody)
			{
				// Kellan's own face skin: its tone measured where the face meets the whole body.
				UTexture* Color = nullptr;
				Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(PawnLooksParamBaseColor), Color);
				Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamSourceAverage), LooksSeamAverage(Color, Face,
					LooksVertexPositions(*WholeBody->GetMeshDescription(0))));
				Instance->PostEditChange();
			}
			Failures += Instance && LooksSave(Instance) ? 0 : 1;
		}
		UStaticMesh* Horn = LooksMakeHorn(Part);
		Failures += Horn && LooksSave(Horn) ? 0 : 1;


		UE_LOG(LogPawnLooks, Display, TEXT("Pawn looks: done, %d problems"), Failures);
		return Failures == 0 ? 0 : 1;
	}
}

UPawnLooksCommandlet::UPawnLooksCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UPawnLooksCommandlet::Main(const FString& Params)
{
	return LooksGenerate(Params.Contains(TEXT("-Probe")));
}
