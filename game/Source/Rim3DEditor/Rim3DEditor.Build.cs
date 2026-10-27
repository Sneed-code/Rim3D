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
			"Json", "MeshDescription", "SkeletalMeshDescription", "StaticMeshDescription", "RigLogicModule", "MaterialEditor", "ImageCore", "RHI",
			"Rim3D",
		});
		// PawnFaces: faces from the MetaHuman Creator presets, and its grooms (an editor-only plugin in Rim3D.uproject).
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MetaHumanCharacter", "MetaHumanCharacterEditor", "MetaHumanCoreTech", "MetaHumanCoreTechLib", "HairStrandsCore", "Projects",
		});
		// PawnLooks.h: names shared with the game module.
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "Rim3D"));
	}
}
