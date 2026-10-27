#include "PawnFacesCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "DNAReader.h"
#include "DNAUtils.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "GroomAsset.h"
#include "GroomBindingAsset.h"
#include "GroomComponent.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "MetaHumanCharacter.h"
#include "MetaHumanCharacterIdentity.h"
#include "MetaHumanCommonDataUtils.h"
#include "MetaHumanFaceTextureSynthesizer.h"
#include "MetaHumanRigEvaluatedState.h"
#include "SkelMeshDNAUtils.h"
#include "SkeletalMeshAttributes.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "GroomAssetInterpolation.h"
#include "GroomAssetPhysics.h"
#include "GroomAssetRendering.h"
#include "GroomBindingBuilder.h"
#include "HairAttributes.h"
#include "HairDescription.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "ImageCore.h"
#include "MaterialEditingLibrary.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneDepth.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "PawnFaces.h"
#include "PawnLooks.h"

DEFINE_LOG_CATEGORY_STATIC(LogPawnFaces, Log, All);

namespace
{
	const TCHAR* const FacesKellanClass = TEXT("/Game/MetaHumans/Kellan/BP_Kellan.BP_Kellan_C");
	const TCHAR* const FacesKellanFace = TEXT("/Game/MetaHumans/Kellan/Face/Kellan_FaceMesh.Kellan_FaceMesh");
	const TCHAR* const FacesPresetFolder = TEXT("/MetaHumanCharacter/Optional/Presets");

	FString FacesPluginContent()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MetaHumanCharacter"));
		return Plugin.IsValid() ? Plugin->GetContentDir() : FString();
	}

	void FacesLogMaterial(const TCHAR* Indent, int32 Slot, const FName SlotName, const UMaterialInterface* Material)
	{
		const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
		UE_LOG(LogPawnFaces, Display, TEXT("%sslot %d %s: %s (base %s, morph targets %d)"), Indent, Slot, *SlotName.ToString(),
			Material ? *Material->GetPathName() : TEXT("none"), Base ? *Base->GetName() : TEXT("none"), Base ? Base->bUsedWithMorphTargets : 0);
	}

	void FacesLogGroom(const UGroomAsset* Groom, const UGroomBindingAsset* Binding)
	{
		if (Groom)
		{
			UE_LOG(LogPawnFaces, Display, TEXT("    groom %s: %d groups, %d cards, %d meshes"), *Groom->GetPathName(), Groom->GetNumHairGroups(),
				Groom->GetHairGroupsCards().Num(), Groom->GetHairGroupsMeshes().Num());
			for (const FHairGroupsMaterial& Material : Groom->GetHairGroupsMaterials())
			{
				UE_LOG(LogPawnFaces, Display, TEXT("      material %s: %s"), *Material.SlotName.ToString(), Material.Material ? *Material.Material->GetPathName() : TEXT("none"));
			}
			TArray<FName> Dependencies;
			IAssetRegistry::GetChecked().GetDependencies(Groom->GetOutermost()->GetFName(), Dependencies);
			for (const FName& Dependency : Dependencies)
			{
				UE_LOG(LogPawnFaces, Display, TEXT("      uses %s"), *Dependency.ToString());
			}
		}
		if (Binding)
		{
			UE_LOG(LogPawnFaces, Display, TEXT("    binding %s: type %d, source %s, target %s, groom %s"), *Binding->GetPathName(),
				static_cast<int32>(Binding->GetGroomBindingType()),
				Binding->GetSourceSkeletalMesh() ? *Binding->GetSourceSkeletalMesh()->GetPathName() : TEXT("none"),
				Binding->GetTargetSkeletalMesh() ? *Binding->GetTargetSkeletalMesh()->GetPathName() : TEXT("none"),
				Binding->GetGroom() ? *Binding->GetGroom()->GetPathName() : TEXT("none"));
		}
	}

	int32 FacesProbe()
	{
		IAssetRegistry::GetChecked().SearchAllAssets(true);

		// Kellan's components.
		UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(LoadClass<AActor>(nullptr, FacesKellanClass));
		if (!Generated || !Generated->SimpleConstructionScript)
		{
			UE_LOG(LogPawnFaces, Error, TEXT("No Kellan"));
			return 1;
		}
		for (USCS_Node* Node : Generated->SimpleConstructionScript->GetAllNodes())
		{
			UActorComponent* Template = Node->ComponentTemplate;
			const USceneComponent* Scene = Cast<USceneComponent>(Template);
			UE_LOG(LogPawnFaces, Display, TEXT("Component %s (%s) parent %s socket %s"), *Node->GetVariableName().ToString(),
				Template ? *Template->GetClass()->GetName() : TEXT("?"), *Node->ParentComponentOrVariableName.ToString(),
				Scene ? *Scene->GetAttachSocketName().ToString() : TEXT(""));
			if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Template))
			{
				const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Skinned->GetSkinnedAsset());
				if (Mesh)
				{
					UE_LOG(LogPawnFaces, Display, TEXT("    mesh %s: %d LODs, %d morphs, mesh description LOD0 %d"), *Mesh->GetPathName(), Mesh->GetLODNum(),
						Mesh->GetMorphTargets().Num(), Mesh->HasMeshDescription(0));
					for (int32 Slot = 0; Slot < Mesh->GetMaterials().Num(); ++Slot)
					{
						const UMaterialInterface* Material = Skinned->OverrideMaterials.IsValidIndex(Slot) && Skinned->OverrideMaterials[Slot]
							? Skinned->OverrideMaterials[Slot].Get() : Mesh->GetMaterials()[Slot].MaterialInterface.Get();
						FacesLogMaterial(TEXT("    "), Slot, Mesh->GetMaterials()[Slot].MaterialSlotName, Material);
					}
				}
			}
			if (const UGroomComponent* Groom = Cast<UGroomComponent>(Template))
			{
				FacesLogGroom(Groom->GroomAsset, Groom->BindingAsset);
			}
		}

		// Kellan's face DNA.
		USkeletalMesh* Face = LoadObject<USkeletalMesh>(nullptr, FacesKellanFace);
		const TSharedPtr<IDNAReader> KellanDna = Face ? USkelMeshDNAUtils::GetDNAReader(Face) : nullptr;
		if (!KellanDna)
		{
			UE_LOG(LogPawnFaces, Error, TEXT("Kellan's face has no DNA"));
			return 1;
		}
		UE_LOG(LogPawnFaces, Display, TEXT("Kellan's face DNA: %d meshes, %d LODs, %d joints, %d blend shape channels"), KellanDna->GetMeshCount(),
			KellanDna->GetLODCount(), KellanDna->GetJointCount(), KellanDna->GetBlendShapeChannelCount());
		for (uint16 Mesh = 0; Mesh < KellanDna->GetMeshCount(); ++Mesh)
		{
			FBox Box(ForceInit);
			for (uint32 V = 0; V < KellanDna->GetVertexPositionCount(Mesh); ++V)
			{
				Box += KellanDna->GetVertexPosition(Mesh, V);
			}
			UE_LOG(LogPawnFaces, Display, TEXT("  mesh %d %s: %d vertices, box %s"), Mesh, *KellanDna->GetMeshName(Mesh), KellanDna->GetVertexPositionCount(Mesh), *Box.ToString());
		}
		for (int32 Lod = 0; Lod < Face->GetLODNum(); ++Lod)
		{
			UE_LOG(LogPawnFaces, Display, TEXT("  skeletal LOD %d: mesh description %d, %d vertices"), Lod, Face->HasMeshDescription(Lod),
				Face->HasMeshDescription(Lod) ? Face->GetMeshDescription(Lod)->Vertices().Num() : 0);
		}

		// The Creator's face model, on its archetype DNA.
		const FString Content = FacesPluginContent();
		const TSharedPtr<IDNAReader> Archetype = ReadDNAFromFile(FMetaHumanCommonDataUtils::GetArchetypeDNAPath(EMetaHumanImportDNAType::Face), EDNADataLayer::All);
		if (Content.IsEmpty() || !Archetype)
		{
			UE_LOG(LogPawnFaces, Error, TEXT("No MetaHumanCharacter plugin content or archetype DNA"));
			return 1;
		}
		FMetaHumanCharacterIdentity Identity;
		if (!Identity.Init(Content / TEXT("Face/IdentityTemplate"), Content / TEXT("Body/IdentityTemplate"), Archetype->Unwrap(), EMetaHumanCharacterOrientation::Y_UP))
		{
			UE_LOG(LogPawnFaces, Error, TEXT("The face model did not load"));
			return 1;
		}
		UE_LOG(LogPawnFaces, Display, TEXT("Face model: %d archetype meshes; presets %s"), Archetype->GetMeshCount(), *FString::Join(Identity.GetPresetNames(), TEXT(", ")));
		for (const TCHAR* Name : {TEXT("Ada"), TEXT("Bruce"), TEXT("Omari")})
		{
			const FString Path = FString::Printf(TEXT("%s/%s.%s"), FacesPresetFolder, Name, Name);
			const UMetaHumanCharacter* Preset = LoadObject<UMetaHumanCharacter>(nullptr, *Path);
			const TSharedPtr<FMetaHumanCharacterIdentity::FState> State = Identity.CreateState();
			if (!Preset || !State || !State->Deserialize(Preset->GetFaceStateData()))
			{
				UE_LOG(LogPawnFaces, Error, TEXT("Preset %s did not load"), *Path);
				continue;
			}
			State->SetFaceScale(Preset->FaceEvaluationSettings.HeadScale);
			const FMetaHumanRigEvaluatedState Evaluated = State->Evaluate();
			UE_LOG(LogPawnFaces, Display, TEXT("Preset %s: %d vertices evaluated, head scale %.3f, face DNA %d"), Name, Evaluated.Vertices.Num(),
				Preset->FaceEvaluationSettings.HeadScale, Preset->HasFaceDNA());
			for (uint16 Mesh = 0; Mesh < KellanDna->GetMeshCount() && Mesh < 9; ++Mesh)
			{
				FBox Box(ForceInit);
				double Moved = 0.0;
				const uint32 Count = KellanDna->GetVertexPositionCount(Mesh);
				for (uint32 V = 0; V < Count; ++V)
				{
					const FVector P(State->GetVertex(Evaluated.Vertices, Mesh, V));
					Box += P;
					Moved += FVector::Dist(P, FVector(KellanDna->GetVertexPosition(Mesh, V)));
				}
				UE_LOG(LogPawnFaces, Display, TEXT("  mesh %d: box %s, average distance from Kellan's %.2f"), Mesh, *Box.ToString(), Count ? Moved / Count : 0.0);
			}
		}

		// The eye material's parameters (for eye colour).
		if (const UMaterialInterface* Eye = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/MetaHumans/Kellan/Face/Materials/MI_MetaHumanEye_Simplified.MI_MetaHumanEye_Simplified")))
		{
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Eye->GetAllVectorParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& Info : Infos)
			{
				FLinearColor Value;
				Eye->GetVectorParameterValue(Info, Value);
				UE_LOG(LogPawnFaces, Display, TEXT("Eye vector %s = %s"), *Info.Name.ToString(), *Value.ToString());
			}
			Eye->GetAllScalarParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& Info : Infos)
			{
				float Value = 0.0f;
				Eye->GetScalarParameterValue(Info, Value);
				UE_LOG(LogPawnFaces, Display, TEXT("Eye scalar %s = %.3f"), *Info.Name.ToString(), Value);
			}
			Eye->GetAllTextureParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& Info : Infos)
			{
				UTexture* Value = nullptr;
				Eye->GetTextureParameterValue(Info, Value);
				UE_LOG(LogPawnFaces, Display, TEXT("Eye texture %s = %s"), *Info.Name.ToString(), Value ? *Value->GetPathName() : TEXT("none"));
			}
		}
		// Archetype meshes and a preset's evaluated head against the archetype.
		for (uint16 Mesh = 0; Mesh < Archetype->GetMeshCount() && Mesh < 12; ++Mesh)
		{
			UE_LOG(LogPawnFaces, Display, TEXT("Archetype mesh %d %s: %d vertices"), Mesh, *Archetype->GetMeshName(Mesh), Archetype->GetVertexPositionCount(Mesh));
		}
		{
			const TSharedPtr<FMetaHumanCharacterIdentity::FState> Base = Identity.CreateState();
			const FMetaHumanRigEvaluatedState Evaluated = Base->Evaluate();
			for (uint16 Mesh = 0; Mesh < 9; ++Mesh)
			{
				FBox Box(ForceInit);
				FBox Raw(ForceInit);
				for (uint32 V = 0; V < Archetype->GetVertexPositionCount(Mesh); ++V)
				{
					Box += FVector(Base->GetVertex(Evaluated.Vertices, Mesh, V));
					Raw += FVector(Archetype->GetVertexPosition(Mesh, V));
				}
				UE_LOG(LogPawnFaces, Display, TEXT("Archetype state mesh %d box %s; DNA file box %s"), Mesh, *Box.ToString(), *Raw.ToString());
			}
			// Kellan's skeletal LOD0 by material slot.
			const FMeshDescription* Description = Face->GetMeshDescription(0);
			FSkeletalMeshConstAttributes Attributes(*Description);
			const auto Names = Attributes.GetPolygonGroupMaterialSlotNames();
			for (const FPolygonGroupID Group : Description->PolygonGroups().GetElementIDs())
			{
				FBox Box(ForceInit);
				for (const FTriangleID Triangle : Description->GetPolygonGroupTriangles(Group))
				{
					for (const FVertexID V : Description->GetTriangleVertices(Triangle))
					{
						Box += FVector(Attributes.GetVertexPositions()[V]);
					}
				}
				UE_LOG(LogPawnFaces, Display, TEXT("Kellan LOD0 group %s: box %s"), *Names[Group].ToString(), *Box.ToString());
			}
		}

		// One of the Creator's grooms and its binding.
		FacesLogGroom(LoadObject<UGroomAsset>(nullptr, TEXT("/MetaHumanCharacter/Optional/Grooms/GroomAssets/Hair/Hair_S_BuzzCut/Hair_S_BuzzCut.Hair_S_BuzzCut")),
			LoadObject<UGroomBindingAsset>(nullptr, TEXT("/MetaHumanCharacter/Optional/Grooms/Bindings/Hair/Hair_S_BuzzCut_Binding.Hair_S_BuzzCut_Binding")));
		return 0;
	}
}

namespace
{
	const TCHAR* const FacesGroomRoot = TEXT("/MetaHumanCharacter/Optional/Grooms/GroomAssets");
	const TCHAR* const FacesHairMaterial = TEXT("/Game/MetaHumans/Common/Materials/MI_Hair.MI_Hair");
	const TCHAR* const FacesKellanEye = TEXT("/Game/MetaHumans/Kellan/Face/Materials/MI_MetaHumanEye_Simplified.MI_MetaHumanEye_Simplified");
	const TCHAR* const FacesIrisChart = TEXT("/Game/MetaHumans/Common/Face/Textures/T_iris_color_picker.T_iris_color_picker");
	const TCHAR* const FacesMicroNormal = TEXT("/Game/MetaHumans/Common/Face/Textures/T_SkinMicroNormal001.T_SkinMicroNormal001");
	// The DNA meshes of the face model's LOD 0, in order; Kellan's mesh sections are found by these names.
	const TCHAR* const FacesParts[] = {TEXT("head"), TEXT("teeth"), TEXT("saliva"), TEXT("eyeLeft"), TEXT("eyeRight"), TEXT("eyeshell"),
		TEXT("eyelashes"), TEXT("eyeEdge"), TEXT("cartilage")};
	constexpr int32 FacesPartCount = UE_ARRAY_COUNT(FacesParts);
	// Kellan's face vertices follow the average of this many nearest face model vertices of the same part.
	constexpr int32 FacesNeighbours = 4;
	// A slider at 0 or 1 moves the face this many standard deviations of the presets along the region's main variation.
	constexpr double FacesSliderSigmas = 1.6;

	bool FacesSave(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *File, Args);
		UE_LOG(LogPawnFaces, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *File);
		return bSaved;
	}

	// Which face model part a section of Kellan's mesh is, from its material slot name.
	int32 FacesPartOf(const FString& SlotName)
	{
		for (int32 Part = FacesPartCount - 1; Part >= 1; --Part)
		{
			if (SlotName.Contains(FacesParts[Part]))
			{
				return Part;
			}
		}
		return 0;
	}

	// ---- Points --------------------------------------------------------------------------------

	struct FFacesGrid
	{
		double Cell = 1.0;
		TArray<FVector> Points;
		TMap<FIntVector, TArray<int32>> Cells;

		FIntVector Key(const FVector& P) const
		{
			return FIntVector(FMath::FloorToInt32(P.X / Cell), FMath::FloorToInt32(P.Y / Cell), FMath::FloorToInt32(P.Z / Cell));
		}

		void Build(const TArray<FVector>& InPoints, double InCell)
		{
			Cell = InCell;
			Points = InPoints;
			Cells.Reset();
			for (int32 i = 0; i < Points.Num(); ++i)
			{
				Cells.FindOrAdd(Key(Points[i])).Add(i);
			}
		}

		void Nearest(const FVector& Q, int32 K, TArray<TPair<int32, double>>& Out) const
		{
			Out.Reset();
			const FIntVector C = Key(Q);
			for (int32 Ring = 0; Ring <= 40; ++Ring)
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
	};

	// One face's vertices, by part, in the face model's frame (cm).
	using FFacesShape = TArray<TArray<FVector>>;

	FFacesShape FacesEvaluate(const FMetaHumanCharacterIdentity::FState& State, const IDNAReader& Archetype)
	{
		const FMetaHumanRigEvaluatedState Evaluated = State.Evaluate();
		FFacesShape Out;
		Out.SetNum(FacesPartCount);
		for (int32 Part = 0; Part < FacesPartCount; ++Part)
		{
			const uint32 Count = Archetype.GetVertexPositionCount(Part);
			Out[Part].SetNum(Count);
			for (uint32 V = 0; V < Count; ++V)
			{
				Out[Part][V] = FVector(State.GetVertex(Evaluated.Vertices, Part, V));
			}
		}
		return Out;
	}

	FVector FacesCentroid(const TArray<FVector>& Points)
	{
		FVector Sum = FVector::ZeroVector;
		for (const FVector& P : Points)
		{
			Sum += P;
		}
		return Points.Num() ? Sum / Points.Num() : Sum;
	}

	// Where a face's regions are, measured on the face model's average face (its frame, cm; +Y forward, +Z up).
	struct FFacesLandmarks
	{
		FVector NoseTip = FVector::ZeroVector;
		double EyeZ = 0.0;
		double MouthZ = 0.0;
		double Unit = 7.0;  // eyes to mouth

		static double Smooth(double A, double B, double X)
		{
			return FMath::SmoothStep(A, B, X);
		}

		// 1 over the face, fading out under the chin, behind the cheeks and up the forehead.
		double Face(const FVector& P) const
		{
			const double Neck = Smooth(MouthZ - 1.5 * Unit, MouthZ - 0.9 * Unit, P.Z);
			const double Front = Smooth(NoseTip.Y - 2.0 * Unit, NoseTip.Y - 1.4 * Unit, P.Y);
			const double Top = 1.0 - Smooth(EyeZ + 0.75 * Unit, EyeZ + 1.1 * Unit, P.Z);
			return Neck * Front * Top;
		}

		static double Ellipsoid(const FVector& P, const FVector& Centre, const FVector& Radii)
		{
			const FVector D = (P - Centre) / Radii;
			return 1.0 - Smooth(0.7, 1.25, D.Size());
		}

		double Nose(const FVector& P) const
		{
			const FVector Centre(0.0, NoseTip.Y - 1.2, (EyeZ + MouthZ + 0.3 * Unit) * 0.5);
			return Face(P) * Ellipsoid(P, Centre, FVector(2.4, 2.8, 0.6 * Unit));
		}

		double Brow(const FVector& P) const
		{
			return Face(P) * Ellipsoid(P, FVector(0.0, NoseTip.Y - 2.6, EyeZ + 0.4 * Unit), FVector(6.5, 3.2, 0.35 * Unit));
		}

		double Jaw(const FVector& P) const
		{
			return Face(P) * (1.0 - Smooth(MouthZ - 0.1 * Unit, MouthZ + 0.25 * Unit, P.Z));
		}

		double Region(int32 Index, const FVector& P) const
		{
			switch (Index)
			{
			case 0: return Face(P);
			case 1: return Brow(P);
			case 2: return Nose(P);
			default: return Jaw(P);
			}
		}
	};

	// The main way a set of (already centred) shapes varies: unit direction and standard deviation, from the
	// presets' Gram matrix by power iteration.
	void FacesMainVariation(const TArray<TArray<float>>& Rows, TArray<float>& OutDirection, double& OutSigma)
	{
		const int32 N = Rows.Num();
		const int32 M = N ? Rows[0].Num() : 0;
		TArray<double> Gram;
		Gram.SetNumZeroed(N * N);
		for (int32 a = 0; a < N; ++a)
		{
			for (int32 b = a; b < N; ++b)
			{
				double Dot = 0.0;
				for (int32 k = 0; k < M; ++k)
				{
					Dot += static_cast<double>(Rows[a][k]) * Rows[b][k];
				}
				Gram[a * N + b] = Gram[b * N + a] = Dot;
			}
		}
		TArray<double> Vec;
		Vec.Init(1.0, N);
		double Lambda = 0.0;
		for (int32 Step = 0; Step < 200; ++Step)
		{
			TArray<double> Next;
			Next.SetNumZeroed(N);
			for (int32 a = 0; a < N; ++a)
			{
				for (int32 b = 0; b < N; ++b)
				{
					Next[a] += Gram[a * N + b] * Vec[b];
				}
			}
			double Norm = 0.0;
			for (const double V : Next)
			{
				Norm += V * V;
			}
			Norm = FMath::Sqrt(Norm);
			if (Norm <= 0.0)
			{
				break;
			}
			Lambda = Norm;
			for (int32 a = 0; a < N; ++a)
			{
				Vec[a] = Next[a] / Norm;
			}
		}
		OutDirection.SetNumZeroed(M);
		for (int32 a = 0; a < N; ++a)
		{
			for (int32 k = 0; k < M; ++k)
			{
				OutDirection[k] += static_cast<float>(Vec[a] * Rows[a][k]);
			}
		}
		double Norm = 0.0;
		for (const float V : OutDirection)
		{
			Norm += static_cast<double>(V) * V;
		}
		Norm = FMath::Sqrt(Norm);
		for (float& V : OutDirection)
		{
			V = Norm > 0.0 ? static_cast<float>(V / Norm) : 0.0f;
		}
		OutSigma = N ? FMath::Sqrt(Lambda / N) : 0.0;
	}

	// ---- Faces ---------------------------------------------------------------------------------

	bool FacesMakeFace(IAssetTools& Tools, const TSharedPtr<FJsonObject>& Config, USkeletalMesh*& OutFace, FTransform& OutModelToKellan)
	{
		USkeletalMesh* Kellan = LoadObject<USkeletalMesh>(nullptr, FacesKellanFace);
		const FString Content = FacesPluginContent();
		const TSharedPtr<IDNAReader> Archetype = ReadDNAFromFile(FMetaHumanCommonDataUtils::GetArchetypeDNAPath(EMetaHumanImportDNAType::Face), EDNADataLayer::All);
		FMetaHumanCharacterIdentity Identity;
		if (!Kellan || Content.IsEmpty() || !Archetype
			|| !Identity.Init(Content / TEXT("Face/IdentityTemplate"), Content / TEXT("Body/IdentityTemplate"), Archetype->Unwrap(), EMetaHumanCharacterOrientation::Y_UP))
		{
			UE_LOG(LogPawnFaces, Error, TEXT("Kellan's face, the MetaHumanCharacter plugin or its face model is missing"));
			return false;
		}
		const TSharedPtr<FMetaHumanCharacterIdentity::FState> AverageState = Identity.CreateState();
		const FFacesShape Average = FacesEvaluate(*AverageState, *Archetype);

		// Landmarks on the average face.
		FFacesLandmarks Marks;
		Marks.NoseTip = FVector(0.0, -1.0e9, 0.0);
		for (const FVector& P : Average[0])
		{
			if (FMath::Abs(P.X) < 1.0 && P.Y > Marks.NoseTip.Y)
			{
				Marks.NoseTip = P;
			}
		}
		Marks.EyeZ = (FacesCentroid(Average[3]).Z + FacesCentroid(Average[4]).Z) * 0.5;
		Marks.MouthZ = FacesCentroid(Average[1]).Z;
		Marks.Unit = Marks.EyeZ - Marks.MouthZ;
		UE_LOG(LogPawnFaces, Display, TEXT("Average face: nose tip %s, eyes at z %.2f, mouth at z %.2f"), *Marks.NoseTip.ToString(), Marks.EyeZ, Marks.MouthZ);

		// The presets, as moves from the average face; each lined up on its neck.
		TArray<FFacesShape> Deltas;
		TArray<bool> Female;
		double NeckTop = TNumericLimits<double>::Max();
		for (const FVector& P : Average[0])
		{
			NeckTop = FMath::Min(NeckTop, P.Z);
		}
		NeckTop += 8.0;
		const TSharedPtr<FJsonObject>* Presets = nullptr;
		Config->TryGetObjectField(TEXT("presets"), Presets);
		for (const TCHAR* Gender : {TEXT("female"), TEXT("male")})
		{
			const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
			if (!Presets || !(*Presets)->TryGetArrayField(Gender, Names))
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& Name : *Names)
			{
				const FString Path = FString::Printf(TEXT("%s/%s.%s"), FacesPresetFolder, *Name->AsString(), *Name->AsString());
				const UMetaHumanCharacter* Preset = LoadObject<UMetaHumanCharacter>(nullptr, *Path);
				const TSharedPtr<FMetaHumanCharacterIdentity::FState> State = Identity.CreateState();
				if (!Preset || !State || !State->Deserialize(Preset->GetFaceStateData()))
				{
					UE_LOG(LogPawnFaces, Warning, TEXT("Preset %s did not load; left out"), *Path);
					continue;
				}
				FFacesShape Shape = FacesEvaluate(*State, *Archetype);
				FVector Neck = FVector::ZeroVector;
				int32 NeckCount = 0;
				for (int32 V = 0; V < Shape[0].Num(); ++V)
				{
					if (Average[0][V].Z < NeckTop)
					{
						Neck += Shape[0][V] - Average[0][V];
						++NeckCount;
					}
				}
				Neck = NeckCount ? Neck / NeckCount : Neck;
				for (int32 Part = 0; Part < FacesPartCount; ++Part)
				{
					for (int32 V = 0; V < Shape[Part].Num(); ++V)
					{
						Shape[Part][V] = Shape[Part][V] - Average[Part][V] - Neck;
					}
				}
				Deltas.Add(MoveTemp(Shape));
				Female.Add(FCString::Strcmp(Gender, TEXT("female")) == 0);
			}
		}
		if (Deltas.Num() < 4)
		{
			UE_LOG(LogPawnFaces, Error, TEXT("Too few preset faces (%d)"), Deltas.Num());
			return false;
		}

		// Flatten: every part's vertices one after another, xyz.
		TArray<FVector> AllAverage;
		for (const TArray<FVector>& Part : Average)
		{
			AllAverage.Append(Part);
		}
		const int32 M = AllAverage.Num() * 3;
		const auto Flatten = [&](const FFacesShape& Shape)
		{
			TArray<float> Out;
			Out.Reserve(M);
			for (const TArray<FVector>& Part : Shape)
			{
				for (const FVector& D : Part)
				{
					Out.Add(static_cast<float>(D.X));
					Out.Add(static_cast<float>(D.Y));
					Out.Add(static_cast<float>(D.Z));
				}
			}
			return Out;
		};
		TArray<TArray<float>> Rows;
		TArray<float> MeanFemale, MeanMale;
		MeanFemale.SetNumZeroed(M);
		MeanMale.SetNumZeroed(M);
		int32 Females = 0;
		for (int32 p = 0; p < Deltas.Num(); ++p)
		{
			Rows.Add(Flatten(Deltas[p]));
			TArray<float>& Mean = Female[p] ? MeanFemale : MeanMale;
			Females += Female[p] ? 1 : 0;
			for (int32 k = 0; k < M; ++k)
			{
				Mean[k] += Rows[p][k];
			}
		}
		const int32 Males = Deltas.Num() - Females;
		for (int32 k = 0; k < M; ++k)
		{
			MeanFemale[k] /= FMath::Max(Females, 1);
			MeanMale[k] /= FMath::Max(Males, 1);
		}

		// The morphs, per vertex of the face model (Kellan's scale comes later).
		TMap<FName, TArray<FVector>> Morphs;
		{
			TArray<FVector>& Morph = Morphs.Add(PawnFacesMorphFemale);
			Morph.SetNumZeroed(AllAverage.Num());
			for (int32 v = 0; v < AllAverage.Num(); ++v)
			{
				const double W = Marks.Face(AllAverage[v]);
				Morph[v] = FVector(MeanFemale[v * 3] - MeanMale[v * 3], MeanFemale[v * 3 + 1] - MeanMale[v * 3 + 1], MeanFemale[v * 3 + 2] - MeanMale[v * 3 + 2]) * W;
			}
			// Each preset's share of the female face, so a mislabelled preset shows in the log.
			for (int32 p = 0; p < Deltas.Num(); ++p)
			{
				double Dot = 0.0;
				double Norm = 0.0;
				for (int32 v = 0; v < AllAverage.Num(); ++v)
				{
					const FVector Mid((MeanFemale[v * 3] + MeanMale[v * 3]) * 0.5, (MeanFemale[v * 3 + 1] + MeanMale[v * 3 + 1]) * 0.5, (MeanFemale[v * 3 + 2] + MeanMale[v * 3 + 2]) * 0.5);
					const FVector D(Rows[p][v * 3], Rows[p][v * 3 + 1], Rows[p][v * 3 + 2]);
					Dot += FVector::DotProduct(D - Mid, Morph[v]);
					Norm += Morph[v].SizeSquared();
				}
				UE_LOG(LogPawnFaces, Display, TEXT("  preset %d (%s): %.2f along the female face (+0.5 female, -0.5 male)"), p, Female[p] ? TEXT("female") : TEXT("male"),
					Norm > 0.0 ? Dot / Norm : 0.0);
			}
		}
		const FName RegionNames[] = {PawnFacesMorphShape, PawnFacesMorphBrow, PawnFacesMorphNose, PawnFacesMorphJaw};
		for (int32 Region = 0; Region < 4; ++Region)
		{
			TArray<float> Weight;
			Weight.SetNum(AllAverage.Num());
			for (int32 v = 0; v < AllAverage.Num(); ++v)
			{
				Weight[v] = static_cast<float>(Marks.Region(Region, AllAverage[v]));
			}
			TArray<TArray<float>> Masked;
			for (int32 p = 0; p < Deltas.Num(); ++p)
			{
				const TArray<float>& Mean = Female[p] ? MeanFemale : MeanMale;
				TArray<float>& Row = Masked.AddDefaulted_GetRef();
				Row.SetNum(M);
				for (int32 k = 0; k < M; ++k)
				{
					Row[k] = (Rows[p][k] - Mean[k]) * Weight[k / 3];
				}
			}
			TArray<float> Direction;
			double Sigma = 0.0;
			FacesMainVariation(Masked, Direction, Sigma);
			// Positive is bigger: the nose and brow forward, the face and jaw wider.
			double Sign = 0.0;
			for (int32 v = 0; v < AllAverage.Num(); ++v)
			{
				const FVector D(Direction[v * 3], Direction[v * 3 + 1], Direction[v * 3 + 2]);
				Sign += (Region == 1 || Region == 2) ? D.Y * Weight[v] : FMath::Sign(AllAverage[v].X) * D.X * Weight[v];
			}
			const double Scale = (Sign < 0.0 ? -1.0 : 1.0) * Sigma * FacesSliderSigmas;
			TArray<FVector>& Morph = Morphs.Add(RegionNames[Region]);
			Morph.SetNum(AllAverage.Num());
			double Largest = 0.0;
			for (int32 v = 0; v < AllAverage.Num(); ++v)
			{
				Morph[v] = FVector(Direction[v * 3], Direction[v * 3 + 1], Direction[v * 3 + 2]) * Scale;
				Largest = FMath::Max(Largest, Morph[v].Size());
			}
			UE_LOG(LogPawnFaces, Display, TEXT("Morph %s: sigma %.2f, largest move %.2f cm (before Kellan's scale)"), *RegionNames[Region].ToString(), Sigma, Largest);
		}

		// Kellan's face, copied, and the face model lined up on it: scale and offset from the parts that don't change
		// much (teeth, eyes and what sits on them).
		USkeletalMesh* Face = Cast<USkeletalMesh>(Tools.DuplicateAsset(PawnFacesMeshName(Kellan->GetName()), PawnFacesFolder, Kellan));
		if (!Face)
		{
			UE_LOG(LogPawnFaces, Error, TEXT("Could not copy Kellan's face"));
			return false;
		}
		TArray<TArray<FVector>> KellanParts;
		KellanParts.SetNum(FacesPartCount);
		{
			const FMeshDescription* Description = Face->GetMeshDescription(0);
			FSkeletalMeshConstAttributes Attributes(*Description);
			const auto SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
			for (const FPolygonGroupID Group : Description->PolygonGroups().GetElementIDs())
			{
				const int32 Part = FacesPartOf(SlotNames[Group].ToString());
				for (const FTriangleID Triangle : Description->GetPolygonGroupTriangles(Group))
				{
					for (const FVertexID V : Description->GetTriangleVertices(Triangle))
					{
						KellanParts[Part].Add(FVector(Attributes.GetVertexPositions()[V]));
					}
				}
			}
		}
		TArray<FVector> From, To;
		for (int32 Part = 1; Part < FacesPartCount; ++Part)
		{
			if (KellanParts[Part].Num() && Average[Part].Num())
			{
				From.Add(FacesCentroid(Average[Part]));
				To.Add(FacesCentroid(KellanParts[Part]));
			}
		}
		const FVector MeanFrom = FacesCentroid(From);
		const FVector MeanTo = FacesCentroid(To);
		double Cross = 0.0;
		double Square = 0.0;
		for (int32 i = 0; i < From.Num(); ++i)
		{
			Cross += FVector::DotProduct(From[i] - MeanFrom, To[i] - MeanTo);
			Square += (From[i] - MeanFrom).SizeSquared();
		}
		const double Scale = Square > 0.0 ? Cross / Square : 1.0;
		const FVector Offset = MeanTo - MeanFrom * Scale;
		double Residual = 0.0;
		for (int32 i = 0; i < From.Num(); ++i)
		{
			Residual = FMath::Max(Residual, FVector::Dist(From[i] * Scale + Offset, To[i]));
		}
		UE_LOG(LogPawnFaces, Display, TEXT("Face model onto Kellan: scale %.4f, offset %s, worst part centre %.2f cm"), Scale, *Offset.ToString(), Residual);
		OutModelToKellan = FTransform(FQuat::Identity, Offset, FVector(Scale));

		// Per part, a grid of the average face in Kellan's frame, with the morphs' deltas per vertex.
		TArray<FFacesGrid> Grids;
		TArray<int32> PartStart;
		Grids.SetNum(FacesPartCount);
		int32 Start = 0;
		for (int32 Part = 0; Part < FacesPartCount; ++Part)
		{
			TArray<FVector> Placed;
			for (const FVector& P : Average[Part])
			{
				Placed.Add(P * Scale + Offset);
			}
			Grids[Part].Build(Placed, 1.0);
			PartStart.Add(Start);
			Start += Average[Part].Num();
		}

		for (int32 Lod = 0; Lod < Face->GetLODNum(); ++Lod)
		{
			if (!Face->HasMeshDescription(Lod))
			{
				continue;
			}
			FMeshDescription* Description = Face->GetMeshDescription(Lod);
			FSkeletalMeshAttributes Attributes(*Description);
			const auto SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
			TArray<int32> PartOfVertex;
			PartOfVertex.Init(0, Description->Vertices().GetArraySize());
			for (const FPolygonGroupID Group : Description->PolygonGroups().GetElementIDs())
			{
				const int32 Part = FacesPartOf(SlotNames[Group].ToString());
				for (const FTriangleID Triangle : Description->GetPolygonGroupTriangles(Group))
				{
					for (const FVertexID V : Description->GetTriangleVertices(Triangle))
					{
						PartOfVertex[V.GetValue()] = Part;
					}
				}
			}
			TArray<TArray<TPair<int32, float>>> Weights;
			Weights.SetNum(Description->Vertices().GetArraySize());
			double Farthest = 0.0;
			TArray<TPair<int32, double>> Found;
			for (const FVertexID V : Description->Vertices().GetElementIDs())
			{
				const int32 Part = PartOfVertex[V.GetValue()];
				Grids[Part].Nearest(FVector(Attributes.GetVertexPositions()[V]), FacesNeighbours, Found);
				float Total = 0.0f;
				for (const TPair<int32, double>& F : Found)
				{
					const float W = 1.0f / static_cast<float>(FMath::Max(F.Value, 0.05));
					Weights[V.GetValue()].Add({PartStart[Part] + F.Key, W});
					Total += W;
				}
				for (TPair<int32, float>& W : Weights[V.GetValue()])
				{
					W.Value /= Total;
				}
				Farthest = FMath::Max(Farthest, Found.Num() ? Found[0].Value : 999.0);
			}
			for (const TPair<FName, TArray<FVector>>& Morph : Morphs)
			{
				Attributes.UnregisterMorphTargetAttribute(Morph.Key);
				Attributes.RegisterMorphTargetAttribute(Morph.Key, false);
				TVertexAttributesRef<FVector3f> Moves = Attributes.GetVertexMorphPositionDelta(Morph.Key);
				double Largest = 0.0;
				for (const FVertexID V : Description->Vertices().GetElementIDs())
				{
					FVector D = FVector::ZeroVector;
					for (const TPair<int32, float>& W : Weights[V.GetValue()])
					{
						D += Morph.Value[W.Key] * W.Value;
					}
					D *= Scale;
					Moves[V] = FVector3f(D);
					Largest = FMath::Max(Largest, D.Size());
				}
				UE_LOG(LogPawnFaces, Display, TEXT("  face LOD %d morph %s: largest move %.2f cm"), Lod, *Morph.Key.ToString(), Largest);
			}
			UE_LOG(LogPawnFaces, Display, TEXT("  face LOD %d: %d vertices, farthest from the face model %.2f cm"), Lod, Description->Vertices().Num(), Farthest);
			Face->CommitMeshDescription(Lod);
		}
		Face->PostEditChange();
		OutFace = Face;
		return FacesSave(Face);
	}

	// ---- Eyes ----------------------------------------------------------------------------------

	// LooksRules' eye colours (sRGB), read from the sim's data.
	TArray<FColor> FacesEyeColors()
	{
		TArray<FColor> Out;
		FString Text;
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../data/defs/Rules/Rules.json"));
		TArray<TSharedPtr<FJsonValue>> Defs;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Defs))
		{
			UE_LOG(LogPawnFaces, Warning, TEXT("Could not read %s; no eye colours"), *Path);
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& Def : Defs)
		{
			const TSharedPtr<FJsonObject> Object = Def->AsObject();
			const TArray<TSharedPtr<FJsonValue>>* Colors = nullptr;
			if (!Object || Object->GetStringField(TEXT("defName")) != TEXT("LooksRules") || !Object->TryGetArrayField(TEXT("eyeColors"), Colors))
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& Entry : *Colors)
			{
				const TArray<TSharedPtr<FJsonValue>> C = Entry->AsObject()->GetArrayField(TEXT("color"));
				if (C.Num() >= 3)
				{
					Out.Add(FColor(FMath::RoundToInt32(FMath::Clamp(C[0]->AsNumber(), 0.0, 1.0) * 255.0), FMath::RoundToInt32(FMath::Clamp(C[1]->AsNumber(), 0.0, 1.0) * 255.0),
						FMath::RoundToInt32(FMath::Clamp(C[2]->AsNumber(), 0.0, 1.0) * 255.0)));
				}
			}
		}
		return Out;
	}

	int32 FacesMakeEyes(IAssetTools& Tools)
	{
		UMaterialInterface* KellanEye = LoadObject<UMaterialInterface>(nullptr, FacesKellanEye);
		UTexture* Chart = LoadObject<UTexture>(nullptr, FacesIrisChart);
		UMaterial* Base = KellanEye ? KellanEye->GetMaterial() : nullptr;
		FImage Image;
		if (!Base || !Chart || !Chart->Source.IsValid() || !Chart->Source.GetMipImage(Image, 0))
		{
			UE_LOG(LogPawnFaces, Error, TEXT("Kellan's eye material or the iris colour chart is missing"));
			return 1;
		}
		UMaterial* Eye = Cast<UMaterial>(Tools.DuplicateAsset(PawnFacesEyeMaterial, PawnFacesFolder, Base));
		if (!Eye)
		{
			return 1;
		}
		bool bRecompile = false;
		UMaterialEditingLibrary::SetMaterialUsage(Eye, MATUSAGE_SkeletalMesh, bRecompile);
		UMaterialEditingLibrary::SetMaterialUsage(Eye, MATUSAGE_MorphTargets, bRecompile);
		Eye->PostEditChange();
		int32 Failures = FacesSave(Eye) ? 0 : 1;

		// The chart in sRGB, to pick the cell nearest each eye colour.
		Image.GammaSpace = Chart->SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
		FImage Srgb;
		Image.CopyTo(Srgb, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const TArrayView64<FColor> Pixels = Srgb.AsBGRA8();
		float KellanU = 0.0f, KellanV = 0.0f;
		KellanEye->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("IrisColor1U")), KellanU);
		KellanEye->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("IrisColor1V")), KellanV);
		const auto At = [&](float U, float V)
		{
			const int32 X = FMath::Clamp(FMath::FloorToInt32(U * Srgb.SizeX), 0, Srgb.SizeX - 1);
			const int32 Y = FMath::Clamp(FMath::FloorToInt32(V * Srgb.SizeY), 0, Srgb.SizeY - 1);
			return Pixels[static_cast<int64>(Y) * Srgb.SizeX + X];
		};
		UE_LOG(LogPawnFaces, Display, TEXT("Iris chart %dx%d; Kellan's iris (%.3f, %.3f) is %s"), Srgb.SizeX, Srgb.SizeY, KellanU, KellanV, *At(KellanU, KellanV).ToString());
		{
			auto* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
			Factory->InitialParent = Eye;
			auto* Default = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(PawnFacesEyeDefault, PawnFacesFolder, UMaterialInstanceConstant::StaticClass(), Factory));
			if (Default)
			{
				Default->CopyMaterialUniformParametersEditorOnly(KellanEye, true);
				Default->PostEditChange();
			}
			Failures += Default && FacesSave(Default) ? 0 : 1;
		}

		for (const FColor& Want : FacesEyeColors())
		{
			int64 Best = 0;
			int32 BestDistance = MAX_int32;
			for (int64 i = 0; i < Pixels.Num(); ++i)
			{
				const FColor& P = Pixels[i];
				const int32 D = FMath::Square(P.R - Want.R) + FMath::Square(P.G - Want.G) + FMath::Square(P.B - Want.B);
				if (D < BestDistance)
				{
					BestDistance = D;
					Best = i;
				}
			}
			const float U = (static_cast<float>(Best % Srgb.SizeX) + 0.5f) / Srgb.SizeX;
			const float V = (static_cast<float>(Best / Srgb.SizeX) + 0.5f) / Srgb.SizeY;
			auto* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
			Factory->InitialParent = Eye;
			auto* Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(PawnFacesEyeInstanceName(Want), PawnFacesFolder, UMaterialInstanceConstant::StaticClass(), Factory));
			if (!Instance)
			{
				++Failures;
				continue;
			}
			Instance->CopyMaterialUniformParametersEditorOnly(KellanEye, true);
			// The second iris colour a little darker (further down the chart), as in Kellan's.
			for (const TPair<const TCHAR*, float>& Value : TArray<TPair<const TCHAR*, float>>{{TEXT("IrisColor1U"), U}, {TEXT("IrisColor1V"), V},
				{TEXT("IrisColor2U"), U}, {TEXT("IrisColor2V"), FMath::Min(V + 0.05f, 0.99f)}})
			{
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Value.Key), Value.Value);
			}
			Instance->PostEditChange();
			UE_LOG(LogPawnFaces, Display, TEXT("Eye colour %s: chart (%.3f, %.3f) %s"), *Want.ToString(), U, V, *Pixels[Best].ToString());
			Failures += FacesSave(Instance) ? 0 : 1;
		}
		return Failures;
	}

	// ---- Grooms --------------------------------------------------------------------------------

	const TCHAR* const FacesKellanGroomHead = TEXT("/MetaHumanCharacter/Optional/Grooms/GroomMesh/SKM_Groom_Head_Legacy02.SKM_Groom_Head_Legacy02");

	TArray<FVector> FacesHeadVertices(USkeletalMesh* Head)
	{
		TArray<FVector> Out;
		if (Head && Head->HasMeshDescription(0))
		{
			const FMeshDescription* Description = Head->GetMeshDescription(0);
			FSkeletalMeshConstAttributes Attributes(*Description);
			Out.SetNum(Description->Vertices().GetArraySize());
			for (const FVertexID V : Description->Vertices().GetElementIDs())
			{
				Out[V.GetValue()] = FVector(Attributes.GetVertexPositions()[V]);
			}
		}
		return Out;
	}

	// The head a Creator groom was made on: its plugin binding's source mesh.
	USkeletalMesh* FacesGroomHead(const FString& GroomName)
	{
		TArray<FAssetData> Found;
		IAssetRegistry::GetChecked().GetAssetsByPath(FName(TEXT("/MetaHumanCharacter/Optional/Grooms/Bindings")), Found, true);
		for (const FAssetData& Asset : Found)
		{
			if (Asset.AssetName.ToString() == GroomName + TEXT("_Binding"))
			{
				const UGroomBindingAsset* Binding = Cast<UGroomBindingAsset>(Asset.GetAsset());
				return Binding ? Binding->GetSourceSkeletalMesh() : nullptr;
			}
		}
		return nullptr;
	}

	UGroomAsset* FacesFindGroom(const FString& Name)
	{
		TArray<FAssetData> Found;
		IAssetRegistry::GetChecked().GetAssetsByPath(FName(FacesGroomRoot), Found, true);
		for (const FAssetData& Asset : Found)
		{
			if (Asset.AssetName.ToString() == Name && Asset.AssetClassPath == UGroomAsset::StaticClass()->GetClassPathName())
			{
				return Cast<UGroomAsset>(Asset.GetAsset());
			}
		}
		return nullptr;
	}

	bool FacesBind(UGroomAsset* Groom, USkeletalMesh* Face, const FString& Name)
	{
		UPackage* Package = CreatePackage(*(FString(PawnFacesFolder) / Name));
		UGroomBindingAsset* Binding = NewObject<UGroomBindingAsset>(Package, *Name, RF_Public | RF_Standalone);
		Binding->SetGroomBindingType(EGroomBindingMeshType::SkeletalMesh);
		Binding->SetGroom(Groom);
		Binding->SetTargetSkeletalMesh(Face);
		Binding->SetNumInterpolationPoints(100);
		const bool bBuilt = FGroomBindingBuilder::BuildBinding(Binding, false);
		UE_LOG(LogPawnFaces, Display, TEXT("Binding %s: %s"), *Name, bBuilt ? TEXT("built") : TEXT("FAILED to build"));
		return bBuilt && FacesSave(Binding);
	}

	// A copy of one of the Creator's grooms for one of our styles: strands only (no cards or helmet meshes, which
	// live in the plugin), on Kellan's hair material, no simulation, bound to the shaped face.
	bool FacesCopyGroom(IAssetTools& Tools, const FString& Source, const FString& Kind, const FString& Style, USkeletalMesh* Face, UMaterialInterface* HairMaterial,
		const FTransform& ModelToKellan)
	{
		UGroomAsset* Original = FacesFindGroom(Source);
		if (!Original)
		{
			UE_LOG(LogPawnFaces, Error, TEXT("Groom %s not found under %s"), *Source, FacesGroomRoot);
			return false;
		}
		UGroomAsset* Copy = Cast<UGroomAsset>(Tools.DuplicateAsset(PawnFacesGroomName(Kind, Style), PawnFacesFolder, Original));
		if (!Copy)
		{
			return false;
		}
		// Each Creator groom was made on one of two heads: Legacy02 is Kellan's (same place and size), Legacy01 is the
		// face model's average head, lower and smaller. Strands made on another head are moved onto Kellan's by the
		// scale and offset that best fit that head's vertices onto Legacy02's (same layout, vertex by vertex).
		FTransform ToKellan = FTransform::Identity;
		USkeletalMesh* Head = FacesGroomHead(Source);
		USkeletalMesh* KellanHead = LoadObject<USkeletalMesh>(nullptr, FacesKellanGroomHead);
		if (!Head || !KellanHead)
		{
			ToKellan = ModelToKellan;
			UE_LOG(LogPawnFaces, Warning, TEXT("  %s: no head found for it; moved as the face model was"), *Source);
		}
		else if (Head != KellanHead)
		{
			const TArray<FVector> From = FacesHeadVertices(Head);
			const TArray<FVector> To = FacesHeadVertices(KellanHead);
			if (From.Num() != To.Num() || From.IsEmpty())
			{
				ToKellan = ModelToKellan;
				UE_LOG(LogPawnFaces, Warning, TEXT("  %s: its head %s doesn't match Kellan's vertex for vertex; moved as the face model was"), *Source, *Head->GetName());
			}
			else
			{
				const FVector MeanFrom = FacesCentroid(From);
				const FVector MeanTo = FacesCentroid(To);
				double Cross = 0.0, Square = 0.0;
				for (int32 i = 0; i < From.Num(); ++i)
				{
					Cross += FVector::DotProduct(From[i] - MeanFrom, To[i] - MeanTo);
					Square += (From[i] - MeanFrom).SizeSquared();
				}
				const double Scale = Square > 0.0 ? Cross / Square : 1.0;
				ToKellan = FTransform(FQuat::Identity, MeanTo - MeanFrom * Scale, FVector(Scale));
				UE_LOG(LogPawnFaces, Display, TEXT("  %s was made on %s: scale %.4f, offset %s onto Kellan's head"), *Source, *Head->GetName(), Scale,
					*ToKellan.GetLocation().ToString());
			}
		}
		for (const EHairDescriptionType Type : ToKellan.Equals(FTransform::Identity) ? TArray<EHairDescriptionType>() : TArray<EHairDescriptionType>{EHairDescriptionType::Source, EHairDescriptionType::Edit})
		{
			FHairDescription Description = Copy->LoadHairDescription(Type);
			if (!Description.IsValid())
			{
				continue;
			}
			TVertexAttributesRef<FVector3f> Positions = Description.VertexAttributes().GetAttributesRef<FVector3f>(HairAttribute::Vertex::Position);
			FBox Before(ForceInit), After(ForceInit);
			for (int32 V = 0; V < Description.GetNumVertices(); ++V)
			{
				const FVector P(Positions[FVertexID(V)]);
				Before += P;
				Positions[FVertexID(V)] = FVector3f(ToKellan.TransformPosition(P));
				After += ToKellan.TransformPosition(P);
			}
			UE_LOG(LogPawnFaces, Display, TEXT("  %s strands (%s): %s moved to %s"), *Source, Type == EHairDescriptionType::Source ? TEXT("source") : TEXT("edited"),
				*Before.ToString(), *After.ToString());
			Copy->CommitHairDescription(MoveTemp(Description), Type);
		}
		Copy->SetHairGroupsCards({});
		Copy->SetHairGroupsMeshes({});
		for (FHairGroupsMaterial& Material : Copy->GetHairGroupsMaterials())
		{
			Material.Material = HairMaterial;
		}
		for (FHairGroupsRendering& Rendering : Copy->GetHairGroupsRendering())
		{
			Rendering.Material = nullptr;
		}
		for (FHairGroupsPhysics& Physics : Copy->GetHairGroupsPhysics())
		{
			Physics.SolverSettings.EnableSimulation = false;
		}
		// Their lower LODs are cards and meshes: strands there too, thinned by the LOD's own decimation.
		for (FHairGroupsLOD& Lods : Copy->GetHairGroupsLOD())
		{
			for (FHairLODSettings& Lod : Lods.LODs)
			{
				Lod.GeometryType = EGroomGeometryType::Strands;
			}
		}
		Copy->PostEditChange();
		UE_LOG(LogPawnFaces, Display, TEXT("Groom %s from %s: %d groups"), *Copy->GetName(), *Source, Copy->GetNumHairGroups());
		return FacesSave(Copy) && FacesBind(Copy, Face, PawnFacesBindingName(Copy->GetName()));
	}

	int32 FacesMakeGrooms(IAssetTools& Tools, const TSharedPtr<FJsonObject>& Config, USkeletalMesh* Face, const FTransform& ModelToKellan)
	{
		UMaterialInterface* HairMaterial = LoadObject<UMaterialInterface>(nullptr, FacesHairMaterial);
		int32 Failures = 0;
		const TSharedPtr<FJsonObject>* Hair = nullptr;
		if (Config->TryGetObjectField(TEXT("hair"), Hair))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Style : (*Hair)->Values)
			{
				if (!Style.Value->AsString().IsEmpty())
				{
					Failures += FacesCopyGroom(Tools, Style.Value->AsString(), TEXT("Hair"), Style.Key, Face, HairMaterial, ModelToKellan) ? 0 : 1;
				}
			}
		}
		const TSharedPtr<FJsonObject>* Beards = nullptr;
		if (Config->TryGetObjectField(TEXT("beards"), Beards))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Style : (*Beards)->Values)
			{
				const TSharedPtr<FJsonObject> Parts = Style.Value->AsObject();
				const FString Beard = Parts ? Parts->GetStringField(TEXT("beard")) : FString();
				const FString Moustache = Parts ? Parts->GetStringField(TEXT("moustache")) : FString();
				Failures += Beard.IsEmpty() || FacesCopyGroom(Tools, Beard, TEXT("Beard"), Style.Key, Face, HairMaterial, ModelToKellan) ? 0 : 1;
				Failures += Moustache.IsEmpty() || FacesCopyGroom(Tools, Moustache, TEXT("Moustache"), Style.Key, Face, HairMaterial, ModelToKellan) ? 0 : 1;
			}
		}
		const TSharedPtr<FJsonObject>* Brows = nullptr;
		if (Config->TryGetObjectField(TEXT("eyebrows"), Brows))
		{
			for (const TPair<const TCHAR*, const TCHAR*> Gender : {TPair<const TCHAR*, const TCHAR*>(TEXT("female"), TEXT("F")), TPair<const TCHAR*, const TCHAR*>(TEXT("male"), TEXT("M"))})
			{
				const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
				if ((*Brows)->TryGetArrayField(Gender.Key, Names))
				{
					for (int32 i = 0; i < Names->Num(); ++i)
					{
						Failures += FacesCopyGroom(Tools, (*Names)[i]->AsString(), TEXT("Brows"), FString::Printf(TEXT("%s%d"), Gender.Value, i), Face, HairMaterial, ModelToKellan) ? 0 : 1;
					}
				}
			}
		}
		// Kellan's own grooms (his eyelashes always, his hair, brows and moustache for pawns from older saves) bound to the shaped face.
		UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(LoadClass<AActor>(nullptr, FacesKellanClass));
		for (USCS_Node* Node : Generated && Generated->SimpleConstructionScript ? Generated->SimpleConstructionScript->GetAllNodes() : TArray<USCS_Node*>())
		{
			if (const UGroomComponent* Groom = Cast<UGroomComponent>(Node->ComponentTemplate); Groom && Groom->GroomAsset)
			{
				Failures += FacesBind(Groom->GroomAsset, Face, PawnFacesKellanBindingName(Groom->GroomAsset->GetName())) ? 0 : 1;
			}
		}
		return Failures;
	}

	// ---- Skin marks (V3) -----------------------------------------------------------------------

	// A square greyscale mask painted with soft-edged shapes; coordinates 0 to 1, y down.
	struct FFacesMask
	{
		int32 Size = 256;
		TArray<float> Pixels;

		FFacesMask() { Pixels.SetNumZeroed(Size * Size); }

		// Paints `Value` where the signed distance (in mask units, negative inside) is below zero, anti-aliased.
		template <typename F>
		void Paint(F Distance, float Value = 1.0f)
		{
			const double Edge = 1.5 / Size;
			for (int32 y = 0; y < Size; ++y)
			{
				for (int32 x = 0; x < Size; ++x)
				{
					const FVector2D P((x + 0.5) / Size, (y + 0.5) / Size);
					const double Cover = FMath::Clamp(0.5 - Distance(P) / Edge, 0.0, 1.0);
					float& Pixel = Pixels[y * Size + x];
					Pixel = Value >= 0.0f ? FMath::Max(Pixel, static_cast<float>(Cover) * Value) : Pixel * (1.0f - static_cast<float>(Cover));
				}
			}
		}

		static double Segment(const FVector2D& P, const FVector2D& A, const FVector2D& B)
		{
			const FVector2D AB = B - A;
			const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(AB.SizeSquared(), 1.0e-9), 0.0, 1.0);
			return FVector2D::Distance(P, A + AB * T);
		}

		// A thick line through points, its half-width going from W0 to W1.
		void Stroke(const TArray<FVector2D>& Points, double W0, double W1, float Value = 1.0f)
		{
			for (int32 i = 0; i + 1 < Points.Num(); ++i)
			{
				const double Wa = FMath::Lerp(W0, W1, static_cast<double>(i) / (Points.Num() - 1));
				const double Wb = FMath::Lerp(W0, W1, static_cast<double>(i + 1) / (Points.Num() - 1));
				const FVector2D A = Points[i], B = Points[i + 1];
				Paint([&](const FVector2D& P)
				{
					const FVector2D AB = B - A;
					const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(AB.SizeSquared(), 1.0e-9), 0.0, 1.0);
					return FVector2D::Distance(P, A + AB * T) - FMath::Lerp(Wa, Wb, T);
				}, Value);
			}
		}

		void Disc(const FVector2D& C, double R, float Value = 1.0f) { Paint([&](const FVector2D& P) { return FVector2D::Distance(P, C) - R; }, Value); }
		void Ring(const FVector2D& C, double R, double W, float Value = 1.0f) { Paint([&](const FVector2D& P) { return FMath::Abs(FVector2D::Distance(P, C) - R) - W; }, Value); }
		void Ellipse(const FVector2D& C, const FVector2D& Radii, double W, double Angle)
		{
			TArray<FVector2D> Points;
			for (int32 i = 0; i <= 48; ++i)
			{
				const double A = 2.0 * PI * i / 48.0;
				const FVector2D Local(FMath::Cos(A) * Radii.X, FMath::Sin(A) * Radii.Y);
				Points.Add(C + Local.GetRotated(FMath::RadiansToDegrees(Angle)));
			}
			Stroke(Points, W, W);
		}
	};

	// Value noise, 0 to 1, for burns, ragged edges and hair.
	double FacesNoise(double X, double Y, int32 Seed)
	{
		const auto Hash = [Seed](int32 Ix, int32 Iy)
		{
			uint32 H = static_cast<uint32>(Ix) * 374761393u + static_cast<uint32>(Iy) * 668265263u + static_cast<uint32>(Seed) * 2246822519u;
			H = (H ^ (H >> 13)) * 1274126177u;
			return static_cast<double>(H ^ (H >> 16)) / 4294967295.0;
		};
		const int32 Ix = FMath::FloorToInt32(X), Iy = FMath::FloorToInt32(Y);
		const double Fx = FMath::SmoothStep(0.0, 1.0, X - Ix), Fy = FMath::SmoothStep(0.0, 1.0, Y - Iy);
		return FMath::Lerp(FMath::Lerp(Hash(Ix, Iy), Hash(Ix + 1, Iy), Fx), FMath::Lerp(Hash(Ix, Iy + 1), Hash(Ix + 1, Iy + 1), Fx), Fy);
	}

	// LooksRules' tattoos.styles, in order: Tribal, Stripes, Anchor, Rose, Sun, Skull, Chain, Wings, Dragon, Knot.
	FFacesMask FacesTattoo(int32 Style)
	{
		FFacesMask M;
		const FVector2D C(0.5, 0.5);
		const auto Curve = [](TFunctionRef<FVector2D(double)> At, int32 Count)
		{
			TArray<FVector2D> Points;
			for (int32 i = 0; i <= Count; ++i)
			{
				Points.Add(At(static_cast<double>(i) / Count));
			}
			return Points;
		};
		switch (Style)
		{
		case 0:  // Tribal: five sweeping spikes from the middle
			for (int32 k = 0; k < 5; ++k)
			{
				M.Stroke(Curve([k](double T) { const double A = k * 2.0 * PI / 5.0 + T * 1.8; return FVector2D(0.5 + FMath::Cos(A) * (0.06 + 0.36 * T), 0.5 + FMath::Sin(A) * (0.06 + 0.36 * T)); }, 24), 0.055, 0.004);
			}
			M.Disc(C, 0.08);
			break;
		case 1:  // Stripes: three wavy bands
			for (int32 k = 0; k < 3; ++k)
			{
				M.Stroke(Curve([k](double T) { return FVector2D(0.08 + 0.84 * T, 0.28 + 0.22 * k + 0.03 * FMath::Sin(T * 2.0 * PI)); }, 24), 0.045, 0.045);
			}
			break;
		case 2:  // Anchor
			M.Ring(FVector2D(0.5, 0.14), 0.055, 0.018);
			M.Stroke({FVector2D(0.5, 0.2), FVector2D(0.5, 0.8)}, 0.028, 0.028);
			M.Stroke({FVector2D(0.34, 0.3), FVector2D(0.66, 0.3)}, 0.022, 0.022);
			M.Stroke(Curve([](double T) { const double A = FMath::DegreesToRadians(-110.0 + 220.0 * T); return FVector2D(0.5 + FMath::Sin(A) * 0.27, 0.58 + FMath::Cos(A) * 0.22); }, 32), 0.026, 0.026);
			M.Stroke({FVector2D(0.23, 0.5), FVector2D(0.27, 0.62)}, 0.03, 0.01);
			M.Stroke({FVector2D(0.77, 0.5), FVector2D(0.73, 0.62)}, 0.03, 0.01);
			break;
		case 3:  // Rose: petals, a spiral heart, a stem and a leaf
			M.Paint([](const FVector2D& P)
			{
				const FVector2D D = P - FVector2D(0.5, 0.36);
				const double A = FMath::Atan2(D.Y, D.X);
				const double R = 0.2 * (0.72 + 0.28 * FMath::Abs(FMath::Cos(2.5 * A)));
				return FMath::Abs(D.Size() - R) - 0.018;
			});
			M.Stroke(Curve([](double T) { const double A = T * 4.0 * PI; return FVector2D(0.5 + FMath::Cos(A) * 0.12 * T, 0.36 + FMath::Sin(A) * 0.12 * T); }, 48), 0.014, 0.014);
			M.Stroke(Curve([](double T) { return FVector2D(0.5 + 0.04 * FMath::Sin(T * PI), 0.56 + 0.38 * T); }, 16), 0.02, 0.014);
			M.Ellipse(FVector2D(0.6, 0.74), FVector2D(0.08, 0.035), 0.014, -0.5);
			break;
		case 4:  // Sun: a disc and twelve rays
			M.Disc(C, 0.14);
			for (int32 k = 0; k < 12; ++k)
			{
				const double A = k * 2.0 * PI / 12.0;
				const FVector2D Dir(FMath::Cos(A), FMath::Sin(A));
				M.Stroke({C + Dir * 0.18, C + Dir * (k % 2 ? 0.34 : 0.43)}, 0.03, 0.004);
			}
			break;
		case 5:  // Skull: a filled head, eye and nose holes cut out, teeth
			M.Disc(FVector2D(0.5, 0.42), 0.26);
			M.Paint([](const FVector2D& P) { return FMath::Max(FMath::Abs(P.X - 0.5) - 0.15, FMath::Abs(P.Y - 0.7) - 0.11); });
			M.Disc(FVector2D(0.4, 0.45), 0.07, -1.0f);
			M.Disc(FVector2D(0.6, 0.45), 0.07, -1.0f);
			M.Paint([](const FVector2D& P) { return FMath::Max(FMath::Abs(P.X - 0.5) - 0.03, FMath::Abs(P.Y - 0.58) - 0.04); }, -1.0f);
			for (int32 k = 0; k < 4; ++k)
			{
				M.Stroke({FVector2D(0.41 + 0.06 * k, 0.68), FVector2D(0.41 + 0.06 * k, 0.8)}, 0.008, 0.008, -1.0f);
			}
			break;
		case 6:  // Chain: four links along a diagonal
			for (int32 k = 0; k < 4; ++k)
			{
				M.Ellipse(FVector2D(0.2 + 0.2 * k, 0.2 + 0.2 * k), k % 2 ? FVector2D(0.1, 0.05) : FVector2D(0.1, 0.06), 0.02, PI / 4.0);
			}
			break;
		case 7:  // Wings: two fans of feathers
			for (const double Side : {-1.0, 1.0})
			{
				for (int32 k = 0; k < 5; ++k)
				{
					const double A = FMath::DegreesToRadians(-35.0 + 18.0 * k);
					const double Length = 0.42 - 0.05 * k;
					M.Stroke(Curve([&](double T)
					{
						const double Bend = A + 0.35 * T;
						return FVector2D(0.5 + Side * (0.04 + FMath::Cos(Bend) * Length * T), 0.45 - FMath::Sin(Bend) * Length * T);
					}, 16), 0.03, 0.008);
				}
			}
			break;
		case 8:  // Dragon: a winding body with spikes, head to tail
			{
				const auto Body = [](double T) { return FVector2D(0.1 + 0.8 * T, 0.5 + 0.2 * FMath::Sin(T * 3.0 * PI)); };
				M.Stroke(Curve(Body, 48), 0.055, 0.008);
				M.Disc(Body(0.0) + FVector2D(-0.02, 0.0), 0.06);
				for (int32 k = 1; k < 9; ++k)
				{
					const double T = k / 10.0;
					const FVector2D P = Body(T);
					const FVector2D Tangent = (Body(T + 0.01) - P).GetSafeNormal();
					M.Stroke({P, P + FVector2D(Tangent.Y, -Tangent.X) * (0.09 - 0.007 * k)}, 0.018, 0.003);
				}
			}
			break;
		default:  // Knot: a closed interlaced loop in a ring
			M.Stroke(Curve([](double T) { const double A = T * 2.0 * PI; return FVector2D(0.5 + 0.3 * FMath::Sin(2.0 * A), 0.5 + 0.3 * FMath::Sin(3.0 * A)); }, 96), 0.022, 0.022);
			M.Ring(C, 0.42, 0.015);
			break;
		}
		return M;
	}

	// LooksRules' scars.styles, in order: Slash, Claw, Burn, Puncture, Ragged, Stitched.
	FFacesMask FacesScar(int32 Style)
	{
		FFacesMask M;
		const auto Jagged = [](const FVector2D& A, const FVector2D& B, double Amount, int32 Seed)
		{
			TArray<FVector2D> Points;
			const FVector2D Normal = FVector2D((B - A).Y, -(B - A).X).GetSafeNormal();
			for (int32 i = 0; i <= 24; ++i)
			{
				const double T = i / 24.0;
				Points.Add(FMath::Lerp(A, B, T) + Normal * (FacesNoise(T * 9.0, 0.5, Seed) - 0.5) * Amount * FMath::Sin(T * PI));
			}
			return Points;
		};
		switch (Style)
		{
		case 0:  // Slash
			M.Stroke(Jagged(FVector2D(0.12, 0.62), FVector2D(0.88, 0.38), 0.03, 1), 0.006, 0.022);
			break;
		case 1:  // Claw: three parallel cuts
			for (int32 k = 0; k < 3; ++k)
			{
				M.Stroke(Jagged(FVector2D(0.25 + 0.15 * k, 0.15), FVector2D(0.15 + 0.15 * k, 0.85), 0.02, 2 + k), 0.016, 0.005);
			}
			break;
		case 2:  // Burn: a mottled blotch
			M.Paint([](const FVector2D& P)
			{
				const double R = 0.3 * (0.75 + 0.5 * FacesNoise(P.X * 7.0, P.Y * 7.0, 7));
				return FVector2D::Distance(P, FVector2D(0.5, 0.5)) - R;
			});
			for (float& Pixel : M.Pixels)
			{
				Pixel *= 0.75f;
			}
			break;
		case 3:  // Puncture: a pit and a faint ring
			M.Disc(FVector2D(0.5, 0.5), 0.07);
			M.Ring(FVector2D(0.5, 0.5), 0.12, 0.015, 0.45f);
			break;
		case 4:  // Ragged: a wide torn line
			M.Stroke(Jagged(FVector2D(0.1, 0.5), FVector2D(0.9, 0.55), 0.12, 11), 0.03, 0.03);
			break;
		default:  // Stitched: a line and its stitch marks
			M.Stroke({FVector2D(0.12, 0.5), FVector2D(0.88, 0.5)}, 0.012, 0.012);
			for (int32 k = 0; k < 8; ++k)
			{
				const double X = 0.16 + 0.1 * k;
				M.Stroke({FVector2D(X - 0.015, 0.42), FVector2D(X + 0.015, 0.58)}, 0.007, 0.007);
			}
			break;
		}
		return M;
	}

	// Body hair: many short strokes, mostly downward, thinning to the edge.
	FFacesMask FacesBodyHair()
	{
		FFacesMask M;
		FRandomStream Random(1234);
		TArray<float> Strokes;
		Strokes.SetNumZeroed(M.Size * M.Size);
		for (int32 k = 0; k < 2600; ++k)
		{
			const FVector2D A(Random.FRand(), Random.FRand());
			const double Angle = FMath::DegreesToRadians(90.0 + Random.FRandRange(-35.0, 35.0));
			const double Length = Random.FRandRange(0.015, 0.035);
			const FVector2D B = A + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Length;
			// Rasterise the stroke directly (one pixel wide): faster than painting each as a shape.
			const int32 Steps = FMath::CeilToInt32(Length * M.Size * 2.0);
			for (int32 s = 0; s <= Steps; ++s)
			{
				const FVector2D P = FMath::Lerp(A, B, static_cast<double>(s) / Steps);
				const int32 X = FMath::Clamp(FMath::FloorToInt32(P.X * M.Size), 0, M.Size - 1);
				const int32 Y = FMath::Clamp(FMath::FloorToInt32(P.Y * M.Size), 0, M.Size - 1);
				Strokes[Y * M.Size + X] = 1.0f;
			}
		}
		for (int32 y = 0; y < M.Size; ++y)
		{
			for (int32 x = 0; x < M.Size; ++x)
			{
				const double D = FVector2D::Distance(FVector2D((x + 0.5) / M.Size, (y + 0.5) / M.Size), FVector2D(0.5, 0.5));
				M.Pixels[y * M.Size + x] = Strokes[y * M.Size + x] * static_cast<float>(1.0 - FMath::SmoothStep(0.25, 0.5, D)) * 0.85f;
			}
		}
		return M;
	}

	UTexture2D* FacesSaveMask(const FString& Name, const FFacesMask& Mask)
	{
		UPackage* Package = CreatePackage(*(FString(PawnFacesFolder) / Name));
		UTexture2D* Texture = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone);
		TArray<uint8> Bytes;
		Bytes.SetNum(Mask.Pixels.Num());
		for (int32 i = 0; i < Mask.Pixels.Num(); ++i)
		{
			Bytes[i] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(Mask.Pixels[i] * 255.0f), 0, 255));
		}
		Texture->Source.Init(Mask.Size, Mask.Size, 1, 1, TSF_G8, Bytes.GetData());
		Texture->CompressionSettings = TC_Grayscale;
		Texture->SRGB = false;
		Texture->PostEditChange();
		return FacesSave(Texture) ? Texture : nullptr;
	}

	template <typename T>
	T* FacesNode(UMaterial* Material, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y));
	}

	void FacesConnect(UMaterialExpression* From, const TCHAR* FromPin, UMaterialExpression* To, const TCHAR* ToPin)
	{
		if (!UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromPin, To, ToPin))
		{
			UE_LOG(LogPawnFaces, Error, TEXT("Could not connect %s to %s pin '%s'"), *From->GetName(), *To->GetName(), ToPin);
		}
	}

	// The mark decal: a mask texture's coverage times an opacity, in one colour, and only where the skin (which the pawn
	// draws into custom depth while it has marks) is the nearest surface, so marks never land on clothes.
	UMaterial* FacesMakeMarkMaterial(IAssetTools& Tools, UTexture* DefaultMask)
	{
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(PawnFacesMarkMaterial, PawnFacesFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		Material->MaterialDomain = MD_DeferredDecal;
		Material->BlendMode = BLEND_Translucent;
		Material->DecalBlendMode = DBM_Translucent;

		auto* Color = FacesNode<UMaterialExpressionVectorParameter>(Material, -700, -200);
		Color->ParameterName = PawnFacesParamColor;
		Color->DefaultValue = FLinearColor(0.02f, 0.02f, 0.04f);
		UMaterialEditingLibrary::ConnectMaterialProperty(Color, TEXT("RGB"), MP_BaseColor);
		auto* Roughness = FacesNode<UMaterialExpressionConstant>(Material, -700, 0);
		Roughness->R = 0.6f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);

		auto* Mask = FacesNode<UMaterialExpressionTextureSampleParameter2D>(Material, -1200, 150);
		Mask->ParameterName = PawnFacesParamMask;
		Mask->Texture = DefaultMask;
		Mask->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(DefaultMask);
		auto* Opacity = FacesNode<UMaterialExpressionScalarParameter>(Material, -1200, 400);
		Opacity->ParameterName = PawnFacesParamOpacity;
		Opacity->DefaultValue = 1.0f;
		auto* Covered = FacesNode<UMaterialExpressionMultiply>(Material, -900, 250);
		FacesConnect(Mask, TEXT("R"), Covered, TEXT("A"));
		FacesConnect(Opacity, TEXT(""), Covered, TEXT("B"));

		// Skin gate: 1 where custom depth (the skin) is within 2 mm of the scene depth, 0 behind cloth, off the body,
		// or if custom depth is not there at all.
		auto* Custom = FacesNode<UMaterialExpressionSceneTexture>(Material, -1500, 600);
		Custom->SceneTextureId = PPI_CustomDepth;
		auto* CustomR = FacesNode<UMaterialExpressionComponentMask>(Material, -1300, 600);
		CustomR->R = true;
		FacesConnect(Custom, TEXT("Color"), CustomR, TEXT(""));
		auto* Scene = FacesNode<UMaterialExpressionSceneDepth>(Material, -1500, 750);
		auto* Gap = FacesNode<UMaterialExpressionSubtract>(Material, -1100, 650);
		FacesConnect(CustomR, TEXT(""), Gap, TEXT("A"));
		FacesConnect(Scene, TEXT(""), Gap, TEXT("B"));
		auto* Distance = FacesNode<UMaterialExpressionAbs>(Material, -1000, 650);
		FacesConnect(Gap, TEXT(""), Distance, TEXT(""));
		auto* Scaled = FacesNode<UMaterialExpressionMultiply>(Material, -950, 650);
		auto* PerCm = FacesNode<UMaterialExpressionConstant>(Material, -1100, 800);
		PerCm->R = 5.0f;
		FacesConnect(Distance, TEXT(""), Scaled, TEXT("A"));
		FacesConnect(PerCm, TEXT(""), Scaled, TEXT("B"));
		auto* Inverse = FacesNode<UMaterialExpressionOneMinus>(Material, -800, 650);
		FacesConnect(Scaled, TEXT(""), Inverse, TEXT(""));
		auto* Gate = FacesNode<UMaterialExpressionSaturate>(Material, -650, 650);
		FacesConnect(Inverse, TEXT(""), Gate, TEXT(""));
		auto* Final = FacesNode<UMaterialExpressionMultiply>(Material, -450, 400);
		FacesConnect(Covered, TEXT(""), Final, TEXT("A"));
		FacesConnect(Gate, TEXT(""), Final, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Final, TEXT(""), MP_Opacity);
		Material->PostEditChange();
		return Material;
	}

	int32 FacesMakeMarks(IAssetTools& Tools)
	{
		int32 Failures = 0;
		UTexture* First = nullptr;
		for (int32 Style = 0; Style < 10; ++Style)
		{
			UTexture* Texture = FacesSaveMask(PawnFacesTattooTexture(Style), FacesTattoo(Style));
			First = First ? First : Texture;
			Failures += Texture ? 0 : 1;
		}
		for (int32 Style = 0; Style < 6; ++Style)
		{
			Failures += FacesSaveMask(PawnFacesScarTexture(Style), FacesScar(Style)) ? 0 : 1;
		}
		Failures += FacesSaveMask(PawnFacesBodyHairTexture, FacesBodyHair()) ? 0 : 1;
		UMaterial* Material = FacesMakeMarkMaterial(Tools, First);
		Failures += Material && FacesSave(Material) ? 0 : 1;
		return Failures;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Face skins (V2b): Kellan's face texture is a man's (stubble, a man's brow), so each Creator preset named in "skins"
	// gets its own face colour and normal map from the plugin's texture synthesis model (offline, 1024 px), from the
	// preset's own skin settings. Each goes on our skin material like Kellan's, so the pawn's skin colour still drives it.

	const TCHAR* const FacesSynthesisModel = TEXT("Optional/TextureSynthesis/TS-1.3-F_UE_res-1024_nchr-153");

	UTexture2D* FacesSaveImage(const FString& Name, const FImage& Image, bool bColor)
	{
		UPackage* Package = CreatePackage(*(FString(PawnFacesFolder) / Name));
		UTexture2D* Texture = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone);
		Texture->PreEditChange(nullptr);
		Texture->Source.Init(Image);
		Texture->SetModernSettingsForNewOrChangedTexture();
		Texture->CompressionSettings = bColor ? TC_Default : TC_Normalmap;
		Texture->LODGroup = bColor ? TEXTUREGROUP_Character : TEXTUREGROUP_CharacterNormalMap;
		Texture->SRGB = bColor;
		Texture->MipGenSettings = TMGS_FromTextureGroup;
		Texture->PostEditChange();
		return FacesSave(Texture) ? Texture : nullptr;
	}

	// The same average PawnLooks takes of Kellan's textures (LooksAverageColor): linear, skipping the black outside
	// the UV islands and the brightest texels.
	FLinearColor FacesAverageColor(const FImage& Image)
	{
		FImage Linear;
		Image.CopyTo(Linear, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		const TArrayView64<FLinearColor> Pixels = Linear.AsRGBA32F();
		FLinearColor Sum(0, 0, 0, 0);
		int64 Count = 0;
		for (int64 i = 0; i < Pixels.Num(); i += 7)
		{
			const float Luminance = Pixels[i].GetLuminance();
			if (Luminance > 0.02f && Luminance < 0.9f)
			{
				Sum += Pixels[i];
				++Count;
			}
		}
		const FLinearColor Average = Count ? Sum / static_cast<float>(Count) : FLinearColor(0.5f, 0.5f, 0.5f);
		return FLinearColor(Average.R, Average.G, Average.B, 1.0f);
	}

	int32 FacesMakeSkins(IAssetTools& Tools, const TSharedPtr<FJsonObject>& Config)
	{
		// Kellan's face skin instance (PawnLooks made it) is the template: our skin material, his roughness and specular.
		USkeletalMesh* Kellan = LoadObject<USkeletalMesh>(nullptr, FacesKellanFace);
		UMaterialInstanceConstant* Template = nullptr;
		for (const FSkeletalMaterial& Slot : Kellan ? Kellan->GetMaterials() : TArray<FSkeletalMaterial>())
		{
			const FString Name = Slot.MaterialInterface ? PawnLooksSkinInstanceName(Slot.MaterialInterface->GetName()) : FString();
			if (!Template && !Name.IsEmpty() && FPackageName::DoesPackageExist(FString(PawnLooksFolder) / Name))
			{
				Template = LoadObject<UMaterialInstanceConstant>(nullptr, *PawnLooksAssetPath(Name));
			}
		}
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MetaHumanCharacter"));
		FMetaHumanFaceTextureSynthesizer Synthesizer;
		if (!Template || !Template->Parent || !Plugin || !Synthesizer.Init(Plugin->GetContentDir() / FacesSynthesisModel, 6))
		{
			UE_LOG(LogPawnFaces, Error, TEXT("Face skins need Kellan's skin instance (run PawnLooks first) and the plugin's texture synthesis model"));
			return 1;
		}
		UE_LOG(LogPawnFaces, Display, TEXT("Texture synthesis: %dx%d, %d skin textures, template %s"), Synthesizer.GetTextureSizeX(),
			Synthesizer.GetTextureSizeY(), Synthesizer.GetMaxHighFrequencyIndex(), *Template->GetName());
		const FMetaHumanFaceTextureAttributeMap& Attributes = Synthesizer.GetFaceTextureAttributeMap();

		const TSharedPtr<FJsonObject>* Skins = nullptr;
		Config->TryGetObjectField(TEXT("skins"), Skins);
		int32 Failures = 0;
		for (const bool bFemale : {true, false})
		{
			const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
			if (!Skins || !(*Skins)->TryGetArrayField(bFemale ? TEXT("female") : TEXT("male"), Names))
			{
				continue;
			}
			int32 Index = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Names)
			{
				const FString Name = Value->AsString();
				const UMetaHumanCharacter* Preset = LoadObject<UMetaHumanCharacter>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), FacesPresetFolder, *Name, *Name));
				if (!Preset)
				{
					UE_LOG(LogPawnFaces, Warning, TEXT("Preset %s did not load; no skin from it"), *Name);
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
					UE_LOG(LogPawnFaces, Warning, TEXT("Preset %s: texture synthesis failed"), *Name);
					++Failures;
					continue;
				}
				FString Traits;
				for (int32 A = 0; A < Attributes.NumAttributes(); ++A)
				{
					const TArray<int32>& Values = Attributes.GetAttributeValues(A);
					const TArray<FString>& ValueNames = Attributes.GetAttributeValueNames(A);
					const int32 V = Values.IsValidIndex(Params.HighFrequencyIndex) ? Values[Params.HighFrequencyIndex] : -1;
					Traits += FString::Printf(TEXT(", %s %s"), *Attributes.GetAttributeName(A), ValueNames.IsValidIndex(V) ? *ValueNames[V] : TEXT("?"));
				}
				const FString Set = FString::Printf(TEXT("%s%d"), bFemale ? TEXT("F") : TEXT("M"), Index);
				UTexture2D* ColorTexture = FacesSaveImage(FString::Printf(TEXT("T_FaceSkin_%s_BaseColor"), *Set), Color, true);
				UTexture2D* NormalTexture = FacesSaveImage(FString::Printf(TEXT("T_FaceSkin_%s_Normal"), *Set), Normal, false);
				auto* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
				Factory->InitialParent = Template->Parent;
				auto* Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(PawnFacesSkinName(bFemale, Index), PawnFacesFolder,
					UMaterialInstanceConstant::StaticClass(), Factory));
				if (!ColorTexture || !NormalTexture || !Instance)
				{
					++Failures;
					continue;
				}
				const FLinearColor Average = FacesAverageColor(Color);
				Instance->CopyMaterialUniformParametersEditorOnly(Template, true);
				Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamBaseColor), ColorTexture);
				Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamNormal), NormalTexture);
				Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(PawnLooksParamSourceAverage), Average);
				Instance->PostEditChange();
				Failures += FacesSave(Instance) ? 0 : 1;
				UE_LOG(LogPawnFaces, Display, TEXT("Face skin %s from %s: texture %d, tone (%.2f, %.2f), average %s%s"), *Set, *Name, Params.HighFrequencyIndex,
					Skin.U, Skin.V, *Average.ToString(), *Traits);
				++Index;
			}
		}
		return Failures;
	}

	// The skin under the clothes (U16's bare layer) on a skin material lit like the face and hands (subsurface, their
	// roughness and specular) in the pawn's skin colour, with a fine tiled skin normal: its UVs are the hoodie's, so no
	// skin texture fits it. The pawn sets SkinColor on a dynamic instance.
	int32 FacesMakeBareSkin(IAssetTools& Tools)
	{
		const UMaterial* Skin = LoadObject<UMaterial>(nullptr, *PawnLooksAssetPath(PawnLooksSkinMaterial));
		UTexture* Micro = LoadObject<UTexture>(nullptr, FacesMicroNormal);
		UMaterial* Material = Skin && Micro ? Cast<UMaterial>(Tools.CreateAsset(PawnFacesBareSkinMaterial, PawnFacesFolder, UMaterial::StaticClass(),
			NewObject<UMaterialFactoryNew>())) : nullptr;
		if (!Material)
		{
			UE_LOG(LogPawnFaces, Error, TEXT("The bare skin material needs M_PawnSkin (run PawnLooks first) and %s"), FacesMicroNormal);
			return 1;
		}
		Material->SetShadingModel(MSM_SubsurfaceProfile);
		Material->SubsurfaceProfile = Skin->SubsurfaceProfile;
		Material->TwoSided = true;  // the cut edges at the neck, wrists and ankles show the inside
		bool bRecompile = false;
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_SkeletalMesh, bRecompile);
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_MorphTargets, bRecompile);

		auto* Color = FacesNode<UMaterialExpressionVectorParameter>(Material, -500, -100);
		Color->ParameterName = PawnLooksParamSkinColor;
		Color->DefaultValue = FLinearColor(0.15f, 0.08f, 0.06f);
		UMaterialEditingLibrary::ConnectMaterialProperty(Color, TEXT("RGB"), MP_BaseColor);

		auto* Uv = FacesNode<UMaterialExpressionTextureCoordinate>(Material, -900, 200);
		Uv->UTiling = Uv->VTiling = 40.0f;
		auto* Normal = FacesNode<UMaterialExpressionTextureSampleParameter2D>(Material, -700, 200);
		Normal->ParameterName = PawnLooksParamNormal;
		Normal->Texture = Micro;
		Normal->SamplerType = SAMPLERTYPE_Normal;
		FacesConnect(Uv, TEXT(""), Normal, TEXT("UVs"));
		auto* Flatten = FacesNode<UMaterialExpressionMultiply>(Material, -400, 200);
		Flatten->ConstB = 1.0f;
		auto* Strength = FacesNode<UMaterialExpressionConstant3Vector>(Material, -600, 350);
		Strength->Constant = FLinearColor(0.3f, 0.3f, 1.0f);
		FacesConnect(Normal, TEXT("RGB"), Flatten, TEXT("A"));
		FacesConnect(Strength, TEXT(""), Flatten, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Flatten, TEXT(""), MP_Normal);

		// Roughness and specular as the face and hands have them (M_PawnSkin's defaults).
		const auto Scalar = [&](const TCHAR* Name, float Default, int32 Y, EMaterialProperty Property)
		{
			Skin->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(Name), Default);
			auto* Parameter = FacesNode<UMaterialExpressionScalarParameter>(Material, -400, Y);
			Parameter->ParameterName = Name;
			Parameter->DefaultValue = Default;
			UMaterialEditingLibrary::ConnectMaterialProperty(Parameter, TEXT(""), Property);
		};
		Scalar(TEXT("Roughness"), 0.55f, 450, MP_Roughness);
		Scalar(TEXT("Specular"), 0.35f, 550, MP_Specular);
		Material->PostEditChange();
		return FacesSave(Material) ? 0 : 1;
	}

	int32 FacesGenerate()
	{
		IAssetRegistry::GetChecked().SearchAllAssets(true);
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		FString Text;
		TSharedPtr<FJsonObject> Config;
		const FString ConfigPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../assets/pawn_faces.json"));
		if (!FFileHelper::LoadFileToString(Text, *ConfigPath) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Config) || !Config)
		{
			UE_LOG(LogPawnFaces, Error, TEXT("Could not read %s"), *ConfigPath);
			return 1;
		}

		// Start clean: the generated folder holds only what this run makes.
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(PawnFacesFolder), false, true);

		USkeletalMesh* Face = nullptr;
		FTransform ModelToKellan;
		if (!FacesMakeFace(Tools, Config, Face, ModelToKellan))
		{
			return 1;
		}
		int32 Failures = FacesMakeEyes(Tools);
		Failures += FacesMakeGrooms(Tools, Config, Face, ModelToKellan);
		Failures += FacesMakeMarks(Tools);
		Failures += FacesMakeSkins(Tools, Config);
		Failures += FacesMakeBareSkin(Tools);
		UE_LOG(LogPawnFaces, Display, TEXT("Pawn faces: done, %d problems"), Failures);
		return Failures == 0 ? 0 : 1;
	}
}

UPawnFacesCommandlet::UPawnFacesCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UPawnFacesCommandlet::Main(const FString& Params)
{
	return Params.Contains(TEXT("-Probe")) ? FacesProbe() : FacesGenerate();
}
