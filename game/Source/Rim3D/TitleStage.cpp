// The title screen's starship stage (TitleStage.h, docs/design/new-game.md "Title"). Everything here is cosmetic.

#include "TitleStage.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProceduralMeshComponent.h"
#include "TextureResource.h"

#include "CreatorPeople.h"
#include "MakeHumanArt.h"
#include "PawnActor.h"
#include "PawnAnimInstance.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <string>
#include <vector>

namespace
{
	const TCHAR* const TsPbrPath = TEXT("/Game/Rim3D/Generated/Guns/M_GunPBR.M_GunPBR");
	const TCHAR* const TsPlainPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");
	const TCHAR* const TsGlowPath = TEXT("/Game/Rim3D/Generated/Buildings/M_BuildingGhost.M_BuildingGhost");

	// The camera (local cm): eye height, field of view (horizontal), a slight look down, and its drift.
	constexpr double TsEyeCm = 138.0;
	constexpr float TsFieldOfView = 45.0f;
	constexpr double TsPitch = -1.5;
	constexpr double TsDriftCm = 3.0;

	// The exposure the stage is lit for (EV100, held: no auto exposure) and its lights. With 9 EV a surface of 0.18 grey
	// reads mid-grey at about 600 lux.
	constexpr float TsExposureEV = 9.0f;
	constexpr float TsSunLux = 3500.0f;
	const FVector TsSunTravel(-0.35, -1.0, -0.22);  // from screen right and a little behind her: a rim on her, the planet half lit
	const FLinearColor TsSunColour(1.0f, 0.95f, 0.88f);

	// Her: where she stands (local cm, feet) and which way she faces (degrees; 191 looks straight at the camera).
	const FVector TsPawnSpot(300.0, 58.0, 0.0);
	constexpr float TsPawnYaw = 186.0f;

	// The window (local cm): its plane, the sill top, the header's underside, and the mullions.
	constexpr double TsWindowX = 520.0;
	constexpr double TsSillTop = 24.0;
	constexpr double TsHeaderBottom = 236.0;
	constexpr double TsMullions[] = {-165.0, 205.0, -560.0, 560.0};

	// The planet: radius, how big it looks (angular radius, degrees), where its middle is from the eye (pitch, yaw), its
	// axis tilt and spin. The galaxy sphere round the eye, its radius and turn (the band rises behind her to the top right).
	constexpr double TsPlanetRadius = 200000.0;
	constexpr double TsPlanetAngularRadius = 35.0;
	constexpr double TsPlanetPitch = -39.0;
	constexpr double TsPlanetYaw = -10.0;
	const FRotator TsPlanetTilt(-51.0, -10.0, 20.0);  // its equator towards the camera, the axis leaning
	constexpr float TsPlanetDegreesPerSecond = 0.35f;
	constexpr double TsGalaxyRadius = 900000.0;
	const FRotator TsGalaxyTurn(0.0, -14.0, 0.0);
	constexpr float TsGalaxyGlow = 700.0f;  // the map is linear light: stars up to 1, the band about 0.1

	// Plain colours when a map is missing (linear).
	const FLinearColor TsOceanColour(0.02f, 0.08f, 0.20f);
	const FLinearColor TsNightSky(0.0015f, 0.0018f, 0.003f);

	// The cabin's surfaces (linear colours) and its glowing strips (colour times strength, cd/m2 at full).
	const FLinearColor TsFrameColour(0.035f, 0.040f, 0.046f);
	const FLinearColor TsFloorColour(0.020f, 0.022f, 0.026f);
	const FLinearColor TsCyan(0.25f, 0.85f, 1.0f);
	const FLinearColor TsGold(1.0f, 0.68f, 0.30f);
	constexpr float TsCyanGlow = 1500.0f;
	constexpr float TsGoldGlow = 800.0f;

	// Her clothes (sneed: "glitterworld wear ... rizz up her attire"): a fitted lace-up blouse in pearl satin over tight
	// glossy black jeans (the padded uniform jacket looked bulky in the PC picture, 2026-11-23). The first garment of
	// each list that is imported for her body is worn (a man gets the jacket and trousers).
	const TCHAR* const TsTops[] = {TEXT("Blouse"), TEXT("Tank"), TEXT("UniformJacket"), TEXT("TeeTucked")};
	const TCHAR* const TsBottoms[] = {TEXT("JeansClassic"), TEXT("Trousers"), TEXT("JeansStraight")};
	const FLinearColor TsTopColour(0.86f, 0.82f, 0.74f);
	const FLinearColor TsBottomColour(0.012f, 0.013f, 0.018f);
	constexpr float TsTopRoughness = 0.22f;
	constexpr float TsBottomRoughness = 0.15f;
	constexpr uint32 TsTopItem = 0x7FFE0002u;
	constexpr uint32 TsBottomItem = 0x7FFE0001u;

	/** Adds triangle a b c facing along Want (Unreal draws the face whose corners run clockwise as seen). */
	void TsTriangle(TArray<int32>& Tris, const TArray<FVector>& P, int32 A, int32 B, int32 C, const FVector& Want)
	{
		const FVector Front = FVector::CrossProduct(P[C] - P[A], P[B] - P[A]);
		if (FVector::DotProduct(Front, Want) >= 0.0)
		{
			Tris.Append({A, B, C});
		}
		else
		{
			Tris.Append({A, C, B});
		}
	}

	struct FTsMesh
	{
		TArray<FVector> Positions;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FColor> Colours;

		/** A UV sphere (u round, v from the north pole, as tools/title_art.py paints), facing out or in. */
		void AddSphere(double Radius, int32 Rings, int32 Segments, bool bInside, const FLinearColor& Colour)
		{
			const int32 Base = Positions.Num();
			for (int32 R = 0; R <= Rings; ++R)
			{
				const double Theta = UE_DOUBLE_PI * R / Rings;
				for (int32 S = 0; S <= Segments; ++S)
				{
					const double Phi = 2.0 * UE_DOUBLE_PI * S / Segments;
					const FVector Dir(FMath::Sin(Theta) * FMath::Cos(Phi), FMath::Sin(Theta) * FMath::Sin(Phi), FMath::Cos(Theta));
					Positions.Add(Dir * Radius);
					Normals.Add(bInside ? -Dir : Dir);
					UVs.Add(FVector2D(static_cast<double>(S) / Segments, static_cast<double>(R) / Rings));
					Colours.Add(Colour.ToFColor(true));
				}
			}
			for (int32 R = 0; R < Rings; ++R)
			{
				for (int32 S = 0; S < Segments; ++S)
				{
					const int32 A = Base + R * (Segments + 1) + S;
					const int32 B = A + Segments + 1;
					const FVector Out = (Positions[A] + Positions[B + 1]).GetSafeNormal();
					const FVector Want = bInside ? -Out : Out;
					if (R > 0)
					{
						TsTriangle(Triangles, Positions, A, A + 1, B, Want);
					}
					if (R < Rings - 1)
					{
						TsTriangle(Triangles, Positions, A + 1, B + 1, B, Want);
					}
				}
			}
		}

		/** A box (centre and half sizes, local cm), its faces outwards. */
		void AddBox(const FVector& Centre, const FVector& Half, const FLinearColor& Colour)
		{
			const FVector Axes[3] = {FVector::ForwardVector, FVector::RightVector, FVector::UpVector};
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				for (const double Side : {-1.0, 1.0})
				{
					const FVector N = Axes[Axis] * Side;
					const FVector U = Axes[(Axis + 1) % 3];
					const FVector V = Axes[(Axis + 2) % 3];
					const double Hn = Half[Axis];
					const double Hu = Half[(Axis + 1) % 3];
					const double Hv = Half[(Axis + 2) % 3];
					const int32 Base = Positions.Num();
					const double Corners[4][2] = {{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}};
					for (const auto& Corner : Corners)
					{
						Positions.Add(Centre + N * Hn + U * (Corner[0] * Hu) + V * (Corner[1] * Hv));
						Normals.Add(N);
						UVs.Add(FVector2D(Corner[0] * 0.5 + 0.5, Corner[1] * 0.5 + 0.5));
						Colours.Add(Colour.ToFColor(true));
					}
					TsTriangle(Triangles, Positions, Base, Base + 1, Base + 2, N);
					TsTriangle(Triangles, Positions, Base, Base + 2, Base + 3, N);
				}
			}
		}

		/** A flat ring in the local YZ plane facing +X, from radius Inner to Outer, angles A0 to A1 (radians). */
		void AddRingPiece(double Inner, double Outer, double A0, double A1, int32 Steps)
		{
			const int32 Base = Positions.Num();
			for (int32 I = 0; I <= Steps; ++I)
			{
				const double A = FMath::Lerp(A0, A1, static_cast<double>(I) / Steps);
				const FVector Radial(0.0, FMath::Cos(A), FMath::Sin(A));
				for (const double R : {Inner, Outer})
				{
					Positions.Add(Radial * R);
					Normals.Add(FVector::ForwardVector);
					UVs.Add(FVector2D(static_cast<double>(I) / Steps, R == Inner ? 0.0 : 1.0));
					Colours.Add(FColor::White);
				}
			}
			for (int32 I = 0; I < Steps; ++I)
			{
				const int32 A = Base + I * 2;
				TsTriangle(Triangles, Positions, A, A + 1, A + 2, FVector::ForwardVector);
				TsTriangle(Triangles, Positions, A + 1, A + 3, A + 2, FVector::ForwardVector);
			}
		}

		void Upload(UProceduralMeshComponent& Mesh, int32 Section) const
		{
			Mesh.CreateMeshSection(Section, Positions, Triangles, Normals, UVs, Colours, TArray<FProcMeshTangent>(), false);
		}
	};

	/** Reads a PNG as BGRA8; false if it is missing or unreadable. */
	bool TsLoadPng(const FString& Path, TArray64<uint8>& OutBgra, int32& OutWidth, int32& OutHeight)
	{
		TArray<uint8> File;
		if (!FPaths::FileExists(Path) || !FFileHelper::LoadFileToArray(File, *Path))
		{
			return false;
		}
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(File.GetData(), File.Num()) || !Wrapper->GetRaw(ERGBFormat::BGRA, 8, OutBgra))
		{
			UE_LOG(LogTemp, Warning, TEXT("Title stage: %s is not a readable PNG"), *Path);
			return false;
		}
		OutWidth = static_cast<int32>(Wrapper->GetWidth());
		OutHeight = static_cast<int32>(Wrapper->GetHeight());
		return OutWidth > 0 && OutHeight > 0 && OutBgra.Num() == static_cast<int64>(OutWidth) * OutHeight * 4;
	}

	/** A transient sRGB texture of BGRA8 pixels. */
	UTexture2D* TsMakeTexture(const uint8* Bgra, int32 Width, int32 Height)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
		if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
		{
			return nullptr;
		}
		Texture->SRGB = true;
		Texture->Filter = TF_Bilinear;
		Texture->AddressX = TA_Wrap;
		Texture->AddressY = TA_Clamp;
		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(Data, Bgra, static_cast<SIZE_T>(Width) * Height * 4);
		Mip.BulkData.Unlock();
		Texture->UpdateResource();
		return Texture;
	}

	bool TsInSmokeTest()
	{
		return FString(FCommandLine::Get()).Contains(TEXT("-SmokeTest="));
	}

	FString TsCharacterFile()
	{
		return TitleStage::ArtFolder() / TEXT("Character.txt");
	}
}

// ---- Remembering the character ----------------------------------------------------------------------

FString TitleStage::ArtFolder()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("TitleArt"));
}

FString TitleStage::FindCharacterText(const colony::Sim& Sim)
{
	std::string Protagonist;
	std::string Anyone;
	Sim.things().registry().view<const colony::Colonist, const colony::PawnLooks>().each(
		[&Protagonist, &Anyone](colony::Entity, const colony::Colonist& Colonist, const colony::PawnLooks& Looks)
		{
			if (Looks.creator.empty())
			{
				return;
			}
			if (Colonist.protagonist && Protagonist.empty())
			{
				Protagonist = Looks.creator;
			}
			if (Anyone.empty())
			{
				Anyone = Looks.creator;
			}
		});
	const std::string& Found = Protagonist.empty() ? Anyone : Protagonist;
	return Found.empty() ? FString() : FString(UTF8_TO_TCHAR(Found.c_str()));
}

void TitleStage::RememberCharacterText(const FString& CreatorText)
{
	CreatorPeople::FLook Look;
	if (TsInSmokeTest() || !CreatorPeople::Decode(CreatorText, Look))
	{
		return;
	}
	FString Before;
	if (FFileHelper::LoadFileToString(Before, *TsCharacterFile()) && Before.TrimStartAndEnd() == CreatorText)
	{
		return;
	}
	IFileManager::Get().MakeDirectory(*ArtFolder(), true);
	FFileHelper::SaveStringToFile(CreatorText, *TsCharacterFile());
}

void TitleStage::RememberCharacter(const colony::Sim& Sim)
{
	const FString Text = FindCharacterText(Sim);
	if (!Text.IsEmpty())
	{
		RememberCharacterText(Text);
	}
}

// ---- The stage -------------------------------------------------------------------------------------

ATitleStage::ATitleStage()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
}

UTexture2D* ATitleStage::SolidTexture(const FLinearColor& Colour)
{
	const FColor Pixel = Colour.ToFColor(true);
	const uint8 Bgra[4] = {Pixel.B, Pixel.G, Pixel.R, 255};
	UTexture2D* Texture = TsMakeTexture(Bgra, 1, 1);
	if (Texture)
	{
		Textures.Add(Texture);
	}
	return Texture;
}

UMaterialInterface* ATitleStage::MakeSurface(UMaterialInterface* Pbr, UMaterialInterface* Plain, const FLinearColor& Colour, float Roughness,
	const FLinearColor& Glow, float GlowStrength, UTexture2D* ColourMap, UTexture2D* GlowMap)
{
	if (!Pbr)
	{
		return Plain;
	}
	UMaterialInstanceDynamic* Surface = UMaterialInstanceDynamic::Create(Pbr, this);
	if (!Surface)
	{
		return Plain;
	}
	if (UTexture2D* Base = ColourMap ? ColourMap : SolidTexture(Colour))
	{
		Surface->SetTextureParameterValue(TEXT("BaseColorMap"), Base);
	}
	Surface->SetScalarParameterValue(TEXT("RoughnessScale"), Roughness);
	if (GlowStrength > 0.0f)
	{
		if (UTexture2D* Light = GlowMap ? GlowMap : SolidTexture(Glow))
		{
			Surface->SetTextureParameterValue(TEXT("EmissiveMap"), Light);
			Surface->SetScalarParameterValue(TEXT("EmissiveStrength"), GlowStrength);
		}
	}
	Materials.Add(Surface);
	return Surface;
}

void ATitleStage::Build()
{
	UMaterialInterface* Pbr = LoadObject<UMaterialInterface>(nullptr, TsPbrPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	UMaterialInterface* Plain = LoadObject<UMaterialInterface>(nullptr, TsPlainPath);
	UMaterialInterface* Glow = LoadObject<UMaterialInterface>(nullptr, TsGlowPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Pbr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Title stage: %s is missing (GunArt commandlet); plain colours"), TsPbrPath);
	}
	BuildCabin(Pbr, Plain);
	BuildPlanet(Pbr, Plain, Glow);
	BuildGalaxy(Pbr, Plain);
	BuildLights();
	PickLook();
	UE_LOG(LogTemp, Display, TEXT("Title stage built: planet map %d, galaxy map %d, look from %s"), bPlanetMap ? 1 : 0, bGalaxyMap ? 1 : 0, *LookSource);
}

void ATitleStage::BuildCabin(UMaterialInterface* Pbr, UMaterialInterface* Plain)
{
	Cabin = NewObject<UProceduralMeshComponent>(this, TEXT("Cabin"));
	Cabin->bUseAsyncCooking = false;
	Cabin->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Cabin->SetCanEverAffectNavigation(false);
	Cabin->SetupAttachment(Root);

	// Section 0: the frame (sill, header, mullions); 1: the floor; 2: cyan strips; 3: gold strips.
	FTsMesh Frame;
	Frame.AddBox(FVector(TsWindowX + 10.0, 0.0, TsSillTop * 0.5), FVector(30.0, 900.0, TsSillTop * 0.5), TsFrameColour);
	Frame.AddBox(FVector(TsWindowX + 10.0, 0.0, TsHeaderBottom + 60.0), FVector(30.0, 900.0, 60.0), TsFrameColour);
	for (const double Y : TsMullions)
	{
		Frame.AddBox(FVector(TsWindowX + 4.0, Y, (TsSillTop + TsHeaderBottom) * 0.5), FVector(12.0, 7.0, (TsHeaderBottom - TsSillTop) * 0.5), TsFrameColour);
	}
	Frame.Upload(*Cabin, 0);
	FTsMesh Floor;
	Floor.AddBox(FVector(200.0, 0.0, -2.0), FVector(360.0, 900.0, 2.0), TsFloorColour);
	Floor.Upload(*Cabin, 1);
	FTsMesh Cyan;
	Cyan.AddBox(FVector(TsWindowX - 20.5, 0.0, TsSillTop - 1.0), FVector(1.0, 900.0, 0.8), TsCyan);
	for (const double Y : TsMullions)
	{
		Cyan.AddBox(FVector(TsWindowX - 8.5, Y, (TsSillTop + TsHeaderBottom) * 0.5), FVector(0.6, 1.2, (TsHeaderBottom - TsSillTop) * 0.5 - 6.0), TsCyan);
	}
	Cyan.Upload(*Cabin, 2);
	FTsMesh Gold;
	Gold.AddBox(FVector(TsWindowX - 20.5, 0.0, TsHeaderBottom + 1.0), FVector(1.0, 900.0, 0.6), TsGold);
	Gold.Upload(*Cabin, 3);

	Cabin->SetMaterial(0, MakeSurface(Pbr, Plain, TsFrameColour, 0.32f));
	Cabin->SetMaterial(1, MakeSurface(Pbr, Plain, TsFloorColour, 0.18f));
	Cabin->SetMaterial(2, MakeSurface(Pbr, Plain, FLinearColor(0.01f, 0.02f, 0.025f), 0.4f, TsCyan, TsCyanGlow));
	Cabin->SetMaterial(3, MakeSurface(Pbr, Plain, FLinearColor(0.02f, 0.015f, 0.01f), 0.4f, TsGold, TsGoldGlow));
	Cabin->RegisterComponent();
}

void ATitleStage::BuildPlanet(UMaterialInterface* Pbr, UMaterialInterface* Plain, UMaterialInterface* Glow)
{
	const FVector Eye(0.0, 0.0, TsEyeCm);
	const FVector Toward = FRotator(TsPlanetPitch, TsPlanetYaw, 0.0).Vector();
	const double Distance = TsPlanetRadius / FMath::Sin(FMath::DegreesToRadians(TsPlanetAngularRadius));
	const FVector Centre = Eye + Toward * Distance;

	PlanetAxis = NewObject<USceneComponent>(this, TEXT("PlanetAxis"));
	PlanetAxis->SetupAttachment(Root);
	PlanetAxis->SetRelativeLocationAndRotation(Centre, TsPlanetTilt);
	PlanetAxis->RegisterComponent();

	// The surface with the clouds laid over it (both maps the same size); without the maps, a plain blue world.
	UTexture2D* Map = nullptr;
	TArray64<uint8> Surface;
	TArray64<uint8> Clouds;
	int32 W = 0;
	int32 H = 0;
	int32 CloudW = 0;
	int32 CloudH = 0;
	const FString Folder = TitleStage::ArtFolder();
	if (TsLoadPng(Folder / TEXT("planet_color.png"), Surface, W, H))
	{
		if (TsLoadPng(Folder / TEXT("planet_clouds.png"), Clouds, CloudW, CloudH) && CloudW == W && CloudH == H)
		{
			// The cloud map is grey cover; clouds are a soft white (sRGB 236).
			for (int64 I = 0; I + 3 < Surface.Num(); I += 4)
			{
				const int32 Cover = Clouds[I + 1];
				for (int32 C = 0; C < 3; ++C)
				{
					Surface[I + C] = static_cast<uint8>((Surface[I + C] * (255 - Cover) + 236 * Cover) / 255);
				}
			}
		}
		Map = TsMakeTexture(Surface.GetData(), W, H);
		if (Map)
		{
			Textures.Add(Map);
			bPlanetMap = true;
		}
	}

	Planet = NewObject<UProceduralMeshComponent>(this, TEXT("Planet"));
	Planet->bUseAsyncCooking = false;
	Planet->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Planet->SetCanEverAffectNavigation(false);
	Planet->SetCastShadow(false);
	Planet->SetupAttachment(PlanetAxis);
	FTsMesh Sphere;
	Sphere.AddSphere(TsPlanetRadius, 128, 256, false, TsOceanColour);
	Sphere.Upload(*Planet, 0);
	Planet->SetMaterial(0, MakeSurface(Pbr, Plain, TsOceanColour, 0.75f, FLinearColor::Black, 0.0f, Map));
	Planet->RegisterComponent();

	// The air: a thin glow round the limb, on rings facing the camera just in front of the planet, brightest where the
	// sun is behind the limb; a little over the planet's edge (haze), fading out into space.
	if (!Glow)
	{
		return;
	}
	const double RingDistance = Distance - TsPlanetRadius * 1.02;
	const double Limb = RingDistance * FMath::Tan(FMath::DegreesToRadians(TsPlanetAngularRadius));
	AirGlow = NewObject<UProceduralMeshComponent>(this, TEXT("AirGlow"));
	AirGlow->bUseAsyncCooking = false;
	AirGlow->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	AirGlow->SetCanEverAffectNavigation(false);
	AirGlow->SetCastShadow(false);
	AirGlow->SetupAttachment(Root);
	const FVector RingCentre = Eye + Toward * RingDistance;
	const FRotator RingFacing = (Eye - RingCentre).Rotation();
	AirGlow->SetRelativeLocationAndRotation(RingCentre, RingFacing);
	const double Edges[] = {0.90, 0.95, 0.975, 0.99, 0.998, 1.004, 1.011, 1.02, 1.032, 1.05};
	constexpr int32 Bands = static_cast<int32>(UE_ARRAY_COUNT(Edges)) - 1;
	constexpr int32 Sectors = 16;
	const FVector ToSun = -TsSunTravel.GetSafeNormal();
	const FRotationMatrix Facing(RingFacing);
	int32 Section = 0;
	for (int32 Band = 0; Band < Bands; ++Band)
	{
		const double Middle = (Edges[Band] + Edges[Band + 1]) * 0.5;
		const float Opacity = Middle < 1.0
			? 0.6f * static_cast<float>(FMath::Pow((Middle - 0.9) / 0.1, 2.2))
			: 0.6f * static_cast<float>(FMath::Exp(-(Middle - 1.0) / 0.016));
		const FLinearColor Tone = Middle < 1.0 ? FLinearColor(0.45f, 0.70f, 1.0f) : FLinearColor(0.28f, 0.52f, 1.0f);
		for (int32 Sector = 0; Sector < Sectors; ++Sector)
		{
			const double A0 = 2.0 * UE_DOUBLE_PI * Sector / Sectors;
			const double A1 = 2.0 * UE_DOUBLE_PI * (Sector + 1) / Sectors;
			const double Mid = (A0 + A1) * 0.5;
			const FVector Radial = Facing.TransformVector(FVector(0.0, FMath::Cos(Mid), FMath::Sin(Mid)));
			const float Lit = FMath::Clamp(static_cast<float>(FVector::DotProduct(Radial, ToSun)) * 0.9f + 0.3f, 0.06f, 1.0f);
			FTsMesh Piece;
			Piece.AddRingPiece(Limb * Edges[Band], Limb * Edges[Band + 1], A0, A1, 8);
			Piece.Upload(*AirGlow, Section);
			if (UMaterialInstanceDynamic* Air = UMaterialInstanceDynamic::Create(Glow, this))
			{
				Air->SetVectorParameterValue(TEXT("Color"), Tone * Lit);
				Air->SetScalarParameterValue(TEXT("Opacity"), Opacity * FMath::Sqrt(Lit));
				AirGlow->SetMaterial(Section, Air);
				Materials.Add(Air);
			}
			++Section;
		}
	}
	AirGlow->RegisterComponent();
}

void ATitleStage::BuildGalaxy(UMaterialInterface* Pbr, UMaterialInterface* Plain)
{
	UTexture2D* Map = nullptr;
	TArray64<uint8> Pixels;
	int32 W = 0;
	int32 H = 0;
	if (TsLoadPng(TitleStage::ArtFolder() / TEXT("galaxy.png"), Pixels, W, H))
	{
		Map = TsMakeTexture(Pixels.GetData(), W, H);
		if (Map)
		{
			Textures.Add(Map);
			bGalaxyMap = true;
		}
	}
	Galaxy = NewObject<UProceduralMeshComponent>(this, TEXT("Galaxy"));
	Galaxy->bUseAsyncCooking = false;
	Galaxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Galaxy->SetCanEverAffectNavigation(false);
	Galaxy->SetCastShadow(false);
	Galaxy->bAffectDynamicIndirectLighting = false;
	// Only on the third light channel: the sun and the lamps (channel 0) leave the sky black, only its glow shows.
	Galaxy->SetLightingChannels(false, false, true);
	Galaxy->SetupAttachment(Root);
	Galaxy->SetRelativeLocationAndRotation(FVector(0.0, 0.0, TsEyeCm), TsGalaxyTurn);
	FTsMesh Sphere;
	Sphere.AddSphere(TsGalaxyRadius, 64, 128, true, TsNightSky);
	Sphere.Upload(*Galaxy, 0);
	Galaxy->SetMaterial(0, MakeSurface(Pbr, Plain, FLinearColor::Black, 1.0f, Map ? FLinearColor::White : TsNightSky, Map ? TsGalaxyGlow : 300.0f,
		nullptr, Map));
	Galaxy->RegisterComponent();
}

void ATitleStage::BuildLights()
{
	Sun = NewObject<UDirectionalLightComponent>(this, TEXT("Sun"));
	Sun->SetMobility(EComponentMobility::Movable);
	Sun->SetIntensity(TsSunLux);
	Sun->SetLightColor(TsSunColour);
	Sun->SetCastShadows(true);
	Sun->SetAtmosphereSunLight(false);
	Sun->SetDynamicShadowDistanceMovableLight(3000.0f);
	Sun->SetupAttachment(Root);
	Sun->RegisterComponent();
	Sun->SetRelativeRotation(TsSunTravel.Rotation());

	// Lamps (local cm, candelas, colour, reach): a soft key up and left of the camera for her face, a cool glow from
	// the sill behind her, a warm light over her head for her hair.
	struct FTsLamp
	{
		FVector At;
		float Candelas;
		FLinearColor Colour;
		float Reach;
		float Soft;
	};
	const FTsLamp LampSpecs[] = {
		{FVector(70.0, -120.0, 225.0), 9000.0f, FLinearColor(0.92f, 0.95f, 1.0f), 900.0f, 60.0f},
		{FVector(470.0, 40.0, 60.0), 2500.0f, TsCyan, 500.0f, 30.0f},
		{FVector(330.0, 50.0, 330.0), 1500.0f, FLinearColor(1.0f, 0.86f, 0.7f), 400.0f, 40.0f},
	};
	for (const FTsLamp& Spec : LampSpecs)
	{
		UPointLightComponent* Lamp = NewObject<UPointLightComponent>(this);
		Lamp->SetMobility(EComponentMobility::Movable);
		Lamp->SetIntensityUnits(ELightUnits::Candelas);
		Lamp->SetIntensity(Spec.Candelas);
		Lamp->SetLightColor(Spec.Colour);
		Lamp->SetAttenuationRadius(Spec.Reach);
		Lamp->SetSourceRadius(Spec.Soft);
		Lamp->SetSoftSourceRadius(Spec.Soft * 2.0f);
		Lamp->SetCastShadows(false);
		Lamp->SetupAttachment(Root);
		Lamp->SetRelativeLocation(Spec.At);
		Lamp->RegisterComponent();
		Lamps.Add(Lamp);
	}

	// A held exposure (the sky's auto exposure is off while the stage stands), soft bloom and vignette.
	Exposure = NewObject<UPostProcessComponent>(this, TEXT("Exposure"));
	Exposure->bUnbound = true;
	Exposure->Priority = 50.0f;
	FPostProcessSettings& P = Exposure->Settings;
	P.bOverride_AutoExposureMethod = true;
	P.AutoExposureMethod = AEM_Histogram;
	P.bOverride_AutoExposureBias = true;
	P.AutoExposureBias = 0.0f;
	P.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	P.AutoExposureApplyPhysicalCameraExposure = false;
	P.bOverride_AutoExposureMinBrightness = true;
	P.AutoExposureMinBrightness = TsExposureEV;
	P.bOverride_AutoExposureMaxBrightness = true;
	P.AutoExposureMaxBrightness = TsExposureEV;
	P.bOverride_AutoExposureSpeedUp = true;
	P.AutoExposureSpeedUp = 20.0f;
	P.bOverride_AutoExposureSpeedDown = true;
	P.AutoExposureSpeedDown = 20.0f;
	P.bOverride_BloomIntensity = true;
	P.BloomIntensity = 0.6f;
	P.bOverride_VignetteIntensity = true;
	P.VignetteIntensity = 0.35f;
	P.bOverride_LensFlareIntensity = true;
	P.LensFlareIntensity = 0.0f;
	P.bOverride_FilmGrainIntensity = true;
	P.FilmGrainIntensity = 0.0f;
	Exposure->SetupAttachment(Root);
	Exposure->RegisterComponent();
}

void ATitleStage::PickLook()
{
	// 1. The character of the last game saved or loaded (TitleStage::RememberCharacter).
	CreatorPeople::FLook Look;
	FString Text;
	if (FFileHelper::LoadFileToString(Text, *TsCharacterFile()) && CreatorPeople::Decode(Text.TrimStartAndEnd(), Look))
	{
		LookText = Text.TrimStartAndEnd();
		LookSource = TEXT("remembered");
	}
	// 2. Before anything was remembered: the protagonist of the newest save (read once, then remembered).
	const USimSubsystem* Bridge = GetWorld() ? GetWorld()->GetSubsystem<USimSubsystem>() : nullptr;
	if (LookText.IsEmpty() && Bridge && Bridge->GetDefs())
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(USimSubsystem::GetSaveDirectory() / TEXT("*.save")), true, false);
		FString Newest;
		FDateTime NewestTime = FDateTime::MinValue();
		for (const FString& File : Files)
		{
			const FDateTime Time = IFileManager::Get().GetTimeStamp(*(USimSubsystem::GetSaveDirectory() / File));
			if (Time > NewestTime)
			{
				NewestTime = Time;
				Newest = File;
			}
		}
		std::vector<uint8_t> Bytes;
		std::string Error;
		if (!Newest.IsEmpty() && colony::readSaveFile(TCHAR_TO_UTF8(*(USimSubsystem::GetSaveDirectory() / Newest)), Bytes, Error))
		{
			const colony::SaveLoadResult Loaded = colony::loadSim(Bytes, Bridge->GetDefs());
			const FString Found = Loaded.sim ? TitleStage::FindCharacterText(*Loaded.sim) : FString();
			if (!Found.IsEmpty() && CreatorPeople::Decode(Found, Look))
			{
				LookText = Found;
				LookSource = TEXT("save");
				TitleStage::RememberCharacterText(Found);
			}
		}
	}
	// 3. The creator's own woman with long straight hair.
	if (LookText.IsEmpty())
	{
		Look = CreatorPeople::FLook();
		Look.Seed = CreatorPeople::kDefaultSeed;
		Look.bFemale = true;
		Look.Values.Add(TEXT("hairStyle"), 1.0f);  // Long (CreatorHairStyles)
		LookText = CreatorPeople::Encode(Look);
		LookSource = TEXT("default");
	}
	bLookFemale = Look.bFemale;
}

bool ATitleStage::HasPlanet() const
{
	return Planet.Get() != nullptr;
}

bool ATitleStage::HasGalaxy() const
{
	return Galaxy.Get() != nullptr;
}

bool ATitleStage::HasBody() const
{
	return DisplayPawn && DisplayPawn->GetMakeHumanMesh() && DisplayPawn->GetMakeHumanMesh()->GetSkeletalMeshAsset();
}

FTransform ATitleStage::GetCameraTransform() const
{
	// A slow drift of a few centimetres and a fraction of a degree, so the picture breathes.
	const double T = Age;
	const FVector Drift(FMath::Sin(T * 0.21) * TsDriftCm * 0.6, FMath::Sin(T * 0.13 + 1.0) * TsDriftCm, FMath::Sin(T * 0.17 + 2.0) * TsDriftCm * 0.6);
	const FRotator Look(TsPitch + 0.15 * FMath::Sin(T * 0.11), 0.25 * FMath::Sin(T * 0.09 + 0.5), 0.0);
	const FTransform Local(Look, FVector(0.0, 0.0, TsEyeCm) + Drift);
	return Local * GetActorTransform();
}

float ATitleStage::GetFieldOfView() const
{
	return TsFieldOfView;
}

void ATitleStage::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Delta = FMath::Clamp(DeltaSeconds, 0.0f, 0.1f);
	Age += Delta;
	if (Planet)
	{
		Planet->AddLocalRotation(FRotator(0.0, TsPlanetDegreesPerSecond * Delta, 0.0));
	}
	UpdatePawn();
}

void ATitleStage::UpdatePawn()
{
	UWorld* World = GetWorld();
	USimPawnSubsystem* Pawns = World ? World->GetSubsystem<USimPawnSubsystem>() : nullptr;
	if (!Pawns || !Pawns->AreAssetsLoaded())
	{
		return;
	}
	const FVector Spot = GetActorTransform().TransformPosition(TsPawnSpot);
	const float Yaw = static_cast<float>(GetActorRotation().Yaw) + TsPawnYaw;
	if (!DisplayPawn)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;
		DisplayPawn = World->SpawnActor<ASimPawnActor>(Spot, FRotator(0.0, Yaw, 0.0), Params);
		if (!DisplayPawn)
		{
			return;
		}
		DisplayPawn->Setup(*Pawns, colony::kNoEntity, 400.0f, false);
		DisplayPawn->SetDetail(ESimPawnDetail::Full, 0.0f);
		CreatorPeople::FLook Look;
		CreatorPeople::Decode(LookText, Look);
		FSimPawnLooks Base;
		Base.bFemale = Look.bFemale;
		DisplayPawn->ApplyLooks(*Pawns, CreatorPeople::LooksFor(Look, Base));
		bPawnFirstFrame = true;
	}
	DisplayPawn->SetActorLocationAndRotation(Spot, FRotator(0.0, Yaw, 0.0));
	DisplayPawn->ApplyCreatorLook(LookText);
	if (!bDressed && DisplayPawn->GetMakeHumanMesh())
	{
		Dress(*Pawns);
	}
	DisplayPawn->SyncGarmentMorphs();
	// Standing at ease: the idle the animation picks when a body stands still (it breathes and shifts its weight).
	if (USimPawnAnimInstance* Anim = DisplayPawn->GetAnim())
	{
		FSimPawnAnimInputs& In = Anim->Inputs;
		In.Location = Spot;
		In.Yaw = Yaw;
		In.Velocity = FVector::ZeroVector;
		In.CruiseSpeed = 400.0f;
		In.Detail = ESimPawnDetail::Full;
		In.bTeleported = bPawnFirstFrame;
		In.bHoldFacing = false;
		In.bStandTwist = true;
		In.bTurnFeet = false;
		In.bLegsFromClip = false;
		In.bHitLayer = false;
		In.Route.Reset();
		bPawnFirstFrame = false;
	}
}

void ATitleStage::Dress(USimPawnSubsystem& Pawns)
{
	const FString Base = DisplayPawn->GetLooks().MakeHuman;
	const auto Pick = [&Pawns, &Base](TArrayView<const TCHAR* const> Names) -> FString
	{
		for (const TCHAR* Name : Names)
		{
			if (Pawns.GetGarmentMesh(Base, Name))
			{
				return Name;
			}
		}
		return FString();
	};
	const FString Top = Pick(MakeArrayView(TsTops));
	const FString Bottom = Pick(MakeArrayView(TsBottoms));
	// Put on in order, trousers first; made up for the stage (no sim items), so the same garments every time.
	FSimPawnApparel Clothes;
	if (!Bottom.IsEmpty())
	{
		FSimWornPiece& Piece = Clothes.Pieces.AddDefaulted_GetRef();
		Piece.Item = TsBottomItem;
		Piece.Def = 1;
		Piece.Cuts.Add(Bottom);
		Piece.bHasTint = true;
		Piece.Tint = TsBottomColour;
		Piece.bCoversCrotch = true;
	}
	if (!Top.IsEmpty())
	{
		FSimWornPiece& Piece = Clothes.Pieces.AddDefaulted_GetRef();
		Piece.Item = TsTopItem;
		Piece.Def = 2;
		Piece.Cuts.Add(Top);
		Piece.bHasTint = true;
		Piece.Tint = TsTopColour;
		Piece.Puff = 1.5f;  // over the torso, on the skin (FSimWornPiece::Puff)
	}
	DisplayPawn->ApplyApparel(Pawns, Clothes, 0x5449544C45535447ULL);
	// The lumen-world look: one colour each (not the item palette), no wear, and a sheen.
	const TArray<TObjectPtr<UMeshComponent>>& Worn = DisplayPawn->GetWornMeshes();
	const TArray<uint32>& Items = DisplayPawn->GetWornMeshItems();
	for (int32 I = 0; I < Worn.Num(); ++I)
	{
		UMeshComponent* Mesh = Worn[I];
		if (!Mesh)
		{
			continue;
		}
		const bool bTop = Items.IsValidIndex(I) && Items[I] == TsTopItem;
		const FLinearColor Colour = bTop ? TsTopColour : TsBottomColour;
		Mesh->SetCustomPrimitiveDataVector3(MakeHumanGarmentTintIndex, FVector(Colour.R, Colour.G, Colour.B));
		Mesh->SetCustomPrimitiveDataFloat(MakeHumanGarmentWearIndex, 0.0f);
		Mesh->SetCustomPrimitiveDataFloat(MakeHumanGarmentTintAmountIndex, 1.0f);
		for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
		{
			if (UMaterialInstanceDynamic* Sheen = Mesh->CreateDynamicMaterialInstance(Slot))
			{
				Sheen->SetScalarParameterValue(TEXT("Roughness"), bTop ? TsTopRoughness : TsBottomRoughness);
			}
		}
	}
	Outfit = Bottom.IsEmpty() ? Top : (Top.IsEmpty() ? Bottom : Top + TEXT("+") + Bottom);
	bDressed = true;
	UE_LOG(LogTemp, Display, TEXT("Title stage: she wears %s"), Outfit.IsEmpty() ? TEXT("nothing (no garments imported)") : *Outfit);
}

void ATitleStage::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (DisplayPawn)
	{
		DisplayPawn->Destroy();
		DisplayPawn = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}
