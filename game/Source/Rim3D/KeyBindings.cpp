#include "KeyBindings.h"

#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Input/Events.h"
#include "Misc/ConfigCacheIni.h"

#include <imgui.h>

namespace
{
	struct FKeyBindRow
	{
		ERim3DAction Action;
		const TCHAR* Id;      // the ini key: never change one, or saved bindings are lost
		const TCHAR* Name;    // what the Controls page shows
		const TCHAR* Group;
		FKey Default;
	};

	const TCHAR* const KeyBindSection = TEXT("Rim3D.Keys");

	// Shown in this order on the Controls page, by group.
	const TArray<FKeyBindRow>& KeyBindRows()
	{
		static const TArray<FKeyBindRow> Rows = {
			{ERim3DAction::MoveForward, TEXT("MoveForward"), TEXT("Move forward"), TEXT("Moving"), EKeys::W},
			{ERim3DAction::MoveBack, TEXT("MoveBack"), TEXT("Move back"), TEXT("Moving"), EKeys::S},
			{ERim3DAction::MoveLeft, TEXT("MoveLeft"), TEXT("Move left"), TEXT("Moving"), EKeys::A},
			{ERim3DAction::MoveRight, TEXT("MoveRight"), TEXT("Move right"), TEXT("Moving"), EKeys::D},
			{ERim3DAction::WalkSlow, TEXT("WalkSlow"), TEXT("Walk slowly (hold)"), TEXT("Moving"), EKeys::LeftControl},
			{ERim3DAction::Sprint, TEXT("Sprint"), TEXT("Sprint (on / off)"), TEXT("Moving"), EKeys::LeftShift},
			{ERim3DAction::Jump, TEXT("Jump"), TEXT("Jump"), TEXT("Moving"), EKeys::SpaceBar},
			{ERim3DAction::RollDash, TEXT("RollDash"), TEXT("Roll (third person) / dash (first person)"), TEXT("Moving"), EKeys::T},
			{ERim3DAction::Interact, TEXT("Interact"), TEXT("Interact: pick up, work, use"), TEXT("Hands"), EKeys::F},
			{ERim3DAction::PutDown, TEXT("PutDown"), TEXT("Put down what you hold"), TEXT("Hands"), EKeys::X},
			{ERim3DAction::EquipWeapon, TEXT("EquipWeapon"), TEXT("Equip the nearest weapon"), TEXT("Hands"), EKeys::E},
			{ERim3DAction::SwitchWeapon, TEXT("SwitchWeapon"), TEXT("Switch weapon"), TEXT("Fighting"), EKeys::Q},
			{ERim3DAction::Reload, TEXT("Reload"), TEXT("Reload"), TEXT("Fighting"), EKeys::R},
			{ERim3DAction::Crouch, TEXT("Crouch"), TEXT("Crouch (on / off)"), TEXT("Fighting"), EKeys::C},
			{ERim3DAction::Prone, TEXT("Prone"), TEXT("Lie prone (on / off)"), TEXT("Fighting"), EKeys::Z},
			{ERim3DAction::Patch, TEXT("Patch"), TEXT("Patch a bleeding wound"), TEXT("Fighting"), EKeys::H},
			{ERim3DAction::Grenade, TEXT("Grenade"), TEXT("Throw a grenade"), TEXT("Fighting"), EKeys::G},
			{ERim3DAction::Attack, TEXT("Attack"), TEXT("Shoot / swing"), TEXT("Fighting"), EKeys::LeftMouseButton},
			{ERim3DAction::AimBlock, TEXT("AimBlock"), TEXT("Aim / block (hold)"), TEXT("Fighting"), EKeys::RightMouseButton},
			{ERim3DAction::FocusNextTarget, TEXT("FocusNextTarget"), TEXT("Focus the next enemy"), TEXT("Fighting"), EKeys::Tab},
			{ERim3DAction::EndFocus, TEXT("EndFocus"), TEXT("Lock on / let go"), TEXT("Fighting"), EKeys::MiddleMouseButton},
			{ERim3DAction::SleepHere, TEXT("SleepHere"), TEXT("Sleep here"), TEXT("Other"), EKeys::N},
			{ERim3DAction::Inventory, TEXT("Inventory"), TEXT("Inventory"), TEXT("Other"), EKeys::I},
			{ERim3DAction::BuildMode, TEXT("BuildMode"), TEXT("Build"), TEXT("Other"), EKeys::B},
			{ERim3DAction::ToggleView, TEXT("ToggleView"), TEXT("First / third person"), TEXT("View"), EKeys::V},
			{ERim3DAction::SwitchMode, TEXT("SwitchMode"), TEXT("Overseer view and back"), TEXT("View"), EKeys::O},
			{ERim3DAction::TakeOver, TEXT("TakeOver"), TEXT("Take over the colonist you look at"), TEXT("View"), EKeys::Y},
			{ERim3DAction::FreeHandToggle, TEXT("FreeHandToggle"), TEXT("Free hand mode (on / off)"), TEXT("Free hand"), EKeys::J},
			{ERim3DAction::FreeHandGrip, TEXT("FreeHandGrip"), TEXT("Grip (hold)"), TEXT("Free hand"), EKeys::LeftMouseButton},
			{ERim3DAction::FreeHandLook, TEXT("FreeHandLook"), TEXT("Look around (hold)"), TEXT("Free hand"), EKeys::RightMouseButton},
			{ERim3DAction::FreeHandReachOut, TEXT("FreeHandReachOut"), TEXT("Reach further"), TEXT("Free hand"), EKeys::MouseScrollUp},
			{ERim3DAction::FreeHandReachIn, TEXT("FreeHandReachIn"), TEXT("Reach closer"), TEXT("Free hand"), EKeys::MouseScrollDown},
		};
		return Rows;
	}

	const FKeyBindRow* KeyBindFind(ERim3DAction Action)
	{
		for (const FKeyBindRow& Row : KeyBindRows())
		{
			if (Row.Action == Action)
			{
				return &Row;
			}
		}
		return nullptr;
	}

	TMap<ERim3DAction, FKey>& KeyBindCurrent()
	{
		static TMap<ERim3DAction, FKey> Current;
		static bool bLoaded = false;
		if (!bLoaded && GConfig)
		{
			bLoaded = true;
			for (const FKeyBindRow& Row : KeyBindRows())
			{
				FString Saved;
				if (GConfig->GetString(KeyBindSection, Row.Id, Saved, GGameUserSettingsIni) && !Saved.IsEmpty())
				{
					const FKey Key(*Saved);
					if (Key.IsValid())
					{
						Current.Add(Row.Action, Key);
					}
				}
			}
		}
		return Current;
	}

	// The Controls page waiting for a key: a Slate input processor sees the next key or mouse button before the game and
	// ImGui do, so any key can be bound (even one ImGui would eat).
	class FKeyBindCapture : public IInputProcessor
	{
	public:
		ERim3DAction Action = ERim3DAction::Count;
		bool bDone = false;

		virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}

		virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& Event) override
		{
			Take(Event.GetKey());
			return true;
		}

		virtual bool HandleMouseButtonDownEvent(FSlateApplication&, const FPointerEvent& Event) override
		{
			Take(Event.GetEffectingButton());
			return true;
		}

		virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication&, const FPointerEvent& Event, const FPointerEvent*) override
		{
			Take(Event.GetWheelDelta() >= 0.0f ? EKeys::MouseScrollUp : EKeys::MouseScrollDown);
			return true;
		}

		virtual const TCHAR* GetDebugName() const override { return TEXT("Rim3DKeyCapture"); }

	private:
		void Take(const FKey& Key)
		{
			if (bDone)
			{
				return;
			}
			bDone = true;
			if (Key != EKeys::Escape && Action != ERim3DAction::Count)
			{
				Rim3DKeys::Set(Action, Key);
			}
		}
	};

	TSharedPtr<FKeyBindCapture> GKeyBindCapture;
	// Two frames of grace after a capture ends, so the key just bound doesn't also act in the game.
	uint64 GKeyBindCaptureEndFrame = 0;

	void KeyBindStopCapture()
	{
		if (GKeyBindCapture.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(GKeyBindCapture);
		}
		GKeyBindCapture.Reset();
		GKeyBindCaptureEndFrame = GFrameCounter;
	}

	void KeyBindStartCapture(ERim3DAction Action)
	{
		KeyBindStopCapture();
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		GKeyBindCapture = MakeShared<FKeyBindCapture>();
		GKeyBindCapture->Action = Action;
		FSlateApplication::Get().RegisterInputPreProcessor(GKeyBindCapture, 0);
	}

	bool KeyBindQuiet()
	{
		return GKeyBindCapture.IsValid() || GFrameCounter <= GKeyBindCaptureEndFrame + 2;
	}

	std::string KeyBindUtf8(const FString& Text)
	{
		return std::string(TCHAR_TO_UTF8(*Text));
	}
}

namespace Rim3DKeys
{
	FKey GetDefault(ERim3DAction Action)
	{
		const FKeyBindRow* Row = KeyBindFind(Action);
		return Row ? Row->Default : EKeys::Invalid;
	}

	FKey Get(ERim3DAction Action)
	{
		if (const FKey* Bound = KeyBindCurrent().Find(Action))
		{
			return *Bound;
		}
		return GetDefault(Action);
	}

	void Set(ERim3DAction Action, const FKey& Key)
	{
		const FKeyBindRow* Row = KeyBindFind(Action);
		if (!Row || !Key.IsValid())
		{
			return;
		}
		KeyBindCurrent().Add(Action, Key);
		if (GConfig)
		{
			GConfig->SetString(KeyBindSection, Row->Id, *Key.GetFName().ToString(), GGameUserSettingsIni);
			GConfig->Flush(false, GGameUserSettingsIni);
		}
	}

	void ResetAll()
	{
		for (const FKeyBindRow& Row : KeyBindRows())
		{
			Set(Row.Action, Row.Default);
		}
	}

	FString GetDisplayName(ERim3DAction Action)
	{
		const FKeyBindRow* Row = KeyBindFind(Action);
		return Row ? FString(Row->Name) : FString();
	}

	FString GetKeyLabel(ERim3DAction Action)
	{
		return Get(Action).GetDisplayName().ToString();
	}

	bool IsDown(const APlayerController* PC, ERim3DAction Action)
	{
		return PC && !GKeyBindCapture.IsValid() && PC->IsInputKeyDown(Get(Action));
	}

	bool WasPressed(const APlayerController* PC, ERim3DAction Action)
	{
		return PC && !KeyBindQuiet() && PC->WasInputKeyJustPressed(Get(Action));
	}

	bool WasReleased(const APlayerController* PC, ERim3DAction Action)
	{
		return PC && PC->WasInputKeyJustReleased(Get(Action));
	}

	bool IsLockOnFocusEnabled()
	{
		bool bEnabled = true;
		if (GConfig)
		{
			GConfig->GetBool(KeyBindSection, TEXT("LockOnFocus"), bEnabled, GGameUserSettingsIni);
		}
		return bEnabled;
	}

	void SetLockOnFocusEnabled(bool bEnabled)
	{
		if (GConfig)
		{
			GConfig->SetBool(KeyBindSection, TEXT("LockOnFocus"), bEnabled, GGameUserSettingsIni);
			GConfig->Flush(false, GGameUserSettingsIni);
		}
	}

	bool IsCapturing()
	{
		return GKeyBindCapture.IsValid();
	}

	void DrawControlsPage(float Scale)
	{
		if (GKeyBindCapture.IsValid() && GKeyBindCapture->bDone)
		{
			KeyBindStopCapture();
		}
		bool bLockOn = IsLockOnFocusEnabled();
		if (ImGui::Checkbox("Lock-on focus (face and aim at the enemy you focus)", &bLockOn))
		{
			SetLockOnFocusEnabled(bLockOn);
		}
		ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Click a key, then press the new one. Esc cancels.");
		FString LastGroup;
		if (ImGui::BeginTable("##Rim3DKeys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 300.0f * Scale);
			ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 170.0f * Scale);
			for (const FKeyBindRow& Row : KeyBindRows())
			{
				if (LastGroup != Row.Group)
				{
					LastGroup = Row.Group;
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.6f, 1.0f), "%s", KeyBindUtf8(LastGroup).c_str());
				}
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(KeyBindUtf8(Row.Name).c_str());
				ImGui::TableSetColumnIndex(1);
				const bool bWaiting = GKeyBindCapture.IsValid() && GKeyBindCapture->Action == Row.Action;
				const FString Label = (bWaiting ? FString(TEXT("press a key...")) : GetKeyLabel(Row.Action)) + TEXT("##") + Row.Id;
				if (ImGui::Button(KeyBindUtf8(Label).c_str(), ImVec2(160.0f * Scale, 0.0f)) && !GKeyBindCapture.IsValid())
				{
					KeyBindStartCapture(Row.Action);
				}
				if (Get(Row.Action) != Row.Default && ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Default: %s", KeyBindUtf8(Row.Default.GetDisplayName().ToString()).c_str());
				}
			}
			ImGui::EndTable();
		}
		if (ImGui::Button("Reset all keys to default"))
		{
			ResetAll();
		}
	}
}
