using UnrealBuildTool;

public class Rim3D : ModuleRules
{
	public Rim3D(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "ColonySim" });

		// Stage 0 tests (Stage0Test.cpp).
		PrivateDependencyModuleNames.AddRange(new string[] { "AIModule", "ProceduralMeshComponent", "Json", "RenderCore", "RHI" });
	}
}
