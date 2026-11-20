#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectGlobals.h"

/** Names shared by UWetnessSubsystem, UBuildingSubsystem and the WetLook commandlet (docs/design/wetness.md, destruction.md). */
namespace WetnessShared
{
	inline const TCHAR* const Folder = TEXT("/Game/Rim3D/Generated/Weather");
	inline const TCHAR* const CollectionName = TEXT("MPC_Weather");
	inline const TCHAR* const CoverName = TEXT("T_WeatherCover");
	inline const TCHAR* const CollectionPath = TEXT("/Game/Rim3D/Generated/Weather/MPC_Weather.MPC_Weather");
	inline const TCHAR* const CoverPath = TEXT("/Game/Rim3D/Generated/Weather/T_WeatherCover.T_WeatherCover");
	/** Our copy of the engine's VertexColorMaterial (a mesh's own vertex colours, lit), made and made wet by -run=WetLook:
	 * what stuff without a surface material (plain colour walls, floors, wreck pieces) is drawn with. */
	inline const TCHAR* const SolidName = TEXT("M_WetSolid");
	inline const TCHAR* const SolidPath = TEXT("/Game/Rim3D/Generated/Weather/M_WetSolid.M_WetSolid");
	inline const TCHAR* const EngineSolidPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");
	inline const FName WetnessParam(TEXT("Wetness"));
	inline const FName CoverMapParam(TEXT("CoverMap"));
	/** Cells along each side of the cover texture: maps up to 1024 m across are covered whole. */
	constexpr int32 CoverSize = 1024;
	/** The cover texel of a cell with open sky over it. */
	constexpr float OpenSky = -1000.0f;
	/** Custom primitive data slot of a worn mesh's wetness (the MakeHuman materials' MhAddWetness uses the same). */
	constexpr int32 WornDataIndex = 12;
	/** Per-instance custom data slot of a building's damage look (0 whole .. 1 nearly gone; docs/design/destruction.md):
	 * UBuildingSubsystem writes it, the materials patched by -run=WetLook read it (0 for anything else). */
	constexpr int32 DamageDataIndex = 7;

	/** The vertex colour material that gets wet and shows damage (M_WetSolid), or the engine's own before -run=WetLook. */
	inline UMaterialInterface* LoadSolidMaterial()
	{
		if (UMaterialInterface* Ours = LoadObject<UMaterialInterface>(nullptr, SolidPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Ours;
		}
		return LoadObject<UMaterialInterface>(nullptr, EngineSolidPath);
	}
}
