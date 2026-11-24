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

	/** Ids of the boxes that keep a fixed width this frame (HoldWidth): text in them wraps at their edge. */
	inline TSet<ImGuiID>& HeldBoxes()
	{
		static TSet<ImGuiID> Held;
		return Held;
	}

	/** How many jade bars have been drawn since the game started (the smoke test checks the hook runs). */
	inline int64& CanopyBarsDrawn()
	{
		static int64 Count = 0;
		return Count;
	}

	/** Name prefix of boxes that get no jade bar (the bottom bar draws its own line). */
	inline const char* NoBarPrefix() { return "##HudBar"; }

	/** With console UI.LogBoxes 1, logs each box's width once per size (BOXWIDTH name width-at-1080), for HeldWidthOf. */
	inline void LogBoxWidth(const ImGuiWindow& Window, float Scale)
	{
		static IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("UI.LogBoxes"));
		if (!Variable || Variable->GetInt() == 0 || !Window.Name)
		{
			return;
		}
		static TSet<FString> Logged;
		const FString Name = UTF8_TO_TCHAR(Window.Name);
		const int32 Width = FMath::RoundToInt32(Window.Size.x / Scale);
		const FString Key = FString::Printf(TEXT("%s %d"), *Name, Width);
		if (!Logged.Contains(Key))
		{
			Logged.Add(Key);
			UE_LOG(LogTemp, Display, TEXT("BOXWIDTH %s %d (auto %d)"), *Name, Width, (Window.Flags & ImGuiWindowFlags_AlwaysAutoResize) != 0 ? 1 : 0);
		}
	}

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
			LogBoxWidth(*Window, Scale);
			if (Window->Size.x < 150.0f * Scale || Window->Size.y < 56.0f * Scale)
			{
				continue;
			}
			if (Window->Name && FCStringAnsi::Strncmp(Window->Name, NoBarPrefix(), PrefixLength) == 0)
			{
				continue;
			}
			const ImVec2 At = Window->Pos;
			const ImVec2 End(At.x + Window->Size.x, At.y + Window->Size.y);
			Window->DrawList->PushClipRect(At, End, false);
			// The glass (sneed 2026-11-24, "make it professional"): a faint jade glow fading down from the top edge, a
			// hairline of light along the top inside the border, and a darker foot, so every box reads as one lit pane.
			const float Glow = FMath::Min(Window->Size.y, 64.0f * Scale);
			Window->DrawList->AddRectFilledMultiColor(At, ImVec2(End.x, At.y + Glow), IM_COL32(94, 212, 168, 16), IM_COL32(94, 212, 168, 16),
				IM_COL32(94, 212, 168, 0), IM_COL32(94, 212, 168, 0));
			Window->DrawList->AddLine(ImVec2(At.x + 1.0f, At.y + 1.0f), ImVec2(End.x - 1.0f, At.y + 1.0f), IM_COL32(232, 228, 214, 22), 1.0f);
			const float Foot = FMath::Min(Window->Size.y * 0.5f, 48.0f * Scale);
			Window->DrawList->AddRectFilledMultiColor(ImVec2(At.x, End.y - Foot), End, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
				IM_COL32(0, 0, 0, 40), IM_COL32(0, 0, 0, 40));
			Window->DrawList->AddRectFilled(At, ImVec2(At.x + 36.0f * Scale, At.y + 3.0f), Jade);
			Window->DrawList->PopClipRect();
			++CanopyBarsDrawn();
		}
		HeldBoxes().Reset();  // HoldWidth marks them afresh each frame
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

	/**
	 * Every text in the game is this much bigger than it was (sneed 2026-11-24: "text size is too small, gotta be 50%
	 * bigger, universally"), while boxes keep their widths (HoldWidth) and wrap. Console UI.TextGrow (1 is the old size).
	 */
	inline float TextGrow()
	{
		static IConsoleVariable* Variable = nullptr;
		if (!Variable)
		{
			Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("UI.TextGrow"));
		}
		return Variable ? FMath::Clamp(Variable->GetFloat(), 0.75f, 2.5f) : 1.5f;
	}

	/**
	 * The widths boxes had before the text grew (sneed 2026-11-24: "i like the width ... make sure ui boxes don't expand
	 * wider"), in pixels at 1080 lines, measured on the PC with UI.TextGrow 1 and UI.LogBoxes 1 (BOXWIDTH lines, 2026-11-24). A box
	 * not listed sizes itself as before.
	 */
	inline float HeldWidthOf(const char* Name)
	{
		struct FHeld
		{
			const char* Name;
			float Width;
		};
		static const FHeld Widths[] = {
			{"##HudLesson", 380.0f},
			{"##HudHover", 220.0f},
			{"##HudInfo", 760.0f},
			{"##HudSettings", 745.0f},
			{"##HudAssign", 1100.0f},
			{"##HudAnimals", 1420.0f},
			{"##HudWildlife", 820.0f},
			{"##HudMechs", 620.0f},
			{"##HudResearch", 1880.0f},
			{"##HudInventory", 720.0f},
			{"##HudOutfits", 1000.0f},
			{"##HudAreas", 760.0f},
			{"##HudMedical", 1150.0f},
			{"##HudPrisoners", 980.0f},
			{"##BuildNav", 250.0f},
			{"##FactionsWindow", 760.0f},
		};
		for (const FHeld& Held : Widths)
		{
			if (FCStringAnsi::Strcmp(Held.Name, Name) == 0)
			{
				return Held.Width;
			}
		}
		return 0.0f;
	}

	/**
	 * Call just before ImGui::Begin of a box that sizes itself: it keeps the width it had before the text grew (or
	 * `Width` pixels at 1080 lines, if given) and its text wraps; its height still fits what it holds.
	 */
	inline void HoldWidth(const char* Name, float Scale, float Width = 0.0f)
	{
		const float At1080 = Width > 0.0f ? Width : HeldWidthOf(Name);
		static IConsoleVariable* Measuring = IConsoleManager::Get().FindConsoleVariable(TEXT("UI.LogBoxes"));
		if (At1080 <= 0.0f || (Measuring && Measuring->GetInt() != 0))
		{
			return;
		}
		const float W = At1080 * Scale;
		ImGui::SetNextWindowSizeConstraints(ImVec2(W, 0.0f), ImVec2(W, TNumericLimits<float>::Max()));
		HeldBoxes().Add(ImHashStr(Name));
	}

	/** Whether text drawn now should wrap at the box's edge: in a box of fixed width, never in one that fits its text. */
	inline bool WrapHere()
	{
		const ImGuiWindow* Window = ImGui::GetCurrentWindowRead();
		const ImGuiWindow* Root = Window ? Window->RootWindow : nullptr;
		if (!Root || (Window->Flags & ImGuiWindowFlags_Tooltip) != 0)
		{
			return false;
		}
		return (Root->Flags & ImGuiWindowFlags_AlwaysAutoResize) == 0 || HeldBoxes().Contains(Root->ID);
	}

	/** Sets the shared look for this frame's windows. Cheap: a few style fields. */
	inline void ApplyFrameStyle(float Scale)
	{
		InstallCanopyBars();
		ImGuiStyle& Style = ImGui::GetStyle();
		Style.FontScaleMain = 1.2f * Scale * TextSizeFactor() * TextGrow();
		Style.WindowRounding = 0.0f;
		Style.ChildRounding = 0.0f;
		Style.FrameRounding = 0.0f;
		Style.PopupRounding = 0.0f;
		Style.GrabRounding = 0.0f;
		Style.TabRounding = 0.0f;
		Style.WindowBorderSize = 1.0f;
		Style.FrameBorderSize = 1.0f;
		Style.PopupBorderSize = 1.0f;
		Style.WindowPadding = ImVec2(12.0f * Scale, 10.0f * Scale);
		Style.FramePadding = ImVec2(8.0f * Scale, 4.0f * Scale);
		Style.GrabMinSize = 10.0f * Scale;
		Style.ScrollbarSize = 10.0f * Scale;
		Style.ScrollbarRounding = 0.0f;
		Style.SeparatorTextBorderSize = 1.0f;
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
		C[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		C[ImGuiCol_ScrollbarGrab] = ImVec4(0.37f, 0.83f, 0.66f, 0.22f);
		C[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.37f, 0.83f, 0.66f, 0.40f);
		C[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.37f, 0.83f, 0.66f, 0.60f);
		C[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		C[ImGuiCol_TextSelectedBg] = ImVec4(0.37f, 0.83f, 0.66f, 0.30f);
		C[ImGuiCol_PlotLines] = Accent();
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
		const bool bWrap = WrapHere();
		if (bWrap)
		{
			ImGui::PushTextWrapPos(0.0f);  // at the box's edge: bigger text in the same width goes onto more lines
		}
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
		if (bWrap)
		{
			ImGui::PopTextWrapPos();
		}
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

	/** The bottom bar's padding; with the buttons' height it gives the bar's height. */
	inline float BarPadY(float Scale)
	{
		return 5.0f * Scale;
	}

	/** The bottom bar's buttons: a third taller than a plain button (sneed 2026-11-24: "make it a bit higher"). */
	inline float BarButtonHeight()
	{
		return ImGui::GetFrameHeight() * 1.35f;
	}

	/** Height of the bottom tab bar in the overseer view, in pixels (call after ApplyFrameStyle). */
	inline float BarHeight(float Scale)
	{
		return BarButtonHeight() + 2.0f * BarPadY(Scale);
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
