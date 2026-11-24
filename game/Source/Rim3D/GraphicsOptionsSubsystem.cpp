#include "GraphicsOptionsSubsystem.h"

#include "HAL/IConsoleManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "RenderUtils.h"
#include "Scalability.h"

#include "ColonyHudStyle.h"

#include <imgui.h>

DEFINE_LOG_CATEGORY_STATIC(LogGfxOptions, Log, All);

namespace
{
	const TCHAR* const GfxConfigSection = TEXT("Rim3D.Graphics");

	TAutoConsoleVariable<int32> GGfxRayTracing(
		TEXT("Gfx.RayTracing"), 1,
		TEXT("1 lights the world with hardware ray tracing (Lumen on the RTX cores), 0 with Lumen's software tracing (the fallback)."), ECVF_Default);
	TAutoConsoleVariable<int32> GGfxUpscaler(
		TEXT("Gfx.Upscaler"), 0, TEXT("0 Unreal's TSR (the default: the game is held back by the processor, not the graphics card), 1 NVIDIA DLSS (falls back to TSR when the plugin or card can't)."), ECVF_Default);
	TAutoConsoleVariable<int32> GGfxResolution(
		TEXT("Gfx.Resolution"), -1,
		TEXT("Render resolution: 0 Native, 1 Quality (67%), 2 Balanced (58%), 3 Performance (50%), 4 Ultra performance (33%); -1 picks Quality with DLSS and Native with TSR."),
		ECVF_Default);
	TAutoConsoleVariable<int32> GGfxFrameGeneration(
		TEXT("Gfx.FrameGeneration"), 0, TEXT("1 turns on DLSS frame generation (smoother, a little more input lag; RTX 40 and 50 cards)."), ECVF_Default);
	TAutoConsoleVariable<int32> GGfxRayReconstruction(
		TEXT("Gfx.RayReconstruction"), 0, TEXT("1 lets DLSS ray reconstruction clean up the ray-traced light (sharper, costs some speed; DLSS only)."), ECVF_Default);

	struct FGfxResolutionStep
	{
		const char* Name;
		float Percent;
	};
	// NVIDIA's standard DLSS scales; TSR uses the same steps.
	const FGfxResolutionStep GfxResolutionSteps[] = {
		{"Native (100%)", 100.0f},
		{"Quality (67%)", 66.7f},
		{"Balanced (58%)", 58.0f},
		{"Performance (50%)", 50.0f},
		{"Ultra performance (33%)", 33.3f},
	};
	constexpr int32 GfxResolutionStepCount = UE_ARRAY_COUNT(GfxResolutionSteps);

	// The settings the Settings window keeps, with the console variable each one lives in.
	const TCHAR* const GfxSavedVariables[] = {
		TEXT("Look.Quality"), TEXT("Gfx.RayTracing"), TEXT("Gfx.Upscaler"), TEXT("Gfx.Resolution"), TEXT("Gfx.FrameGeneration"), TEXT("Gfx.RayReconstruction"),
	};

	IConsoleVariable* GfxFind(const TCHAR* Name)
	{
		return IConsoleManager::Get().FindConsoleVariable(Name);
	}

	int32 GfxGetInt(const TCHAR* Name, int32 Fallback)
	{
		const IConsoleVariable* Var = GfxFind(Name);
		return Var ? Var->GetInt() : Fallback;
	}

	void GfxSetInt(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = GfxFind(Name))
		{
			if (Var->GetInt() != Value)
			{
				Var->Set(Value, ECVF_SetByCode);
			}
		}
	}

	bool GfxRadio(const char* Label, int32& Value, int32 Mine)
	{
		const bool bClicked = ImGui::RadioButton(Label, Value == Mine);
		if (bClicked)
		{
			Value = Mine;
		}
		return bClicked;
	}
}

void UGraphicsOptionsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Load();
	const FConsoleVariableDelegate OnChange = FConsoleVariableDelegate::CreateWeakLambda(this, [this](IConsoleVariable*) { Apply(); });
	for (TAutoConsoleVariable<int32>* Var : {&GGfxRayTracing, &GGfxUpscaler, &GGfxResolution, &GGfxFrameGeneration, &GGfxRayReconstruction})
	{
		Var->AsVariable()->SetOnChangedCallback(OnChange);
	}
	Apply();
	// The DLSS plugin loads after the engine starts (even after OnPostEngineInit in the editor), so look again once it has.
	PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddUObject(this, &UGraphicsOptionsSubsystem::Apply);
	ModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddUObject(this, &UGraphicsOptionsSubsystem::OnModulesChanged);
}

void UGraphicsOptionsSubsystem::OnModulesChanged(FName ModuleName, EModuleChangeReason Reason)
{
	static const FName GfxDlssModule(TEXT("DLSS"));
	static const FName GfxDlssgModule(TEXT("StreamlineDLSSG"));
	if (Reason == EModuleChangeReason::ModuleLoaded && (ModuleName == GfxDlssModule || ModuleName == GfxDlssgModule))
	{
		Apply();
	}
}

void UGraphicsOptionsSubsystem::Deinitialize()
{
	FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
	FModuleManager::Get().OnModulesChanged().Remove(ModulesChangedHandle);
	for (TAutoConsoleVariable<int32>* Var : {&GGfxRayTracing, &GGfxUpscaler, &GGfxResolution, &GGfxFrameGeneration, &GGfxRayReconstruction})
	{
		Var->AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate());
	}
	Super::Deinitialize();
}

bool UGraphicsOptionsSubsystem::IsRayTracingAvailable()
{
	return IsRayTracingEnabled();
}

bool UGraphicsOptionsSubsystem::IsDlssAvailable()
{
	// The DLSS plugin registers r.NGX.DLSS.Enable only when NGX started on an RTX card.
	return GfxFind(TEXT("r.NGX.DLSS.Enable")) != nullptr && FModuleManager::Get().IsModuleLoaded(TEXT("DLSS"));
}

bool UGraphicsOptionsSubsystem::IsFrameGenerationAvailable()
{
	return GfxFind(TEXT("r.Streamline.DLSSG.Enable")) != nullptr;
}

void UGraphicsOptionsSubsystem::Load()
{
	if (!GConfig)
	{
		return;
	}
	for (const TCHAR* Name : GfxSavedVariables)
	{
		int32 Value = 0;
		if (GConfig->GetInt(GfxConfigSection, Name, Value, GGameUserSettingsIni))
		{
			if (IConsoleVariable* Var = GfxFind(Name))
			{
				// Below a command-line or console setting, so -ini overrides and smoke tests still win.
				Var->Set(Value, ECVF_SetByGameSetting);
			}
		}
	}
}

void UGraphicsOptionsSubsystem::Save() const
{
	if (!GConfig)
	{
		return;
	}
	for (const TCHAR* Name : GfxSavedVariables)
	{
		if (const IConsoleVariable* Var = GfxFind(Name))
		{
			GConfig->SetInt(GfxConfigSection, Name, Var->GetInt(), GGameUserSettingsIni);
		}
	}
	GConfig->Flush(false, GGameUserSettingsIni);
}

void UGraphicsOptionsSubsystem::Apply()
{
	if (bApplying)
	{
		return;
	}
	TGuardValue<bool> Guard(bApplying, true);

	// Ray tracing: Lumen traces against the RTX hardware's scene, or its own distance fields when off or unavailable.
	const bool bRayTracing = GGfxRayTracing.GetValueOnGameThread() != 0 && IsRayTracingAvailable();
	GfxSetInt(TEXT("r.Lumen.HardwareRayTracing"), bRayTracing ? 1 : 0);

	// Upscaler.
	const bool bDlss = GGfxUpscaler.GetValueOnGameThread() == static_cast<int32>(EGfxUpscaler::Dlss) && IsDlssAvailable();
	ActiveUpscaler = bDlss ? EGfxUpscaler::Dlss : EGfxUpscaler::Tsr;
	GfxSetInt(TEXT("r.NGX.DLSS.Enable"), bDlss ? 1 : 0);
	GfxSetInt(TEXT("r.NGX.DLSS.DenoiserMode"), bDlss && bRayTracing && GGfxRayReconstruction.GetValueOnGameThread() != 0 ? 1 : 0);
	GfxSetInt(TEXT("r.Streamline.DLSSG.Enable"), bDlss && GGfxFrameGeneration.GetValueOnGameThread() != 0 ? 1 : 0);
	// TSR stays the anti-aliasing method; the DLSS plugin takes over from it when it is on.
	GfxSetInt(TEXT("r.AntiAliasingMethod"), 4);

	// Render resolution, through the scalability's resolution so the sky's Look.Quality keeps it.
	int32 Step = GGfxResolution.GetValueOnGameThread();
	if (Step < 0 || Step >= GfxResolutionStepCount)
	{
		Step = bDlss ? 1 : 0;
	}
	ActiveScreenPercent = GfxResolutionSteps[Step].Percent;
	Scalability::FQualityLevels Levels = Scalability::GetQualityLevels();
	if (!FMath::IsNearlyEqual(Levels.ResolutionQuality, ActiveScreenPercent, 0.05f))
	{
		Levels.ResolutionQuality = ActiveScreenPercent;
		Scalability::SetQualityLevels(Levels);
	}

	UE_LOG(LogGfxOptions, Display, TEXT("Graphics: ray tracing %s (hardware %s), %s at %.0f%%, frame generation %d, ray reconstruction %d"),
		bRayTracing ? TEXT("on") : TEXT("off"), IsRayTracingAvailable() ? TEXT("ready") : TEXT("not available"), bDlss ? TEXT("DLSS") : TEXT("TSR"),
		ActiveScreenPercent, GfxGetInt(TEXT("r.Streamline.DLSSG.Enable"), 0), GfxGetInt(TEXT("r.NGX.DLSS.DenoiserMode"), 0));
}

void UGraphicsOptionsSubsystem::DrawOptions(float Scale)
{
	bool bChanged = false;
	const ImVec4 Grey(0.7f, 0.7f, 0.7f, 1.0f);

	// Quality step (the sky's Look.Quality).
	int32 Quality = GfxGetInt(TEXT("Look.Quality"), 2);
	ImGui::TextUnformatted("Quality");
	ImGui::SameLine(110.0f * Scale * ColonyHud::TextGrow());
	bool bQuality = false;
	bQuality |= GfxRadio("Low##GfxQ", Quality, 0);
	ImGui::SameLine();
	bQuality |= GfxRadio("Medium##GfxQ", Quality, 1);
	ImGui::SameLine();
	bQuality |= GfxRadio("High##GfxQ", Quality, 2);
	ImGui::SameLine();
	bQuality |= GfxRadio("Epic##GfxQ", Quality, 3);
	if (bQuality)
	{
		GfxSetInt(TEXT("Look.Quality"), Quality);
		bChanged = true;
	}

	// Ray tracing.
	if (IsRayTracingAvailable())
	{
		bool bRayTracing = GGfxRayTracing.GetValueOnGameThread() != 0;
		if (ImGui::Checkbox("Ray-traced light##Gfx", &bRayTracing))
		{
			GGfxRayTracing.AsVariable()->Set(bRayTracing ? 1 : 0, ECVF_SetByCode);
			bChanged = true;
		}
		ImGui::SameLine();
		ImGui::TextColored(Grey, "%s", bRayTracing ? "(RTX cores: truer bounce light, reflections, soft shadows)" : "(off: Lumen's cheaper software light)");
	}
	else
	{
		ImGui::TextColored(Grey, "Ray-traced light: not available on this card or driver.");
	}

	// Upscaler.
	int32 Upscaler = GGfxUpscaler.GetValueOnGameThread();
	ImGui::TextUnformatted("Upscaler");
	ImGui::SameLine(110.0f * Scale * ColonyHud::TextGrow());
	bool bUpscaler = GfxRadio("TSR (Unreal)##Gfx", Upscaler, 0);
	if (IsDlssAvailable())
	{
		ImGui::SameLine();
		bUpscaler |= GfxRadio("DLSS (NVIDIA)##Gfx", Upscaler, 1);
	}
	else
	{
		ImGui::SameLine();
		ImGui::TextColored(Grey, "DLSS: plugin not installed");
	}
	if (bUpscaler)
	{
		GGfxUpscaler.AsVariable()->Set(Upscaler, ECVF_SetByCode);
		bChanged = true;
	}

	// Render resolution.
	int32 Step = GGfxResolution.GetValueOnGameThread();
	if (Step < 0 || Step >= GfxResolutionStepCount)
	{
		Step = ActiveUpscaler == EGfxUpscaler::Dlss ? 1 : 0;
	}
	ImGui::SetNextItemWidth(260.0f * Scale);
	if (ImGui::BeginCombo("Render resolution##Gfx", GfxResolutionSteps[Step].Name))
	{
		for (int32 Index = 0; Index < GfxResolutionStepCount; ++Index)
		{
			if (ImGui::Selectable(GfxResolutionSteps[Index].Name, Index == Step))
			{
				GGfxResolution.AsVariable()->Set(Index, ECVF_SetByCode);
				bChanged = true;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::TextColored(Grey, "Lower draws fewer pixels and lets the upscaler fill in: faster. Native with DLSS is DLAA, the sharpest.");

	// DLSS extras.
	if (ActiveUpscaler == EGfxUpscaler::Dlss)
	{
		if (GGfxRayTracing.GetValueOnGameThread() != 0 && IsRayTracingAvailable())
		{
			bool bReconstruct = GGfxRayReconstruction.GetValueOnGameThread() != 0;
			if (ImGui::Checkbox("Ray reconstruction##Gfx", &bReconstruct))
			{
				GGfxRayReconstruction.AsVariable()->Set(bReconstruct ? 1 : 0, ECVF_SetByCode);
				bChanged = true;
			}
			ImGui::SameLine();
			ImGui::TextColored(Grey, "(cleaner ray-traced light and reflections, a little slower)");
		}
		if (IsFrameGenerationAvailable())
		{
			bool bFrameGen = GGfxFrameGeneration.GetValueOnGameThread() != 0;
			if (ImGui::Checkbox("Frame generation##Gfx", &bFrameGen))
			{
				GGfxFrameGeneration.AsVariable()->Set(bFrameGen ? 1 : 0, ECVF_SetByCode);
				bChanged = true;
			}
			ImGui::SameLine();
			ImGui::TextColored(Grey, "(smoother motion, slightly more input lag)");
		}
	}

	if (ImGui::Button("Graphics defaults##Gfx"))
	{
		GfxSetInt(TEXT("Look.Quality"), 2);
		GGfxRayTracing.AsVariable()->Set(1, ECVF_SetByCode);
		GGfxUpscaler.AsVariable()->Set(0, ECVF_SetByCode);
		GGfxResolution.AsVariable()->Set(-1, ECVF_SetByCode);
		GGfxFrameGeneration.AsVariable()->Set(0, ECVF_SetByCode);
		GGfxRayReconstruction.AsVariable()->Set(0, ECVF_SetByCode);
		bChanged = true;
	}

	if (bChanged)
	{
		Save();
	}
}
