#pragma once

// Where the overseer HUD's boxes go (sneed 2026-11-24, docs/design/ui-layout.md "Layout v2"):
// - A window opened from the bottom bar (Work, Schedule, Assign, the Build menu...) starts at the screen's left edge and
//   sits right on the bar, no gap and no world showing round it; it is as wide as it needs to be.
// - The right-hand column stacks from the very top: the date box (with the alarm's Draft button), the inspect box (or the hover readout), the learning
//   helper, then the alerts. Each box reports its height after drawing; the next frame stacks on those heights, since
//   the boxes are drawn by different subsystems in no fixed order.

#include "CoreMinimal.h"

#include "ColonyHudStyle.h"

namespace HudLayout
{
	/** The top of the bottom bar: a bar window's bottom edge goes exactly here. */
	inline float TabBottom(const ImGuiViewport* Viewport, float Scale)
	{
		return Viewport->Pos.y + Viewport->Size.y - ColonyHud::BarHeight(Scale);
	}

	/** The screen's left edge, where a bar window starts. */
	inline float TabLeft(const ImGuiViewport* Viewport)
	{
		return Viewport->Pos.x;
	}

	/** The right column's boxes, top to bottom. */
	enum class ERightSlot : uint8
	{
		Date,     // time, day, the Draft button, date and weather (sneed 2026-11-25: top right)
		Inspect,
		Lessons,
		Alerts,
		Count
	};

	struct FRightColumn
	{
		float Height[static_cast<int32>(ERightSlot::Count)] = {};
		uint64 Frame[static_cast<int32>(ERightSlot::Count)] = {};
	};

	inline FRightColumn& RightColumn()
	{
		static FRightColumn Column;
		return Column;
	}

	/** The gap between stacked boxes and from the screen's top edge (none at the very top: "fully top right"). */
	inline float RightGap(float Scale) { return 4.0f * Scale; }

	/** Where this slot's box puts its top edge this frame: under the boxes above it that were drawn last frame. */
	inline float RightTop(const ImGuiViewport* Viewport, ERightSlot Slot, float Scale)
	{
		const FRightColumn& Column = RightColumn();
		float Top = Viewport->Pos.y;
		for (int32 Index = 0; Index < static_cast<int32>(Slot); ++Index)
		{
			if (Column.Frame[Index] + 2 >= GFrameCounter && Column.Height[Index] > 0.0f)
			{
				Top += Column.Height[Index] + RightGap(Scale);
			}
		}
		return Top;
	}

	/** Call inside the slot's window (before End): records its height for the boxes under it. */
	inline void ReportRight(ERightSlot Slot)
	{
		FRightColumn& Column = RightColumn();
		Column.Height[static_cast<int32>(Slot)] = ImGui::GetWindowSize().y;
		Column.Frame[static_cast<int32>(Slot)] = GFrameCounter;
	}

	/** Places the next window in its right-column slot, flush with the screen's right edge. */
	inline void PlaceRight(const ImGuiViewport* Viewport, ERightSlot Slot, float Scale)
	{
		ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x, RightTop(Viewport, Slot, Scale)), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	}

	/** Places the next window as a bar window: left edge of the screen, bottom on the bar. */
	inline void PlaceTab(const ImGuiViewport* Viewport, float Scale)
	{
		ImGui::SetNextWindowPos(ImVec2(TabLeft(Viewport), TabBottom(Viewport, Scale)), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
	}
}
