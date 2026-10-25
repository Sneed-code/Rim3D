using System.IO;
using UnrealBuildTool;

// Editor-only tools: commandlets that make generated assets (docs/design/looks.md).
public class Rim3DEditor : ModuleRules
{
	public Rim3DEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "UnrealEd", "AssetTools", "AssetRegistry",
			"MeshDescription", "SkeletalMeshDescription", "StaticMeshDescription", "RigLogicModule", "MaterialEditor", "ImageCore",
			"Rim3D",
		});
		// PawnLooks.h: names shared with the game module.
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "Rim3D"));
	}
}
