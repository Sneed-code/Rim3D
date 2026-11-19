#include "PowerOverlaySubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"

#include <imgui.h>

#include "OverseerSubsystem.h"
#include "SimPicking.h"
#include "SimSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Power.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Temperature.h"

namespace
{
	TAutoConsoleVariable<int32> GOverlayPower(
		TEXT("Overlay.Power"), 0, TEXT("1 shows the power view in the overseer screen: grids, power made and used, batteries."), ECVF_Default);
	TAutoConsoleVariable<int32> GOverlayTemperature(
		TEXT("Overlay.Temperature"), 0, TEXT("1 shows the temperature view in the overseer screen: each room's temperature, coolers' sides."), ECVF_Default);

	/** Markers further than this from the camera are not drawn (metres). */
	constexpr float PowerOverlayMaxDistanceM = 160.0f;

	ImGuiWindowFlags PowerOverlayFlags()
	{
		return ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	}

	void PowerOverlayText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get());
	}

	void PowerOverlayTextColored(const ImVec4& Color, const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextColored(Color, "%s", Utf8.Get());
	}

	/** A grid's own colour: one of eight easy-to-tell-apart hues by its id. */
	ImU32 PowerOverlayGridColour(uint32 Net, int32 Alpha)
	{
		static const int32 Hues[8][3] = {
			{255, 210, 60}, {90, 200, 255}, {255, 120, 200}, {150, 255, 120}, {255, 150, 70}, {180, 140, 255}, {80, 240, 200}, {240, 240, 240}};
		const int32* C = Hues[(Net == 0 ? 7 : (Net - 1)) % 8];
		return IM_COL32(C[0], C[1], C[2], Alpha);
	}

	/** Blue when freezing, cyan when cold, green when comfortable, orange when hot, red past 35 C. */
	ImU32 PowerOverlayTemperatureColour(float Celsius)
	{
		if (Celsius < 0.0f) { return IM_COL32(110, 150, 255, 255); }
		if (Celsius < 12.0f) { return IM_COL32(120, 220, 255, 255); }
		if (Celsius <= 26.0f) { return IM_COL32(130, 235, 130, 255); }
		if (Celsius <= 35.0f) { return IM_COL32(255, 170, 70, 255); }
		return IM_COL32(255, 80, 70, 255);
	}

	ImVec4 PowerOverlayVec(ImU32 Colour)
	{
		return ImGui::ColorConvertU32ToFloat4(Colour);
	}

	/** Text with a dark shadow, centred on a point. */
	void PowerOverlayLabel(ImDrawList* Draw, const ImVec2& Centre, float Size, ImU32 Colour, const char* Text)
	{
		ImFont* Font = ImGui::GetFont();
		const ImVec2 Extent = Font->CalcTextSizeA(Size, FLT_MAX, 0.0f, Text);
		const ImVec2 At(Centre.x - Extent.x * 0.5f, Centre.y - Extent.y * 0.5f);
		Draw->AddText(Font, Size, ImVec2(At.x + 1.5f, At.y + 1.5f), IM_COL32(0, 0, 0, 200), Text);
		Draw->AddText(Font, Size, At, Colour, Text);
	}

	FString PowerOverlayWatts(double Watts)
	{
		return FString::Printf(TEXT("%.0f W"), Watts);
	}
}

bool UPowerOverlaySubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UPowerOverlaySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UPowerOverlaySubsystem, STATGROUP_Tickables);
}

void UPowerOverlaySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	BridgeSource = Collection.InitializeDependency<USimSubsystem>();
	Collection.InitializeDependency<UOverseerSubsystem>();
}

USimSubsystem* UPowerOverlaySubsystem::GetBridge() const
{
	return BridgeSource.Get();
}

bool UPowerOverlaySubsystem::IsOverseerActive() const
{
	const UWorld* OwnWorld = GetWorld();
	const UOverseerSubsystem* Overseer = OwnWorld ? OwnWorld->GetSubsystem<UOverseerSubsystem>() : nullptr;
	return Overseer && Overseer->IsOverseerActive();
}

bool UPowerOverlaySubsystem::IsPowerViewOn() const { return GOverlayPower.GetValueOnGameThread() != 0; }
bool UPowerOverlaySubsystem::IsTemperatureViewOn() const { return GOverlayTemperature.GetValueOnGameThread() != 0; }

void UPowerOverlaySubsystem::SetPowerView(bool bOn)
{
	GOverlayPower->Set(bOn ? 1 : 0, ECVF_SetByCode);
}

void UPowerOverlaySubsystem::SetTemperatureView(bool bOn)
{
	GOverlayTemperature->Set(bOn ? 1 : 0, ECVF_SetByCode);
}

void UPowerOverlaySubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim() || !IsOverseerActive())
	{
		return;
	}
	IConsoleVariable* ShowUi = IConsoleManager::Get().FindConsoleVariable(TEXT("Overseer.UI"));
	if (ShowUi && ShowUi->GetInt() == 0)
	{
		return;
	}
	const ImGui::FScopedContext ScopedContext;
	if (!ScopedContext)
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	if (Viewport->Size.x <= 0.0f || Viewport->Size.y <= 0.0f)
	{
		return;
	}
	const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
	DrawToggles(Scale);
	float Top = Viewport->Pos.y + Viewport->Size.y * 0.42f;
	if (IsPowerViewOn())
	{
		DrawPowerView(*Bridge, Scale);
		DrawGridList(*Bridge, Scale, Top);
	}
	if (IsTemperatureViewOn())
	{
		DrawTemperatureView(*Bridge, Scale, Top);
	}
	DrawSelectedPower(*Bridge, Scale, Top);
}

void UPowerOverlaySubsystem::DrawToggles(float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float Margin = 12.0f * Scale;
	// Bottom right, above the key hints.
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - Margin, Viewport->Pos.y + Viewport->Size.y - Margin - 44.0f * Scale),
		ImGuiCond_Always, ImVec2(1.0f, 1.0f));
	ImGui::SetNextWindowBgAlpha(0.6f);
	if (ImGui::Begin("##OverlayToggles", nullptr, PowerOverlayFlags()))
	{
		bool bPower = IsPowerViewOn();
		if (ImGui::Checkbox("Power", &bPower))
		{
			SetPowerView(bPower);
		}
		ImGui::SameLine();
		bool bTemperature = IsTemperatureViewOn();
		if (ImGui::Checkbox("Temperature", &bTemperature))
		{
			SetTemperatureView(bTemperature);
		}
	}
	ImGui::End();
}

void UPowerOverlaySubsystem::DrawPowerView(USimSubsystem& Bridge, float Scale)
{
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (!PC)
	{
		return;
	}
	const FVector Camera = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	ImDrawList* Draw = ImGui::GetBackgroundDrawList();
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	const float MaxDistanceCm = PowerOverlayMaxDistanceM * 100.0f;
	int32 Markers = 0;
	for (const colony::ThingView& View : Snapshot.things)
	{
		if (View.category != colony::ThingCategory::Building || View.power == colony::PowerState::None ||
			View.stage != colony::ConstructionStage::Built)
		{
			continue;
		}
		const FVector WorldAt = Bridge.SimToWorld(View.x, View.y, View.level) + FVector(0.0, 0.0, 40.0);
		if (FVector::DistSquared(WorldAt, Camera) > MaxDistanceCm * MaxDistanceCm)
		{
			continue;
		}
		FVector2D P;
		if (!PC->ProjectWorldLocationToScreen(WorldAt, P, true))
		{
			continue;
		}
		const ImVec2 At(Viewport->Pos.x + static_cast<float>(P.X), Viewport->Pos.y + static_cast<float>(P.Y));
		const colony::PowerInfo& Info = Sim.power().info(View.def);
		const ImU32 Grid = PowerOverlayGridColour(View.powerNet, View.powerNet == 0 ? 120 : 235);
		if (Info.transmitter && !Info.switchable)
		{
			// A conduit: a small dot of its grid's colour (grey when on no grid).
			Draw->AddCircleFilled(At, 3.0f * Scale, Grid);
			++Markers;
			continue;
		}
		ImU32 Ring = IM_COL32(150, 150, 150, 235);
		if (View.power == colony::PowerState::Powered)
		{
			Ring = IM_COL32(70, 230, 90, 255);
		}
		else if (View.power == colony::PowerState::Unpowered)
		{
			Ring = IM_COL32(255, 60, 50, 255);
		}
		const float Radius = 7.0f * Scale;
		Draw->AddCircleFilled(At, Radius, Grid);
		Draw->AddCircle(At, Radius + 1.5f * Scale, Ring, 0, 3.0f * Scale);
		if (View.power == colony::PowerState::Unpowered)
		{
			PowerOverlayLabel(Draw, ImVec2(At.x, At.y - Radius - 10.0f * Scale), 16.0f * Scale, Ring, "no power");
		}
		else if (Info.output > 0.0 && View.powerWatts > 0.5f)
		{
			const FTCHARToUTF8 Text(*FString::Printf(TEXT("+%.0f W"), View.powerWatts));
			PowerOverlayLabel(Draw, ImVec2(At.x, At.y - Radius - 10.0f * Scale), 15.0f * Scale, IM_COL32(255, 235, 140, 255), Text.Get());
		}
		if (View.batteryFill >= 0.0f)
		{
			// A battery: a charge bar under the dot.
			const float W = 30.0f * Scale;
			const float H = 6.0f * Scale;
			const ImVec2 Min(At.x - W * 0.5f, At.y + Radius + 4.0f * Scale);
			Draw->AddRectFilled(Min, ImVec2(Min.x + W, Min.y + H), IM_COL32(20, 20, 20, 200));
			Draw->AddRectFilled(Min, ImVec2(Min.x + W * FMath::Clamp(View.batteryFill, 0.0f, 1.0f), Min.y + H), IM_COL32(110, 230, 110, 235));
			Draw->AddRect(Min, ImVec2(Min.x + W, Min.y + H), IM_COL32(230, 230, 230, 200));
		}
		++Markers;
	}
	Stats.PowerMarkers = Markers;
	++Stats.PowerFrames;
}

void UPowerOverlaySubsystem::DrawGridList(USimSubsystem& Bridge, float Scale, float& InOutTop)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float Margin = 12.0f * Scale;
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - Margin, InOutTop), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.75f);
	if (ImGui::Begin("##OverlayGrids", nullptr, PowerOverlayFlags()))
	{
		PowerOverlayText(TEXT("Power grids"));
		if (Snapshot.powerNets.empty())
		{
			PowerOverlayTextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), TEXT("No grids yet: join generators and machines with power conduits."));
		}
		int32 Shown = 0;
		for (const colony::PowerNetView& Net : Snapshot.powerNets)
		{
			if (++Shown > 12)
			{
				PowerOverlayTextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), FString::Printf(TEXT("... and %d more"), static_cast<int32>(Snapshot.powerNets.size()) - 12));
				break;
			}
			ImGui::TextColored(PowerOverlayVec(PowerOverlayGridColour(Net.id, 255)), "%s", "o");
			ImGui::SameLine();
			FString Line = FString::Printf(TEXT("Grid %u: making %s, using %s"), Net.id, *PowerOverlayWatts(Net.generation), *PowerOverlayWatts(Net.demand));
			if (Net.capacity > 0.0)
			{
				Line += FString::Printf(TEXT(", batteries %.0f / %.0f Wd"), Net.stored, Net.capacity);
			}
			if (Net.satisfied)
			{
				PowerOverlayText(Line);
			}
			else
			{
				PowerOverlayTextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), Line + TEXT("  SHORT OF POWER"));
			}
		}
		InOutTop += ImGui::GetWindowHeight() + 6.0f * Scale;
	}
	ImGui::End();
}

void UPowerOverlaySubsystem::DrawTemperatureView(USimSubsystem& Bridge, float Scale, float& InOutTop)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float Margin = 12.0f * Scale;
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	const colony::Sim& Sim = *Bridge.GetSim();
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - Margin, InOutTop), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.75f);
	if (ImGui::Begin("##OverlayTemperature", nullptr, PowerOverlayFlags()))
	{
		ImGui::TextUnformatted("Outdoors:");
		ImGui::SameLine();
		ImGui::TextColored(PowerOverlayVec(PowerOverlayTemperatureColour(Snapshot.outdoorTemperature)), "%.0f C", Snapshot.outdoorTemperature);
		InOutTop += ImGui::GetWindowHeight() + 6.0f * Scale;
	}
	ImGui::End();

	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (!PC)
	{
		return;
	}
	const FVector Camera = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
	const float MaxDistanceCm = PowerOverlayMaxDistanceM * 100.0f;
	ImDrawList* Draw = ImGui::GetBackgroundDrawList();
	const auto Screen = [&](const FVector& WorldAt, ImVec2& Out)
	{
		if (FVector::DistSquared(WorldAt, Camera) > MaxDistanceCm * MaxDistanceCm)
		{
			return false;
		}
		FVector2D P;
		if (!PC->ProjectWorldLocationToScreen(WorldAt, P, true))
		{
			return false;
		}
		Out = ImVec2(Viewport->Pos.x + static_cast<float>(P.X), Viewport->Pos.y + static_cast<float>(P.Y));
		return true;
	};
	int32 Labels = 0;
	for (const colony::RoomView& Room : Snapshot.rooms)
	{
		if (Room.outdoors || Room.cellCount <= 0)
		{
			continue;
		}
		const float CX = static_cast<float>(Room.bounds.minX) + static_cast<float>(Room.bounds.sizeX) * 0.5f;
		const float CY = static_cast<float>(Room.bounds.minY) + static_cast<float>(Room.bounds.sizeY) * 0.5f;
		ImVec2 At;
		if (!Screen(Bridge.SimToWorld(CX, CY, Room.bounds.level) + FVector(0.0, 0.0, 120.0), At))
		{
			continue;
		}
		const FTCHARToUTF8 Text(*FString::Printf(TEXT("%.0f C"), Room.temperature));
		PowerOverlayLabel(Draw, At, 24.0f * Scale, PowerOverlayTemperatureColour(Room.temperature), Text.Get());
		++Labels;
	}
	// Coolers: a blue dot where they cool, a red one where they blow the heat.
	for (const colony::ThingView& View : Snapshot.things)
	{
		if (View.category != colony::ThingCategory::Building || View.stage != colony::ConstructionStage::Built)
		{
			continue;
		}
		colony::CellPos Cold;
		colony::CellPos Hot;
		if (!Sim.temperature().sidesOf(View.id, Cold, Hot))
		{
			continue;
		}
		ImVec2 ColdAt;
		ImVec2 HotAt;
		const FVector Lift(0.0, 0.0, 60.0);
		if (Screen(Bridge.SimToWorld(Cold.x + 0.5f, Cold.y + 0.5f, Cold.level) + Lift, ColdAt))
		{
			Draw->AddCircleFilled(ColdAt, 6.0f * Scale, IM_COL32(90, 160, 255, 230));
		}
		if (Screen(Bridge.SimToWorld(Hot.x + 0.5f, Hot.y + 0.5f, Hot.level) + Lift, HotAt))
		{
			Draw->AddCircleFilled(HotAt, 6.0f * Scale, IM_COL32(255, 90, 60, 230));
		}
	}
	Stats.RoomLabels = Labels;
	++Stats.TemperatureFrames;
}

void UPowerOverlaySubsystem::DrawSelectedPower(USimSubsystem& Bridge, float Scale, float Top)
{
	const UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
	if (!Overseer || Overseer->GetSelection().Kind != ESimPickKind::Thing)
	{
		return;
	}
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	const colony::ThingView* View = Snapshot.find(Overseer->GetSelection().Thing);
	if (!View || View->category != colony::ThingCategory::Building || View->power == colony::PowerState::None ||
		View->stage != colony::ConstructionStage::Built)
	{
		return;
	}
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::PowerInfo& Info = Sim.power().info(View->def);
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float Margin = 12.0f * Scale;
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - Margin, Top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.85f);
	if (ImGui::Begin("##OverlaySelectedPower", nullptr, PowerOverlayFlags()))
	{
		++Stats.SelectedPanelFrames;
		FString Name = UTF8_TO_TCHAR(Sim.defs().get(View->def).label().c_str());
		if (!Name.IsEmpty())
		{
			Name[0] = FChar::ToUpper(Name[0]);
		}
		PowerOverlayText(Name + TEXT(": power"));
		switch (View->power)
		{
			case colony::PowerState::Powered:
				PowerOverlayTextColored(ImVec4(0.45f, 0.95f, 0.5f, 1.0f), Info.switchable ? TEXT("Switch closed: joins its cables") : TEXT("Has power"));
				break;
			case colony::PowerState::Unpowered:
				PowerOverlayTextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), View->powerNet == 0 ? TEXT("No power: not joined to a grid by conduits") : TEXT("No power: its grid is short"));
				break;
			case colony::PowerState::Off:
				PowerOverlayTextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f), Info.switchable ? TEXT("Switch open: cuts its cables") : TEXT("Switched off"));
				break;
			default:
				break;
		}
		if (Info.output > 0.0)
		{
			PowerOverlayText(FString::Printf(TEXT("Making %.0f W (at best %.0f W)"), FMath::Max(0.0f, View->powerWatts), Info.output));
		}
		else if (Info.consumption > 0.0)
		{
			const float Using = FMath::Max(0.0f, -View->powerWatts);
			FString Line = FString::Printf(TEXT("Using %.0f W (full %.0f W)"), Using, Info.consumption);
			if (Sim.defs().get(View->def).has("heatPusher.lowPowerFactor") && Using > 0.0f && Using < Info.consumption * 0.5)
			{
				Line += TEXT(": idle at its target");
			}
			PowerOverlayText(Line);
		}
		if (View->batteryFill >= 0.0f)
		{
			PowerOverlayText(FString::Printf(TEXT("Charge %.0f%% of %.0f Wd"), View->batteryFill * 100.0f, Info.battery));
		}
		if (View->powerNet > 0 && View->powerNet <= Snapshot.powerNets.size())
		{
			const colony::PowerNetView& Net = Snapshot.powerNets[View->powerNet - 1];
			ImGui::TextColored(PowerOverlayVec(PowerOverlayGridColour(Net.id, 255)), "%s", "o");
			ImGui::SameLine();
			PowerOverlayText(FString::Printf(TEXT("Grid %u: making %.0f W, using %.0f W"), Net.id, Net.generation, Net.demand));
			if (Net.capacity > 0.0)
			{
				PowerOverlayText(FString::Printf(TEXT("Batteries %.0f / %.0f Wd"), Net.stored, Net.capacity));
			}
		}
		// Anything but a plain conduit or a battery can be switched.
		const bool bFlickable = Info.switchable || Info.output > 0.0 || Info.consumption > 0.0;
		if (bFlickable)
		{
			const bool bOn = View->power != colony::PowerState::Off;
			const char* Label = Info.switchable ? (bOn ? "Open switch" : "Close switch") : (bOn ? "Switch off" : "Switch on");
			if (ImGui::Button(Label))
			{
				Bridge.Submit(colony::FlickCommand{View->id, !bOn});
				++Stats.FlicksSent;
			}
		}
	}
	ImGui::End();
}
