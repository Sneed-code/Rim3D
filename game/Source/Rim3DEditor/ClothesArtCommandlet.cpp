#include "ClothesArtCommandlet.h"

#include "AnimationRuntime.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "PawnLooks.h"

DEFINE_LOG_CATEGORY_STATIC(LogClothesArt, Log, All);

namespace
{
	const TCHAR* const ClothesFolder = TEXT("/Game/Rim3D/Generated/Clothes");
	const TCHAR* const ClothesKellanFace = TEXT("/Game/MetaHumans/Kellan/Face/Kellan_FaceMesh.Kellan_FaceMesh");

	bool ClothesSave(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
		UE_LOG(LogClothesArt, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *Package->GetName());
		return bSaved;
	}

	FLinearColor ClothesColor(const TSharedPtr<FJsonObject>& Entry)
	{
		const TArray<TSharedPtr<FJsonValue>>& C = Entry->GetArrayField(TEXT("color"));
		return FLinearColor(C[0]->AsNumber(), C[1]->AsNumber(), C[2]->AsNumber());
	}

	/** One flat lit material that skinned meshes with morph targets can use, two sided (the cut edges show the cloth's inside). */
	UMaterial* ClothesMaster()
	{
		static UMaterial* Master = nullptr;
		if (Master)
		{
			return Master;
		}
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		Master = Cast<UMaterial>(Tools.CreateAsset(TEXT("M_Garment"), ClothesFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Master)
		{
			return nullptr;
		}
		Master->TwoSided = true;
		// Holes for the torn look (U16): the mask cuts them where a noise on the garment's own UVs is under the wear.
		Master->BlendMode = BLEND_Masked;
		Master->OpacityMaskClipValue = 0.5f;
		bool bRecompile = false;
		UMaterialEditingLibrary::SetMaterialUsage(Master, MATUSAGE_SkeletalMesh, bRecompile);
		UMaterialEditingLibrary::SetMaterialUsage(Master, MATUSAGE_MorphTargets, bRecompile);
		// The colour and the wear can come from the pawn's custom primitive data (the colour of the cloth, RGB at 0, the
		// wear 0 to 1 at 3) so that one material instance per piece serves every pawn; the instance's own colour is the default.
		auto* Color = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionVectorParameter::StaticClass(), -700, 0));
		Color->ParameterName = TEXT("Color");
		Color->DefaultValue = FLinearColor(0.3f, 0.3f, 0.3f);
		Color->bUseCustomPrimitiveData = true;
		Color->PrimitiveDataIndex = 0;
		auto* Wear = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionScalarParameter::StaticClass(), -700, 150));
		Wear->ParameterName = TEXT("Wear");
		Wear->DefaultValue = 0.0f;
		Wear->bUseCustomPrimitiveData = true;
		Wear->PrimitiveDataIndex = 3;
		auto* Uv = Cast<UMaterialExpressionTextureCoordinate>(UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionTextureCoordinate::StaticClass(), -700, 300));
		Uv->CoordinateIndex = 0;
		auto* Torn = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionCustom::StaticClass(), -300, 100));
		Torn->Description = TEXT("Torn");
		Torn->OutputType = CMOT_Float4;
		Torn->Code = TEXT(
			"float2 q = UV * 9.0;\n"
			"float n = 0.0; float amp = 0.5;\n"
			"for (int i = 0; i < 3; i++) {\n"
			"  float2 id = floor(q); float2 f = frac(q); f = f * f * (3.0 - 2.0 * f);\n"
			"  float a = frac(sin(dot(id, float2(12.9898, 78.233))) * 43758.5453);\n"
			"  float b = frac(sin(dot(id + float2(1, 0), float2(12.9898, 78.233))) * 43758.5453);\n"
			"  float c = frac(sin(dot(id + float2(0, 1), float2(12.9898, 78.233))) * 43758.5453);\n"
			"  float d = frac(sin(dot(id + float2(1, 1), float2(12.9898, 78.233))) * 43758.5453);\n"
			"  n += amp * lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);\n"
			"  amp *= 0.5; q *= 2.3;\n"
			"}\n"
			"n /= 0.875;\n"
			"float w = saturate(Wear);\n"
			// Holes where the noise is under 55% of the wear, a dark frayed rim just outside them, and the whole cloth dirtier.
			"float hole = step(n, w * 0.55);\n"
			"float rim = smoothstep(w * 0.55, w * 0.55 + 0.1, n);\n"
			"float3 col = Color * lerp(1.0, 0.7, w) * lerp(0.5, 1.0, w > 0.001 ? rim : 1.0);\n"
			"return float4(col, 1.0 - hole);\n");
		Torn->Inputs.Reset();
		{
			FCustomInput& InUv = Torn->Inputs.AddDefaulted_GetRef();
			InUv.InputName = TEXT("UV");
			InUv.Input.Connect(0, Uv);
			FCustomInput& InColor = Torn->Inputs.AddDefaulted_GetRef();
			InColor.InputName = TEXT("Color");
			InColor.Input.Connect(0, Color);
			FCustomInput& InWear = Torn->Inputs.AddDefaulted_GetRef();
			InWear.InputName = TEXT("Wear");
			InWear.Input.Connect(0, Wear);
		}
		auto* Rgb = Cast<UMaterialExpressionComponentMask>(UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionComponentMask::StaticClass(), 100, 0));
		Rgb->R = Rgb->G = Rgb->B = 1;
		Rgb->A = 0;
		Rgb->Input.Connect(0, Torn);
		auto* Alpha = Cast<UMaterialExpressionComponentMask>(UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionComponentMask::StaticClass(), 100, 200));
		Alpha->R = Alpha->G = Alpha->B = 0;
		Alpha->A = 1;
		Alpha->Input.Connect(0, Torn);
		UMaterialEditingLibrary::ConnectMaterialProperty(Rgb, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Alpha, TEXT(""), MP_OpacityMask);
		auto* Rough = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionConstant::StaticClass(), -400, 400));
		Rough->R = 0.9f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		UMaterialEditingLibrary::RecompileMaterial(Master);
		ClothesSave(Master);
		return Master;
	}

	UMaterialInterface* ClothesInstance(const FString& Name, const FLinearColor& Color)
	{
		UMaterial* Master = ClothesMaster();
		if (!Master)
		{
			return nullptr;
		}
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		auto* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Master;
		auto* Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(TEXT("MI_") + Name, ClothesFolder, UMaterialInstanceConstant::StaticClass(), Factory));
		if (!Instance)
		{
			return nullptr;
		}
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, TEXT("Color"), Color);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		ClothesSave(Instance);
		return Instance;
	}

	bool ClothesNameHas(const FString& Bone, const TArray<FString>& Words)
	{
		for (const FString& Word : Words)
		{
			if (Bone.Contains(Word))
			{
				return true;
			}
		}
		return false;
	}

	/** Cuts the triangles whose vertices all weigh most on a bone named with one of the words, then thickens what is left along its normals. */
	void ClothesCutAndThicken(USkeletalMesh* Mesh, const TArray<FString>& Drop, double Inflate)
	{
		for (int32 Lod = 0; Lod < Mesh->GetLODNum(); ++Lod)
		{
			if (!Mesh->HasMeshDescription(Lod))
			{
				continue;
			}
			FMeshDescription* Description = Mesh->GetMeshDescription(Lod);
			FSkeletalMeshAttributes Attributes(*Description);
			const FSkeletalMeshAttributes::FBoneNameAttributesRef BoneNames = Attributes.GetBoneNames();
			FSkinWeightsVertexAttributesRef Skin = Attributes.GetVertexSkinWeights();

			TSet<FVertexID> Dropped;
			for (const FVertexID V : Description->Vertices().GetElementIDs())
			{
				int32 Best = INDEX_NONE;
				float BestWeight = -1.0f;
				for (const UE::AnimationCore::FBoneWeight& W : Skin.Get(V))
				{
					if (W.GetWeight() > BestWeight)
					{
						BestWeight = W.GetWeight();
						Best = W.GetBoneIndex();
					}
				}
				const FString Bone = Best != INDEX_NONE && Best < Attributes.Bones().Num() ? BoneNames[FBoneID(Best)].ToString() : FString();
				if (ClothesNameHas(Bone, Drop))
				{
					Dropped.Add(V);
				}
			}
			TArray<FTriangleID> Doomed;
			if (Drop.Num() > 0)
			{
				for (const FTriangleID T : Description->Triangles().GetElementIDs())
				{
					bool bAll = true;
					for (const FVertexID V : Description->GetTriangleVertices(T))
					{
						bAll &= Dropped.Contains(V);
					}
					if (bAll)
					{
						Doomed.Add(T);
					}
				}
			}
			const int32 Before = Description->Triangles().Num();
			TSet<FEdgeID> DeadEdges;
			TSet<FVertexInstanceID> DeadInstances;
			for (const FTriangleID T : Doomed)
			{
				TArray<FEdgeID> OrphanEdges;
				TArray<FVertexInstanceID> OrphanInstances;
				TArray<FPolygonGroupID> OrphanGroups;
				Description->DeleteTriangle(T, &OrphanEdges, &OrphanInstances, &OrphanGroups);
				DeadEdges.Append(OrphanEdges);
				DeadInstances.Append(OrphanInstances);
			}
			for (const FEdgeID E : DeadEdges)
			{
				Description->DeleteEdge(E);
			}
			for (const FVertexInstanceID I : DeadInstances)
			{
				Description->DeleteVertexInstance(I);
			}
			TArray<FVertexID> DeadVertices;
			for (const FVertexID V : Description->Vertices().GetElementIDs())
			{
				if (Description->GetVertexVertexInstanceIDs(V).Num() == 0)
				{
					DeadVertices.Add(V);
				}
			}
			for (const FVertexID V : DeadVertices)
			{
				Description->DeleteVertex(V);
			}
			FElementIDRemappings Remap;
			Description->Compact(Remap);

			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			for (const FVertexID V : Description->Vertices().GetElementIDs())
			{
				FVector Sum = FVector::ZeroVector;
				for (const FVertexInstanceID I : Description->GetVertexVertexInstanceIDs(V))
				{
					Sum += FVector(Normals[I]);
				}
				Positions[V] += FVector3f(Sum.GetSafeNormal() * Inflate);
			}
			UE_LOG(LogClothesArt, Display, TEXT("  %s LOD %d: cut %d of %d triangles, thickened %.1f cm"), *Mesh->GetName(), Lod, Doomed.Num(), Before, Inflate);
			Mesh->CommitMeshDescription(Lod);
		}
		Mesh->PostEditChange();
	}

	bool ClothesMakeGarment(const TSharedPtr<FJsonObject>& Spec, int32 Index, const TSharedPtr<FJsonObject>& Part)
	{
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		const FString Def = Spec->GetStringField(TEXT("def"));
		const FString Name = FString::Printf(TEXT("%s_%d"), *Def, Index);
		const FString From = PawnLooksMorphMeshName(TEXT("m_med_nrw_") + Part->GetStringField(TEXT("from")));
		USkeletalMesh* Original = LoadObject<USkeletalMesh>(nullptr, *(FString(PawnLooksFolder) / From + TEXT(".") + From));
		if (!Original)
		{
			UE_LOG(LogClothesArt, Error, TEXT("%s is missing: run the PawnLooks commandlet first"), *From);
			return false;
		}
		USkeletalMesh* Copy = Cast<USkeletalMesh>(Tools.DuplicateAsset(TEXT("SK_") + Name, ClothesFolder, Original));
		if (!Copy)
		{
			return false;
		}
		TArray<FString> Drop;
		for (const TSharedPtr<FJsonValue>& Word : Part->GetArrayField(TEXT("drop")))
		{
			Drop.Add(Word->AsString());
		}
		ClothesCutAndThicken(Copy, Drop, Part->GetNumberField(TEXT("inflate")));
		UMaterialInterface* Material = ClothesInstance(Name, ClothesColor(Part));
		if (!Material)
		{
			return false;
		}
		TArray<FSkeletalMaterial> Materials = Copy->GetMaterials();
		for (FSkeletalMaterial& Slot : Materials)
		{
			Slot.MaterialInterface = Material;
		}
		Copy->SetMaterials(Materials);
		return ClothesSave(Copy);
	}

	// ---- Hats -------------------------------------------------------------------------------

	struct FClothesHead
	{
		FTransform Bone;    // the head bone in the mesh's space, reference pose
		double Crown = 0;   // top of the face mesh, cm, in the mesh's space
		FVector Centre;     // middle of the head at the crown
	};

	bool ClothesFindHead(FClothesHead& Out)
	{
		USkeletalMesh* Face = LoadObject<USkeletalMesh>(nullptr, ClothesKellanFace);
		const FString Hoodie = PawnLooksMorphMeshName(TEXT("m_med_nrw_top_hoodie_nrm_High"));
		USkeletalMesh* Body = LoadObject<USkeletalMesh>(nullptr, *(FString(PawnLooksFolder) / Hoodie + TEXT(".") + Hoodie));
		if (!Face || !Body || !Face->HasMeshDescription(0))
		{
			return false;
		}
		const FReferenceSkeleton& Ref = Body->GetRefSkeleton();
		const int32 Head = Ref.FindBoneIndex(TEXT("head"));
		if (Head == INDEX_NONE)
		{
			return false;
		}
		Out.Bone = FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Head);
		const FMeshDescription* Description = Face->GetMeshDescription(0);
		FSkeletalMeshConstAttributes Attributes(*Description);
		TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
		Out.Crown = -1e9;
		for (const FVertexID V : Description->Vertices().GetElementIDs())
		{
			Out.Crown = FMath::Max<double>(Out.Crown, Positions[V].Z);
		}
		FVector Sum = FVector::ZeroVector;
		int32 Count = 0;
		for (const FVertexID V : Description->Vertices().GetElementIDs())
		{
			if (Positions[V].Z > Out.Crown - 6.0)
			{
				Sum += FVector(Positions[V]);
				++Count;
			}
		}
		Out.Centre = Count ? Sum / Count : FVector::ZeroVector;
		UE_LOG(LogClothesArt, Display, TEXT("Head bone at %s, crown z %.1f cm, crown middle %s"), *Out.Bone.GetLocation().ToString(), Out.Crown, *Out.Centre.ToString());
		return true;
	}

	/** Moves the mesh by `Offset` (into place on the head, mesh space) and then into head bone space. */
	void ClothesToHeadBone(UStaticMesh* Mesh, const FClothesHead& Head, const FVector& Offset)
	{
		Mesh->ModifyMeshDescription(0);
		FMeshDescription* Description = Mesh->GetMeshDescription(0);
		FStaticMeshAttributes Attributes(*Description);
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		for (const FVertexID V : Description->Vertices().GetElementIDs())
		{
			Positions[V] = FVector3f(Head.Bone.InverseTransformPosition(FVector(Positions[V]) + Offset));
		}
		for (const FVertexInstanceID I : Description->VertexInstances().GetElementIDs())
		{
			Normals[I] = FVector3f(Head.Bone.InverseTransformVector(FVector(Normals[I])));
			Tangents[I] = FVector3f(Head.Bone.InverseTransformVector(FVector(Tangents[I])));
		}
		Mesh->CommitMeshDescription(0);
	}

	UStaticMesh* ClothesImportProp(const FString& Id)
	{
		const FString File = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("ThirdParty/ClothesArt"), Id, Id + TEXT("_1k.gltf"));
		if (!FPaths::FileExists(File))
		{
			UE_LOG(LogClothesArt, Error, TEXT("%s is not downloaded: run python tools/clothes_art.py"), *File);
			return nullptr;
		}
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = FString(ClothesFolder) / TEXT("Raw") / Id;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks({Task});
		for (UObject* Object : Task->GetObjects())
		{
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
			{
				return Mesh;
			}
		}
		return nullptr;
	}

	/** The upper part of an ellipsoid, radii in cm, from the pole down to `CutBelow` (cm from the middle). */
	void ClothesFillCap(UStaticMesh* Mesh, const FVector& Radii, double CutBelow)
	{
		Mesh->SetNumSourceModels(1);
		FMeshDescription* Description = Mesh->CreateMeshDescription(0);
		FStaticMeshAttributes Attributes(*Description);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector2f> Uvs = Attributes.GetVertexInstanceUVs();
		const FPolygonGroupID Group = Description->CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Garment");
		const double MaxPhi = FMath::Acos(FMath::Clamp(CutBelow / Radii.Z, -1.0, 1.0));
		const int32 Rings = 12, Sides = 24;
		TArray<FVertexInstanceID> Instances;
		for (int32 R = 0; R <= Rings; ++R)
		{
			const double Phi = MaxPhi * R / Rings;
			for (int32 S = 0; S < Sides; ++S)
			{
				const double Theta = 2.0 * PI * S / Sides;
				const FVector Unit(FMath::Sin(Phi) * FMath::Cos(Theta), FMath::Sin(Phi) * FMath::Sin(Theta), FMath::Cos(Phi));
				const FVertexID V = Description->CreateVertex();
				Positions[V] = FVector3f(Unit * Radii);
				const FVertexInstanceID I = Description->CreateVertexInstance(V);
				Normals[I] = FVector3f(FVector(Unit.X / Radii.X, Unit.Y / Radii.Y, Unit.Z / Radii.Z).GetSafeNormal());
				Tangents[I] = FVector3f(FVector(-FMath::Sin(Theta), FMath::Cos(Theta), 0.0));
				Signs[I] = 1.0f;
				Uvs.Set(I, 0, FVector2f(static_cast<float>(S) / Sides, static_cast<float>(R) / Rings));
				Instances.Add(I);
			}
		}
		for (int32 R = 0; R < Rings; ++R)
		{
			for (int32 S = 0; S < Sides; ++S)
			{
				const int32 A = R * Sides + S, B = R * Sides + (S + 1) % Sides, C = (R + 1) * Sides + S, D = (R + 1) * Sides + (S + 1) % Sides;
				if (R > 0)
				{
					Description->CreateTriangle(Group, {Instances[A], Instances[C], Instances[B]});
				}
				Description->CreateTriangle(Group, {Instances[B], Instances[C], Instances[D]});
			}
		}
		Mesh->CommitMeshDescription(0);
	}

	bool ClothesMakeProp(const TSharedPtr<FJsonObject>& Spec, const FClothesHead& Head, TMap<FString, UStaticMesh*>& Imported)
	{
		const FString Def = Spec->GetStringField(TEXT("def"));
		const FString Name = TEXT("SM_") + Def;
		UPackage* Package = CreatePackage(*(FString(ClothesFolder) / Name));
		UStaticMesh* Mesh = nullptr;
		FVector Offset(Head.Centre.X, Head.Centre.Y, Head.Crown);
		if (Spec->HasField(TEXT("id")))
		{
			const FString Id = Spec->GetStringField(TEXT("id"));
			UStaticMesh*& Raw = Imported.FindOrAdd(Id);
			if (!Raw)
			{
				Raw = ClothesImportProp(Id);
			}
			if (!Raw)
			{
				return false;
			}
			Mesh = DuplicateObject<UStaticMesh>(Raw, Package, *Name);
			Mesh->SetFlags(RF_Public | RF_Standalone);
			Mesh->NaniteSettings.bEnabled = false;
			Mesh->ModifyMeshDescription(0);
			FMeshDescription* Description = Mesh->GetMeshDescription(0);
			FStaticMeshAttributes Attributes(*Description);
			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			FBox Box(ForceInit);
			for (const FVertexID V : Description->Vertices().GetElementIDs())
			{
				Box += FVector(Positions[V]);
			}
			const TArray<TSharedPtr<FJsonValue>>& Size = Spec->GetArrayField(TEXT("size"));
			const FVector Wanted(Size[0]->AsNumber() * 100.0, Size[1]->AsNumber() * 100.0, Size[2]->AsNumber() * 100.0);
			const FVector Extent = Box.GetSize();
			const double Scale = FMath::Min3(Wanted.X / FMath::Max(Extent.X, 0.01), Wanted.Y / FMath::Max(Extent.Y, 0.01), Wanted.Z / FMath::Max(Extent.Z, 0.01));
			const FVector Middle((Box.Min.X + Box.Max.X) * 0.5, (Box.Min.Y + Box.Max.Y) * 0.5, Box.Min.Z);
			for (const FVertexID V : Description->Vertices().GetElementIDs())
			{
				Positions[V] = FVector3f((FVector(Positions[V]) - Middle) * Scale);
			}
			Mesh->CommitMeshDescription(0);
			Offset.Z -= Spec->GetNumberField(TEXT("sink"));
		}
		else
		{
			Mesh = NewObject<UStaticMesh>(Package, *Name, RF_Public | RF_Standalone);
			Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, TEXT("Garment"), TEXT("Garment")));
			const TArray<TSharedPtr<FJsonValue>>& R = Spec->GetArrayField(TEXT("radii"));
			ClothesFillCap(Mesh, FVector(R[0]->AsNumber(), R[1]->AsNumber(), R[2]->AsNumber()), Spec->GetNumberField(TEXT("cutBelow")));
			Offset.Z += Spec->GetNumberField(TEXT("centre"));
		}
		ClothesToHeadBone(Mesh, Head, Offset);
		UMaterialInterface* Material = ClothesInstance(Def, ClothesColor(Spec));
		for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num() && Material; ++Slot)
		{
			Mesh->SetMaterial(Slot, Material);
		}
		FStaticMeshSourceModel& Source = Mesh->GetSourceModel(0);
		Source.BuildSettings.bRecomputeNormals = false;
		Source.BuildSettings.bRecomputeTangents = true;
		Mesh->Build(true);
		Mesh->PostEditChange();
		return ClothesSave(Mesh);
	}
}

UClothesArtCommandlet::UClothesArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UClothesArtCommandlet::Main(const FString& Params)
{
	FString Text;
	const FString ListFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../assets/clothes_art.json")));
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *ListFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogClothesArt, Error, TEXT("Cannot read %s"), *ListFile);
		return 1;
	}
	IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(ClothesFolder), false, true);
	int32 Failed = 0;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("garments")))
	{
		const TSharedPtr<FJsonObject> Spec = Value->AsObject();
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& Part : Spec->GetArrayField(TEXT("parts")))
		{
			Failed += ClothesMakeGarment(Spec, Index++, Part->AsObject()) ? 0 : 1;
		}
	}
	FClothesHead Head;
	if (!ClothesFindHead(Head))
	{
		UE_LOG(LogClothesArt, Error, TEXT("No head: run the PawnLooks commandlet first"));
		return 1;
	}
	TMap<FString, UStaticMesh*> Imported;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("props")))
	{
		Failed += ClothesMakeProp(Value->AsObject(), Head, Imported) ? 0 : 1;
	}
	UE_LOG(LogClothesArt, Display, TEXT("%s"), Failed == 0 ? TEXT("All clothes made.") : TEXT("Some clothes failed."));
	return Failed == 0 ? 0 : 1;
}
