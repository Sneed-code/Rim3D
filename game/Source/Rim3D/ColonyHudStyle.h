#pragma once

// One look for every ImGui box on screen (docs/design/ui-layout.md, "Rules for every box"): one panel colour, one border,
// one corner radius, one padding, two text sizes (large for titles and buttons that name a window, medium for the rest),
// tooltips after the mouse rests 1.2 s. Colours are the mock-up's (https://claude.ai/artifact/QXsccQCbcwoBLcugZisa6N).
// Every subsystem that draws ImGui calls ColonyHud::ApplyFrameStyle(Scale) where it used to set FontScaleMain itself.

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

#include <imgui.h>

namespace ColonyHud
{
	inline ImVec4 Rgb(uint32 Hex, float Alpha = 1.0f)
	{
		return ImVec4(static_cast<float>((Hex >> 16) & 0xFF) / 255.0f, static_cast<float>((Hex >> 8) & 0xFF) / 255.0f,
			static_cast<float>(Hex & 0xFF) / 255.0f, Alpha);
	}

	// The palette.
	inline ImVec4 PanelBg() { return ImVec4(18.0f / 255.0f, 21.0f / 255.0f, 24.0f / 255.0f, 0.94f); }
	inline ImVec4 Border() { return Rgb(0x353d45); }
	inline ImVec4 TextMain() { return Rgb(0xe9e4d8); }
	inline ImVec4 TextDim() { return Rgb(0xa7b0b5); }
	inline ImVec4 TextFaint() { return Rgb(0x8d969c); }
	inline ImVec4 Accent() { return Rgb(0xe0b04a); }     // amber: the open tab, the picked category, headings
	inline ImVec4 AccentDark() { return Rgb(0x3a2c17); }
	inline ImVec4 Good() { return Rgb(0x79c983); }
	inline ImVec4 Warn() { return Rgb(0xe0a04a); }
	inline ImVec4 Bad() { return Rgb(0xe5675c); }
	inline ImVec4 ButtonBg() { return Rgb(0x1c2126); }
	inline ImVec4 ButtonHover() { return Rgb(0x262c32); }
	inline ImVec4 ButtonBorder() { return Rgb(0x4a535c); }
	inline ImVec4 FieldBg() { return Rgb(0x0e1013); }

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
		ImGuiStyle& Style = ImGui::GetStyle();
		Style.FontScaleMain = 1.2f * Scale * TextSizeFactor();
		Style.WindowRounding = 6.0f * Scale;
		Style.ChildRounding = 4.0f * Scale;
		Style.FrameRounding = 4.0f * Scale;
		Style.PopupRounding = 6.0f * Scale;
		Style.GrabRounding = 3.0f * Scale;
		Style.TabRounding = 4.0f * Scale;
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
		C[ImGuiCol_PopupBg] = ImVec4(14.0f / 255.0f, 16.0f / 255.0f, 19.0f / 255.0f, 0.98f);
		C[ImGuiCol_Border] = Border();
		C[ImGuiCol_FrameBg] = FieldBg();
		C[ImGuiCol_FrameBgHovered] = ButtonHover();
		C[ImGuiCol_FrameBgActive] = ButtonHover();
		C[ImGuiCol_TitleBg] = PanelBg();
		C[ImGuiCol_TitleBgActive] = Rgb(0x1c2126, 0.98f);
		C[ImGuiCol_TitleBgCollapsed] = PanelBg();
		C[ImGuiCol_Button] = ButtonBg();
		C[ImGuiCol_ButtonHovered] = ButtonHover();
		C[ImGuiCol_ButtonActive] = Rgb(0x2f363d);
		C[ImGuiCol_Header] = ButtonBg();
		C[ImGuiCol_HeaderHovered] = ButtonHover();
		C[ImGuiCol_HeaderActive] = Rgb(0x2f363d);
		C[ImGuiCol_CheckMark] = Accent();
		C[ImGuiCol_SliderGrab] = Accent();
		C[ImGuiCol_SliderGrabActive] = Rgb(0xf0c45e);
		C[ImGuiCol_Separator] = Border();
		C[ImGuiCol_Tab] = ButtonBg();
		C[ImGuiCol_TabHovered] = ButtonHover();
		C[ImGuiCol_TabSelected] = AccentDark();
		C[ImGuiCol_PlotHistogram] = Good();
		C[ImGuiCol_TableHeaderBg] = ButtonBg();
		C[ImGuiCol_TableBorderStrong] = Border();
		C[ImGuiCol_TableBorderLight] = Rgb(0x2b3238);
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

	/** A heading inside a window (Basics, Building...): amber, large. */
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

	/** A button that stays lit (amber edge, dark amber fill) while `bActive`. */
	inline bool Button(const FString& Label, bool bActive = false, const ImVec2& Size = ImVec2(0.0f, 0.0f))
	{
		if (bActive)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, AccentDark());
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(0x4a3a1f));
			ImGui::PushStyleColor(ImGuiCol_Border, Accent());
			ImGui::PushStyleColor(ImGuiCol_Text, Rgb(0xf5e3c3));
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

	/** Space the date and weather box takes above the bar's right end (two lines and padding). */
	inline float DateBoxHeight(float Scale)
	{
		return 2.0f * ImGui::GetTextLineHeightWithSpacing() + 2.0f * ImGui::GetStyle().WindowPadding.y + 6.0f * Scale;
	}
}
