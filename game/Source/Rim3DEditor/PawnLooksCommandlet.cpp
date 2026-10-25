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
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "PawnLooks.h"

DEFINE_LOG_CATEGORY_STATIC(LogPawnLooks, Log, All);

namespace
{
	const TCHAR* const LooksKellanClass = TEXT("/Game/MetaHumans/Kellan/BP_Kellan.BP_Kellan_C");
	const TCHAR* const LooksFaceMesh = TEXT("/Game/MetaHumans/Kellan/Face/Kellan_FaceMesh.Kellan_FaceMesh");
	const TCHAR* const LooksSkinProfile = TEXT("/Game/MetaHumans/Common/Face/SubsurfaceProfiles/SSP_Head.SSP_Head");
	// The body Kellan has; the other body types are measured against it.
	const TCHAR* const LooksReferenceBody = TEXT("m_med_nrw");
	// Body morphs fade out over this distance from the neck seam, so the head (which keeps its
	// shape) still meets the body.
	constexpr double LooksSeamFadeCm = 7.0;
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
	// weights by bone name, and its joints as a skeleton in Unreal's terms.
	struct FLooksBody
	{
		FString Name;
		TArray<FVector> Positions;
		TArray<TArray<TPair<int32, float>>> Skin;  // per vertex: bone index in Bones, weight
		TArray<FName> Bones;
		TArray<FTransform> BoneWorld;  // bind pose, component space, by Bones
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
		const uint32 Count = Reader->GetVertexPositionCount(0);
		Out.Positions.SetNum(Count);
		Out.Skin.SetNum(Count);
		for (uint32 i = 0; i < Count; ++i)
		{
			Out.Positions[i] = Reader->GetVertexPosition(0, i);
		}
		for (uint32 i = 0; i < Reader->GetSkinWeightsCount(0) && i < Count; ++i)
		{
			const TArrayView<const float> Weights = Reader->GetSkinWeightsValues(0, i);
			const TArrayView<const uint16> Joints = Reader->GetSkinWeightsJointIndices(0, i);
			for (int32 k = 0; k < Weights.Num() && k < Joints.Num(); ++k)
			{
				Out.Skin[i].Add({Joints[k], Weights[k]});
			}
		}

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
	TArray<FVector> LooksShapeDelta(const FLooksBody& Reference, const FLooksBody& Target)
	{
		TArray<FVector> Delta;
		Delta.SetNumZeroed(Reference.Positions.Num());
		for (int32 i = 0; i < Reference.Positions.Num() && i < Target.Positions.Num(); ++i)
		{
			FVector Moved = FVector::ZeroVector;
			float Total = 0.0f;
			for (const TPair<int32, float>& W : Target.Skin[i])
			{
				if (!Target.BoneWorld.IsValidIndex(W.Key) || !Reference.BoneWorld.IsValidIndex(W.Key))
				{
					continue;
				}
				const FVector Local = Target.BoneWorld[W.Key].InverseTransformPosition(Target.Positions[i]);
				Moved += Reference.BoneWorld[W.Key].TransformPosition(Local) * W.Value;
				Total += W.Value;
			}
			Delta[i] = Total > 0.0f ? Moved / Total - Reference.Positions[i] : FVector::ZeroVector;
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
		auto* Ratio = LooksExpression(Material, UMaterialExpressionDivide::StaticClass(), -600, 100);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Skin, TEXT("RGB"), Ratio, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Average, TEXT("RGB"), Ratio, TEXT("B"));
		auto* Tinted = LooksExpression(Material, UMaterialExpressionMultiply::StaticClass(), -350, -100);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Color, TEXT("RGB"), Tinted, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Ratio, TEXT(""), Tinted, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Tinted, TEXT(""), MP_BaseColor);

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

		// Near Kellan's head (which keeps its shape, neck included) the body keeps its shape too,
		// fading in over LooksSeamFadeCm, so the head still meets the collar and shoulders.
		FLooksGrid FaceGrid;
		FaceGrid.Build(LooksVertexPositions(*Face->GetMeshDescription(0)), 2.0);
		TArray<float> Fade;
		Fade.SetNum(Reference.Positions.Num());
		int32 Held = 0;
		for (int32 i = 0; i < Reference.Positions.Num(); ++i)
		{
			double Distance = 0.0;
			FaceGrid.Nearest(Reference.Positions[i], LooksSeamFadeCm + 1.0, Distance);
			Fade[i] = FMath::SmoothStep(0.0f, 1.0f, static_cast<float>(FMath::Min(Distance, LooksSeamFadeCm) / LooksSeamFadeCm));
			Held += Fade[i] < 1.0f ? 1 : 0;
		}
		UE_LOG(LogPawnLooks, Display, TEXT("%d of the body file's vertices are near the head and change less"), Held);

		TMap<FName, TArray<FVector>> BodyDeltas;  // per vertex of the reference DNA body
		for (const TPair<FName, FString>& Type : PawnLooksBodyTypes())
		{
			FLooksBody Target;
			if (!LooksReadBody(Type.Value, Angles, Target))
			{
				return 1;
			}
			TArray<FVector>& Delta = BodyDeltas.Add(Type.Key, LooksShapeDelta(Reference, Target));
			for (int32 i = 0; i < Delta.Num(); ++i)
			{
				Delta[i] *= Fade[i];
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
		for (const TPair<FName, USkinnedMeshComponent*>& Component : Kellan)
		{
			USkeletalMesh* Original = Cast<USkeletalMesh>(Component.Value->GetSkinnedAsset());
			if (!Original || !Cast<USkeletalMeshComponent>(Component.Value) || Original == Face)
			{
				continue;
			}
			USkeletalMesh* Copy = Cast<USkeletalMesh>(Tools.DuplicateAsset(PawnLooksMorphMeshName(Original->GetName()), PawnLooksFolder, Original));
			if (!Copy)
			{
				UE_LOG(LogPawnLooks, Error, TEXT("Could not copy %s"), *Original->GetPathName());
				++Failures;
				continue;
			}
			LooksAddMorphs(Copy, DnaGrid, BodyDeltas, Original == KellanBody ? 1 : LooksClothNeighbours);
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
