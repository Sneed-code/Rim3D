using UnrealBuildTool;

public class Rim3D : ModuleRules
{
	public Rim3D(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "ColonySim" });

		// The bridge's speed keys (SimSubsystem.cpp).
		PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// The overseer's panels (OverseerSubsystem.cpp): Dear ImGui, plugin in game/Plugins/ImGui (assets/manifest.json).
		PrivateDependencyModuleNames.Add("ImGui");

		// Pawn animation (PawnAnimInstance.cpp): motion matching and the blend stack, all native.
		PrivateDependencyModuleNames.AddRange(new string[] { "AnimGraphRuntime", "BlendStack", "Chooser", "PoseSearch" });

		// Smoke tests (SmokeTest.cpp).
		PrivateDependencyModuleNames.AddRange(new string[] { "AIModule", "ProceduralMeshComponent", "Json", "RenderCore", "RHI" });
	}
}
