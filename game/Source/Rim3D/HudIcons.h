#pragma once

// Painted HUD icons (orders, zones, Build categories, bottom bar tabs, draft): PNGs in assets/ui/icons/<name>.png, made
// with ChatGPT in one style (docs/design/ui-layout.md, "Icons"). Loaded on first use, with a full mip chain so they stay
// smooth at any HUD scale, and kept for the whole run. A missing file gives null and the caller falls back to text.

#include "CoreMinimal.h"

#include <imgui.h>

class UTexture;
struct ImDrawList;

namespace HudIcons
{
	/** The icon called `Name` (file name without .png), or null when there is no such file or UI.HudIcons is 0. `Pixels`
	 *  is how big it will be drawn: the smallest stored size (256, 128, 64, 32) at least that big is returned. */
	UTexture* Get(const FString& Name, float Pixels = 256.0f);

	/** Draws `Icon` as a square of side `Size` centred on `Centre`; dimmed for something locked. */
	void Draw(ImDrawList* Draw, UTexture* Icon, const ImVec2& Centre, float Size, bool bDim = false);

	/**
	 * A ColonyHud::Button with the icon at its left and the label after it, the two centred together. The label may carry
	 * an ImGui "##id" suffix; only the part before it is shown. With no icon it is a plain ColonyHud::Button.
	 */
	bool Button(UTexture* Icon, const FString& Label, bool bActive, const ImVec2& Size);

	/** How many icons have been loaded since the game started (smoke test `hud`). */
	int32 LoadedCount();
}
