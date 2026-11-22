#pragma once

// One look for every ImGui box on screen: "Canopy" (docs/design/ui-layout.md, "Canopy in game"), the same look as the title
// and new-game screens (docs/design/new-game.md). Dark green-black glass with square corners and a short jade bar on each
// box's top edge, bone text, jade for what is open or chosen, ember for costs and warnings. One border, one padding, two
// text sizes in boxes (large for titles and buttons that name a window, medium for the rest), tooltips after the mouse
// rests 1.2 s. Every subsystem that draws ImGui calls ColonyHud::ApplyFrameStyle(Scale) where it used to set
// FontScaleMain itself; the first call on an ImGui context also hooks the jade bars in (InstallCanopyBars).

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace ColonyHud
{
	inline ImVec4 Rgb(uint32 Hex, float Alpha = 1.0f)
	{
		return ImVec4(static_cast<float>((Hex >> 16) & 0xFF) / 255.0f, static_cast<float>((Hex >> 8) & 0xFF) / 255.0f,
			static_cast<float>(Hex & 0xFF) / 255.0f, Alpha);
	}

	// The Canopy palette (the same values as the new-game screens' NgStyle).
	inline ImVec4 PanelBg() { return ImVec4(0.030f, 0.050f, 0.043f, 0.92f); }
	inline ImVec4 Border() { return ImVec4(0.37f, 0.83f, 0.66f, 0.18f); }
	inline ImVec4 TextMain() { return ImVec4(0.91f, 0.89f, 0.84f, 1.0f); }   // bone
	inline ImVec4 TextDim() { return ImVec4(0.66f, 0.70f, 0.67f, 1.0f); }
	inline ImVec4 TextFaint() { return ImVec4(0.47f, 0.52f, 0.49f, 1.0f); }
	inline ImVec4 Accent() { return ImVec4(0.37f, 0.83f, 0.66f, 1.0f); }     // jade: the open tab, the picked category, headings
	inline ImVec4 AccentDark() { return ImVec4(0.08f, 0.26f, 0.20f, 0.95f); }
	inline ImVec4 AccentHover() { return ImVec4(0.12f, 0.33f, 0.26f, 0.98f); }
	inline ImVec4 Ember() { return ImVec4(1.00f, 0.56f, 0.36f, 1.0f); }      // costs, waiting offers
	inline ImVec4 EmberDark() { return ImVec4(0.36f, 0.16f, 0.08f, 0.95f); }
	inline ImVec4 Good() { return ImVec4(0.52f, 0.82f, 0.50f, 1.0f); }
	inline ImVec4 Warn() { return Ember(); }
	inline ImVec4 Bad() { return ImVec4(0.93f, 0.38f, 0.34f, 1.0f); }
	inline ImVec4 ButtonBg() { return ImVec4(0.055f, 0.085f, 0.075f, 0.90f); }
	inline ImVec4 ButtonHover() { return ImVec4(0.10f, 0.17f, 0.145f, 0.95f); }
	inline ImVec4 ButtonBorder() { return ImVec4(0.40f, 0.48f, 0.44f, 0.55f); }
	inline ImVec4 FieldBg() { return ImVec4(0.015f, 0.025f, 0.022f, 0.95f); }
	inline ImVec4 Locked() { return ImVec4(0.035f, 0.045f, 0.042f, 0.90f); }  // a button for something not yet open (research)

	/** How many jade bars have been drawn since the game started (the smoke test checks the hook runs). */
	inline int64& CanopyBarsDrawn()
	{
		static int64 Count = 0;
		return Count;
	}

	/** Name prefix of boxes that get no jade bar (the bottom bar draws its own line). */
	inline const char* NoBarPrefix() { return "##HudBar"; }

	/**
	 * Draws the jade bar on the top edge of every plain box this frame, just before ImGui ends it, so no window has to do
	 * it itself. Skipped: child windows, tooltips, popups, menus, see-through boxes (no background) and small ones (key
	 * hints, a single readout), and the bottom bar.
	 */
	inline void DrawCanopyBars(ImGuiContext* Context, ImGuiContextHook*)
	{
		if (!Context)
		{
			return;
		}
		const float ViewHeight = Context->Viewports.Size > 0 ? Context->Viewports[0]->Size.y : 1080.0f;
		const float Scale = FMath::Clamp(ViewHeight / 1080.0f, 1.0f, 2.0f);
		const ImU32 Jade = ImGui::ColorConvertFloat4ToU32(Accent());
		const ImGuiWindowFlags Skip = ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup |
			ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_NoBackground;
		const size_t PrefixLength = FCStringAnsi::Strlen(NoBarPrefix());
		for (ImGuiWindow* Window : Context->Windows)
		{
			if (!Window || !Window->Active || Window->Hidden || Window->Collapsed || (Window->Flags & Skip) != 0 || !Window->DrawList)
			{
				continue;
			}
			if (Window->Size.x < 150.0f * Scale || Window->Size.y < 56.0f * Scale)
			{
				continue;
			}
			if (Window->Name && FCStringAnsi::Strncmp(Window->Name, NoBarPrefix(), PrefixLength) == 0)
			{
				continue;
			}
			const ImVec2 At = Window->Pos;
			Window->DrawList->PushClipRect(At, ImVec2(At.x + Window->Size.x, At.y + Window->Size.y), false);
			Window->DrawList->AddRectFilled(At, ImVec2(At.x + 36.0f * Scale, At.y + 3.0f), Jade);
			Window->DrawList->PopClipRect();
			++CanopyBarsDrawn();
		}
	}

	/** Our hook's owner id, so one context gets it once. */
	inline ImGuiID CanopyHookOwner() { return 0x43414E4F; }

	/** Hooks DrawCanopyBars into the current ImGui context once. */
	inline void InstallCanopyBars()
	{
		ImGuiContext* Context = ImGui::GetCurrentContext();
		if (!Context)
		{
			return;
		}
		for (const ImGuiContextHook& Hook : Context->Hooks)
		{
			if (Hook.Owner == CanopyHookOwner())
			{
				return;
			}
		}
		ImGuiContextHook Hook;
		Hook.Type = ImGuiContextHookType_EndFramePre;
		Hook.Owner = CanopyHookOwner();
		Hook.Callback = &DrawCanopyBars;
		ImGui::AddContextHook(Context, &Hook);
	}

	/** Readable from 1080p to 4K: 1 at 1080 lines, 2 at 2160. */
	inline float ViewScale(const ImGuiViewport* Viewport)
	{
		return Viewport ? FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f) : 1.0f;
	}

	/** The player's text size (Settings > Game > Text size, console UI.TextSize 0 normal, 1 bigger). */
	inline float TextSizeFactor()
	{
		static IConsoleVariable* Variable = nullptr;
		if (!Variable)
		{
			Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("UI.TextSize"));
		}
		return Variable && Variable->GetInt() == 1 ? 1.2f : 1.0f;
	}

	/** Sets the shared look for this frame's windows. Cheap: a few style fields. */
	inline void ApplyFrameStyle(float Scale)
	{
		InstallCanopyBars();
		ImGuiStyle& Style = ImGui::GetStyle();
		Style.FontScaleMain = 1.2f * Scale * TextSizeFactor();
		Style.WindowRounding = 0.0f;
		Style.ChildRounding = 0.0f;
		Style.FrameRounding = 0.0f;
		Style.PopupRounding = 0.0f;
		Style.GrabRounding = 0.0f;
		Style.TabRounding = 0.0f;
		Style.WindowBorderSize = 1.0f;
		Style.FrameBorderSize = 1.0f;
		Style.PopupBorderSize = 1.0f;
		Style.WindowPadding = ImVec2(10.0f * Scale, 8.0f * Scale);
		Style.FramePadding = ImVec2(8.0f * Scale, 4.0f * Scale);
		Style.ItemSpacing = ImVec2(6.0f * Scale, 5.0f * Scale);
		Style.ItemInnerSpacing = ImVec2(5.0f * Scale, 4.0f * Scale);
		// Tooltips after the mouse rests about 1.2 s (SetItemTooltip and IsItemHovered(ImGuiHoveredFlags_ForTooltip)).
		Style.HoverDelayNormal = 1.2f;
		Style.HoverDelayShort = 0.6f;
		Style.HoverStationaryDelay = 0.15f;
		Style.HoverFlagsForTooltipMouse = ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_Stationary | ImGuiHoveredFlags_AllowWhenDisabled;

		ImVec4* C = Style.Colors;
		C[ImGuiCol_Text] = TextMain();
		C[ImGuiCol_TextDisabled] = TextFaint();
		C[ImGuiCol_WindowBg] = PanelBg();
		C[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		C[ImGuiCol_PopupBg] = ImVec4(0.020f, 0.034f, 0.030f, 0.98f);
		C[ImGuiCol_Border] = Border();
		C[ImGuiCol_FrameBg] = FieldBg();
		C[ImGuiCol_FrameBgHovered] = ButtonHover();
		C[ImGuiCol_FrameBgActive] = ButtonHover();
		C[ImGuiCol_TitleBg] = PanelBg();
		C[ImGuiCol_TitleBgActive] = ImVec4(0.045f, 0.075f, 0.065f, 0.98f);
		C[ImGuiCol_TitleBgCollapsed] = PanelBg();
		C[ImGuiCol_Button] = ButtonBg();
		C[ImGuiCol_ButtonHovered] = ButtonHover();
		C[ImGuiCol_ButtonActive] = AccentDark();
		C[ImGuiCol_Header] = ButtonBg();
		C[ImGuiCol_HeaderHovered] = ButtonHover();
		C[ImGuiCol_HeaderActive] = AccentDark();
		C[ImGuiCol_CheckMark] = Accent();
		C[ImGuiCol_SliderGrab] = Accent();
		C[ImGuiCol_SliderGrabActive] = TextMain();
		C[ImGuiCol_Separator] = Border();
		C[ImGuiCol_Tab] = ButtonBg();
		C[ImGuiCol_TabHovered] = ButtonHover();
		C[ImGuiCol_TabSelected] = AccentDark();
		C[ImGuiCol_PlotHistogram] = Good();
		C[ImGuiCol_TableHeaderBg] = ButtonBg();
		C[ImGuiCol_TableBorderStrong] = Border();
		C[ImGuiCol_TableBorderLight] = ImVec4(0.37f, 0.83f, 0.66f, 0.08f);
		C[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
	}

	/** The large text size, for titles: a thing's name, a window's title. */
	inline void PushLarge()
	{
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
	}

	inline void PopLarge()
	{
		ImGui::PopFont();
	}

	inline void Text(const FString& Value)
	{
		const FTCHARToUTF8 Utf8(*Value);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}

	inline void TextColored(const ImVec4& Color, const FString& Value)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Color);
		Text(Value);
		ImGui::PopStyleColor();
	}

	inline void Title(const FString& Value)
	{
		PushLarge();
		Text(Value);
		PopLarge();
	}

	/** A heading inside a window (Basics, Building...): jade, large. */
	inline void Heading(const FString& Value)
	{
		PushLarge();
		TextColored(Accent(), Value);
		PopLarge();
	}

	/** A tooltip that shows after the mouse rests 1.2 s on the last item. */
	inline void Tip(const FString& Value)
	{
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
		{
			const FTCHARToUTF8 Utf8(*Value);
			ImGui::SetTooltip("%s", Utf8.Get());
		}
	}

	/** A button that stays lit (jade edge, deep jade fill) while `bActive`. */
	inline bool Button(const FString& Label, bool bActive = false, const ImVec2& Size = ImVec2(0.0f, 0.0f))
	{
		if (bActive)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, AccentDark());
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHover());
			ImGui::PushStyleColor(ImGuiCol_Border, Accent());
			ImGui::PushStyleColor(ImGuiCol_Text, TextMain());
		}
		const FTCHARToUTF8 Utf8(*Label);
		const bool bClicked = ImGui::Button(Utf8.Get(), Size);
		if (bActive)
		{
			ImGui::PopStyleColor(4);
		}
		return bClicked;
	}

	/** Flags of a fixed box: no title, no moving, sized to fit, never steals focus. */
	inline ImGuiWindowFlags BoxFlags()
	{
		return ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	}

	/** The bottom bar's padding; with the frame height it gives the bar's height. */
	inline float BarPadY(float Scale)
	{
		return 5.0f * Scale;
	}

	/** Height of the bottom tab bar in the overseer view, in pixels (call after ApplyFrameStyle). */
	inline float BarHeight(float Scale)
	{
		return ImGui::GetFrameHeight() + 2.0f * BarPadY(Scale);
	}

	/** Where boxes that sit above the bottom bar put their bottom edge: the bar's top less a small gap. */
	inline float AboveBar(const ImGuiViewport* Viewport, float Scale)
	{
		return Viewport->Pos.y + Viewport->Size.y - BarHeight(Scale) - 6.0f * Scale;
	}

	/** The date box's top edge as it was last drawn (screen pixels), or a negative number before the first draw. */
	inline float& DateBoxTop()
	{
		static float Top = -1.0f;
		return Top;
	}

	/** The date box's left edge as it was last drawn, or a negative number before the first draw. */
	inline float& DateBoxLeft()
	{
		static float Left = -1.0f;
		return Left;
	}

	/**
	 * Space the date and weather box takes above the bar's right end, with a small gap: measured from where it was last
	 * drawn (its rows change: Step one second shows only while paused), else its three rows (time large, date, the level
	 * row of small buttons) and padding.
	 */
	inline float DateBoxHeight(float Scale)
	{
		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		if (DateBoxTop() > 0.0f && Viewport)
		{
			return FMath::Max(0.0f, AboveBar(Viewport, Scale) - DateBoxTop()) + 6.0f * Scale;
		}
		return 1.3f * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() +
			2.0f * ImGui::GetStyle().WindowPadding.y + 6.0f * Scale;
	}
}
