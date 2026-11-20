#pragma once

#include "CoreMinimal.h"

/** Names shared by UWetnessSubsystem and the WetLook commandlet (docs/design/wetness.md). */
namespace WetnessShared
{
	inline const TCHAR* const Folder = TEXT("/Game/Rim3D/Generated/Weather");
	inline const TCHAR* const CollectionName = TEXT("MPC_Weather");
	inline const TCHAR* const CoverName = TEXT("T_WeatherCover");
	inline const TCHAR* const CollectionPath = TEXT("/Game/Rim3D/Generated/Weather/MPC_Weather.MPC_Weather");
	inline const TCHAR* const CoverPath = TEXT("/Game/Rim3D/Generated/Weather/T_WeatherCover.T_WeatherCover");
	inline const FName WetnessParam(TEXT("Wetness"));
	inline const FName CoverMapParam(TEXT("CoverMap"));
	/** Cells along each side of the cover texture: maps up to 1024 m across are covered whole. */
	constexpr int32 CoverSize = 1024;
	/** The cover texel of a cell with open sky over it. */
	constexpr float OpenSky = -1000.0f;
	/** Custom primitive data slot of a worn mesh's wetness (the MakeHuman materials' MhAddWetness uses the same). */
	constexpr int32 WornDataIndex = 12;
}
